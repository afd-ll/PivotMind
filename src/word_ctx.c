/**
 * @file word_ctx.c
 * @brief 词整体左右邻分布实现 —— 见 include/word_ctx.h
 *
 * 数据来源只有语料本身（charpair 是字级的，拿不出 (X | AB) 条件分布）。
 * 因此本模块有【构建期】与【读取期】两套路径：
 *   · 构建期：begin(候选) → scan_text(逐行) → finish()，结果追加进结果表
 *   · 读取期：load() → query()
 * 结果表落盘为 word_ctx.bin（PMWC v1），是派生量的**缓存**：
 * 语料或候选集变了就重跑工具，不存在第二真值源。
 *
 * 内存纪律（本仓踩过的坑，别再犯）：
 *   · 上限触发时**显式记账**（wctx_drop_stats），绝不静默截断
 *     —— v0.6.12 三个上限叠起来把词表砍掉 44% 且一条不报错。
 *   · 构建期按「批」处理：一批候选的全部邻接条目算完就落进结果表，
 *     下一次 begin 整体释放 ⇒ 峰值内存与【批大小】成正比，与语料规模无关。
 */

#include "word_ctx.h"
#include "error.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WCTX_MAGIC   0x43574D50u   /* "PMWC" */
#define WCTX_VERSION 1u

/* ============================ 内存结构 ============================ */

typedef struct {
    char               word[WCTX_KEY];
    unsigned long long count;
    double             hl, hr;
    unsigned           lbound, rbound;
    unsigned           left_n, right_n;
} WctxRec;

typedef struct {
    int          a, b;            /* 两个组成 token 的 id */
    unsigned long long count;
    unsigned     lbound, rbound;
    int          head[2];         /* 左/右邻链表头（CtxE 下标；-1 = 空） */
} BCand;

typedef struct {
    uint64_t key;                 /* (cand_id, side, nb_token_id) 打包 */
    uint32_t cnt;
    int32_t  next;
} CtxE;

static struct {
    /* ---- 结果表（构建与读取共用） ---- */
    WctxRec* rec;
    int      nrec, cap_rec;
    int*     rhash;               /* 开放寻址：词 → rec 下标；-1 空 */
    int      rhash_cap;

    /* ---- 构建期（每批整体重置） ---- */
    char (*tok)[WCTX_KEY];
    int      ntok, cap_tok;
    int*     thash;
    int      thash_cap;

    BCand*   cand;
    int      ncand, cap_cand;
    int*     chash;               /* (a,b) → cand 下标；-1 空 */
    int      chash_cap;

    CtxE*    ent;
    int      nent, cap_ent;
    int*     ehash;               /* key → ent 下标 + 1；0 空 */
    int      ehash_cap;

    char (*seg)[WCTX_KEY];

    unsigned long long drop[3];
    int      built;               /* 结果表已有内容（构建或装载过） */
} g;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

/* ============================ 小工具 ============================ */

static uint64_t _fnv(const char* s) {
    uint64_t h = 1469598103934665603ULL;
    for (const char* p = s; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;
    return h;
}

/* 整数键混洗（murmur3 finalizer）。
 * 🔴 必须混：本模块的键是位打包出来的 ——
 *   邻接键 = (候选id << 31) | (侧 << 30) | 邻居token id，
 *   候选键 = (首字id << 32) | 次字id。
 *   直接 `key & mask` 取的是**最低几位 = 邻居/次字 id**，于是「所有邻居相同的条目」
 *   全挤进同一个桶，探测链退化成 O(n) ⇒ 实测 907k 语料重训卡在 21KB/s（36 分钟没跑完一个批）。
 *   邻居字分布极度倾斜（的/了/是 到处都是），所以这不是理论风险，是必踩。 */
static uint64_t _mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static int _next_pow2(int n) {
    int c = 1;
    while (c < n) c <<= 1;
    return c;
}

/* 3 字节 UTF-8 且落在 U+4E00..U+9FFF ⇒ 1（与 charpair 的 token 规则同一判据） */
static int _is_cjk1(const char* s) {
    if (!s || strlen(s) != 3) return 0;
    unsigned cp = (((unsigned char)s[0] & 0x0F) << 12) |
                  (((unsigned char)s[1] & 0x3F) << 6) |
                   ((unsigned char)s[2] & 0x3F);
    return cp >= 0x4E00 && cp <= 0x9FFF;
}

/* 把词切成 token（规则同 charpair：CJK 单字 / 拉丁词 / 标点断链）。
 * out 最多收 2 个 token；返回 token 总数（可能 > 2，调用方据此拒收）。 */
static int _split_word(const char* w, char out[2][WCTX_KEY]) {
    int n = 0;
    const char* p = w;
    while (p && *p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x80) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_') {
                char one[WCTX_KEY];
                int m = 0;
                while (*p) {
                    unsigned char d = (unsigned char)*p;
                    if (!((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') ||
                          (d >= '0' && d <= '9') || d == '_')) break;
                    if (m < WCTX_KEY - 1) one[m++] = (char)((d >= 'A' && d <= 'Z') ? d + 32 : d);
                    p++;
                }
                one[m] = '\0';
                if (n < 2) snprintf(out[n], WCTX_KEY, "%s", one);
                n++;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { p++; continue; }
            p++;                      /* 标点：断链 */
            continue;
        }
        int len = (c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : ((c & 0xF8) == 0xF0 ? 4 : 1));
        if (len == 3) {
            unsigned int cp3 = ((unsigned int)(p[0] & 0x0F) << 12) |
                               ((unsigned int)(p[1] & 0x3F) << 6) |
                                (unsigned int)(p[2] & 0x3F);
            if (cp3 >= 0x4E00 && cp3 <= 0x9FFF) {
                if (n < 2) { memcpy(out[n], p, 3); out[n][3] = '\0'; }
                n++;
                p += 3;
                continue;
            }
        }
        p += len;
    }
    return n;
}

/* ============================ token 池 ============================ */

static int _tok_find(const char* s) {
    if (!g.thash) return -1;
    uint64_t mask = (uint64_t)(g.thash_cap - 1);
    uint64_t h = _fnv(s) & mask;
    for (int i = 0; i < g.thash_cap; i++) {
        int j = (int)((h + (uint64_t)i) & mask);
        if (g.thash[j] < 0) return -1;
        if (strcmp(g.tok[g.thash[j]], s) == 0) return g.thash[j];
    }
    return -1;
}

static void _tok_rehash(int cap) {
    int* nh = (int*)malloc((size_t)cap * sizeof(int));
    if (!nh) return;
    for (int i = 0; i < cap; i++) nh[i] = -1;
    uint64_t mask = (uint64_t)(cap - 1);
    for (int i = 0; i < g.ntok; i++) {
        uint64_t h = _fnv(g.tok[i]) & mask;
        for (int p = 0; p < cap; p++) {
            int j = (int)((h + (uint64_t)p) & mask);
            if (nh[j] < 0) { nh[j] = i; break; }
        }
    }
    free(g.thash);
    g.thash = nh;
    g.thash_cap = cap;
}

/* create=0 只查；create=1 查不到则新建。超上限返回 -1 并记账。 */
static int _tok_intern(const char* s, int create) {
    int i = _tok_find(s);
    if (i >= 0 || !create) return i;
    if (g.ntok >= g.cap_tok) { g.drop[0]++; return -1; }
    if ((g.ntok + 1) * 10 >= g.thash_cap * 7) _tok_rehash(g.thash_cap * 2);
    i = g.ntok++;
    snprintf(g.tok[i], WCTX_KEY, "%s", s);
    uint64_t mask = (uint64_t)(g.thash_cap - 1);
    uint64_t h = _fnv(s) & mask;
    for (int p = 0; p < g.thash_cap; p++) {
        int j = (int)((h + (uint64_t)p) & mask);
        if (g.thash[j] < 0) { g.thash[j] = i; break; }
    }
    return i;
}

/* ============================ 候选索引 ============================ */

static uint64_t _cand_key(int a, int b) { return ((uint64_t)(uint32_t)a << 32) | (uint32_t)b; }

static int _cand_head(uint64_t key) {
    uint64_t mask = (uint64_t)(g.chash_cap - 1);
    uint64_t h = _mix64(key) & mask;
    for (int i = 0; i < g.chash_cap; i++) {
        int j = (int)((h + (uint64_t)i) & mask);
        if (g.chash[j] < 0) return -1;
        int ci = g.chash[j];
        if (g.cand[ci].a == (int)(key >> 32) && g.cand[ci].b == (int)(uint32_t)key) return ci;
    }
    return -1;
}

/* ============================ 邻接条目 ============================ */

static int _ent_grow(void) {
    if (g.nent < g.cap_ent) return 1;
    int nc = g.cap_ent ? g.cap_ent * 2 : 65536;
    if (nc > WCTX_MAX_CTX) nc = WCTX_MAX_CTX;
    if (nc <= g.cap_ent) return 0;
    CtxE* ne = (CtxE*)realloc(g.ent, (size_t)nc * sizeof(CtxE));
    if (!ne) return 0;
    g.ent = ne;
    g.cap_ent = nc;
    return 1;
}

static void _ent_rehash(int cap) {
    int* nh = (int*)calloc((size_t)cap, sizeof(int));
    if (!nh) return;
    uint64_t mask = (uint64_t)(cap - 1);
    for (int i = 0; i < g.nent; i++) {
        uint64_t h = _mix64(g.ent[i].key) & mask;
        for (int p = 0; p < cap; p++) {
            int j = (int)((h + (uint64_t)p) & mask);
            if (nh[j] == 0) { nh[j] = i + 1; break; }
        }
    }
    free(g.ehash);
    g.ehash = nh;
    g.ehash_cap = cap;
}

/* 记一次邻接观测：cand[ci] 的 side 侧看到 nb 这个 token。 */
static void _ctx_add(int ci, int side, int nb) {
    static const unsigned long long SIDE_SHIFT = 30;
    uint64_t key = ((uint64_t)(uint32_t)ci << 31) | ((uint64_t)side << SIDE_SHIFT) | (uint64_t)(uint32_t)nb;

    if ((g.nent + 1) * 10 >= g.ehash_cap * 7) _ent_rehash(g.ehash_cap * 2);

    uint64_t mask = (uint64_t)(g.ehash_cap - 1);
    uint64_t h = _mix64(key) & mask;
    for (int p = 0; p < g.ehash_cap; p++) {
        int j = (int)((h + (uint64_t)p) & mask);
        if (g.ehash[j] == 0) {
            if (!_ent_grow()) { g.drop[0]++; return; }
            int e = g.nent++;
            g.ent[e].key = key;
            g.ent[e].cnt = 1;
            g.ent[e].next = g.cand[ci].head[side];
            g.cand[ci].head[side] = e;
            g.ehash[j] = e + 1;
            return;
        }
        int e = g.ehash[j] - 1;
        if (g.ent[e].key == key) { g.ent[e].cnt++; return; }
    }
    g.drop[0]++;   /* 表满（理论上不会到；真到了要看得见） */
}

/* ============================ 结果表 ============================ */

static int _rec_find(const char* word) {
    if (!g.rhash) return -1;
    uint64_t mask = (uint64_t)(g.rhash_cap - 1);
    uint64_t h = _fnv(word) & mask;
    for (int i = 0; i < g.rhash_cap; i++) {
        int j = (int)((h + (uint64_t)i) & mask);
        if (g.rhash[j] < 0) return -1;
        if (strcmp(g.rec[g.rhash[j]].word, word) == 0) return g.rhash[j];
    }
    return -1;
}

static void _rec_rehash(int cap) {
    int* nh = (int*)malloc((size_t)cap * sizeof(int));
    if (!nh) return;
    for (int i = 0; i < cap; i++) nh[i] = -1;
    uint64_t mask = (uint64_t)(cap - 1);
    for (int i = 0; i < g.nrec; i++) {
        uint64_t h = _fnv(g.rec[i].word) & mask;
        for (int p = 0; p < cap; p++) {
            int j = (int)((h + (uint64_t)p) & mask);
            if (nh[j] < 0) { nh[j] = i; break; }
        }
    }
    free(g.rhash);
    g.rhash = nh;
    g.rhash_cap = cap;
}

/* 追加一条结果；同名词已存在则跳过（跨批去重）。返回 1 = 新增。 */
static int _rec_add(const char* word, unsigned long long count, double hl, double hr,
                    unsigned lbound, unsigned rbound, unsigned ln, unsigned rn) {
    if (_rec_find(word) >= 0) return 0;
    if (!g.rhash) _rec_rehash(1024);
    if (g.nrec >= g.cap_rec) {
        int nc = g.cap_rec ? g.cap_rec * 2 : 4096;
        WctxRec* nr = (WctxRec*)realloc(g.rec, (size_t)nc * sizeof(WctxRec));
        if (!nr) return 0;
        g.rec = nr;
        g.cap_rec = nc;
    }
    if ((g.nrec + 1) * 10 >= g.rhash_cap * 7) _rec_rehash(g.rhash_cap * 2);

    WctxRec* r = &g.rec[g.nrec];
    memset(r, 0, sizeof(*r));
    snprintf(r->word, WCTX_KEY, "%s", word);
    r->count = count; r->hl = hl; r->hr = hr;
    r->lbound = lbound; r->rbound = rbound;
    r->left_n = ln; r->right_n = rn;

    uint64_t mask = (uint64_t)(g.rhash_cap - 1);
    uint64_t h = _fnv(word) & mask;
    for (int p = 0; p < g.rhash_cap; p++) {
        int j = (int)((h + (uint64_t)p) & mask);
        if (g.rhash[j] < 0) { g.rhash[j] = g.nrec; break; }
    }
    g.nrec++;
    g.built = 1;
    return 1;
}

/* ============================ 构建期 ============================ */

static void _build_free(void) {
    free(g.tok);    g.tok = NULL;    g.ntok = 0;  g.cap_tok = 0;
    free(g.thash);  g.thash = NULL;  g.thash_cap = 0;
    free(g.cand);   g.cand = NULL;   g.ncand = 0; g.cap_cand = 0;
    free(g.chash);  g.chash = NULL;  g.chash_cap = 0;
    free(g.ent);    g.ent = NULL;    g.nent = 0;  g.cap_ent = 0;
    free(g.ehash);  g.ehash = NULL;  g.ehash_cap = 0;
    free(g.seg);    g.seg = NULL;
}

int wctx_begin(const char* const* words, int n) {
    pthread_mutex_lock(&g_mu);
    _build_free();
    memset(g.drop, 0, sizeof(g.drop));
    if (!words || n <= 0) { pthread_mutex_unlock(&g_mu); return 0; }

    g.cap_tok = WCTX_MAX_TOKENS;
    g.tok = (char (*)[WCTX_KEY])calloc((size_t)g.cap_tok, WCTX_KEY);
    g.thash_cap = 1 << 17;
    g.thash = (int*)malloc((size_t)g.thash_cap * sizeof(int));
    g.cap_cand = (n > WCTX_MAX_CANDS) ? WCTX_MAX_CANDS : n;
    g.cand = (BCand*)calloc((size_t)g.cap_cand, sizeof(BCand));
    g.chash_cap = _next_pow2(g.cap_cand * 2 + 16);
    g.chash = (int*)malloc((size_t)g.chash_cap * sizeof(int));
    g.seg = (char (*)[WCTX_KEY])calloc(WCTX_SEG_MAX, WCTX_KEY);
    if (!g.tok || !g.thash || !g.cand || !g.chash || !g.seg) {
        _build_free();
        pthread_mutex_unlock(&g_mu);
        LOG_WARNING("[word_ctx] 构建期内存分配失败（候选 %d）", n);
        return 0;
    }
    for (int i = 0; i < g.thash_cap; i++) g.thash[i] = -1;
    for (int i = 0; i < g.chash_cap; i++) g.chash[i] = -1;

    /* 预留 id 0 = <边界>：段首/段尾的伪邻类。
     * 探针 word_ctx.py 把边界当成一个正常邻类参与熵，这里必须一致 ——
     * 否则「只在句首出现」的词会被算成熵 0 而误杀。 */
    g.ntok = 1;
    snprintf(g.tok[0], WCTX_KEY, "<边界>");
    {
        uint64_t mask = (uint64_t)(g.thash_cap - 1);
        uint64_t h = _fnv(g.tok[0]) & mask;
        for (int p = 0; p < g.thash_cap; p++) {
            int j = (int)((h + (uint64_t)p) & mask);
            if (g.thash[j] < 0) { g.thash[j] = 0; break; }
        }
    }

    int accepted = 0;
    for (int i = 0; i < n; i++) {
        if (!words[i] || !words[i][0]) continue;
        char two[2][WCTX_KEY];
        memset(two, 0, sizeof(two));
        int nt = _split_word(words[i], two);
        if (nt != 2) { g.drop[2]++; continue; }     /* 当前只支持 2-token 词 */
        if (accepted >= g.cap_cand) { g.drop[1]++; continue; }

        int a = _tok_intern(two[0], 1);
        int b = _tok_intern(two[1], 1);
        if (a < 0 || b < 0) { g.drop[1]++; continue; }

        uint64_t key = _cand_key(a, b);
        if (_cand_head(key) >= 0) continue;          /* 重复候选 */

        int ci = accepted++;
        g.cand[ci].a = a;
        g.cand[ci].b = b;
        g.cand[ci].head[0] = g.cand[ci].head[1] = -1;

        uint64_t mask = (uint64_t)(g.chash_cap - 1);
        uint64_t h = _mix64(key) & mask;
        for (int p = 0; p < g.chash_cap; p++) {
            int j = (int)((h + (uint64_t)p) & mask);
            if (g.chash[j] < 0) { g.chash[j] = ci; break; }
        }
    }
    g.ncand = accepted;

    g.ehash_cap = 1 << 16;
    g.ehash = (int*)calloc((size_t)g.ehash_cap, sizeof(int));

    pthread_mutex_unlock(&g_mu);
    return accepted;
}

/* 处理一段连续汉字：对每个相邻对 (A,B) 记左邻/右邻（含 <边界>）。返回命中数。
 * 边界**计入分布**（id 0），与探针 word_ctx.py 的 BORDER 一致。 */
static int _flush_segment(int n) {
    int hits = 0;
    for (int k = 0; k + 1 < n; k++) {
        int a = _tok_intern(g.seg[k], 0);
        int b = _tok_intern(g.seg[k + 1], 0);
        if (a <= 0 || b <= 0) continue;
        int ci = _cand_head(_cand_key(a, b));
        if (ci < 0) continue;

        g.cand[ci].count++;
        hits++;

        if (k == 0) {
            g.cand[ci].lbound++;
            _ctx_add(ci, 0, 0);                 /* 左邻 = <边界> */
        } else {
            int x = _tok_intern(g.seg[k - 1], 1);
            if (x >= 0) _ctx_add(ci, 0, x);
        }
        if (k + 2 >= n) {
            g.cand[ci].rbound++;
            _ctx_add(ci, 1, 0);                 /* 右邻 = <边界> */
        } else {
            int y = _tok_intern(g.seg[k + 2], 1);
            if (y >= 0) _ctx_add(ci, 1, y);
        }
    }
    return hits;
}

/* 扫描一行：只认汉字，其余（空白/标点/拉丁/emoji）一律断链。
 * 这是**故意**与探针的 HAN.findall(line) 等价 —— 见 word_ctx.h 口径段。 */
int wctx_scan_text(const char* text) {
    if (!text || !text[0]) return 0;
    pthread_mutex_lock(&g_mu);
    if (!g.seg || g.ncand <= 0 || !g.ehash) { pthread_mutex_unlock(&g_mu); return 0; }

    int n = 0, hits = 0;
    const char* p = text;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x80) {
            if (n) { hits += _flush_segment(n); n = 0; }
            p++;
            continue;
        }
        int len = (c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : ((c & 0xF8) == 0xF0 ? 4 : 1));
        if (len == 3) {
            unsigned int cp3 = ((unsigned int)(p[0] & 0x0F) << 12) |
                               ((unsigned int)(p[1] & 0x3F) << 6) |
                                (unsigned int)(p[2] & 0x3F);
            if (cp3 >= 0x4E00 && cp3 <= 0x9FFF) {
                if (n < WCTX_SEG_MAX) { memcpy(g.seg[n], p, 3); g.seg[n][3] = '\0'; n++; }
                p += 3;
                continue;
            }
        }
        if (n) { hits += _flush_segment(n); n = 0; }
        p += len;
    }
    if (n) hits += _flush_segment(n);

    pthread_mutex_unlock(&g_mu);
    return hits;
}

static double _entropy_of(int ci, int side, unsigned* distinct) {
    unsigned long long total = 0;
    int cnt = 0;
    for (int e = g.cand[ci].head[side]; e >= 0; e = g.ent[e].next) {
        total += g.ent[e].cnt;
        cnt++;
    }
    if (distinct) *distinct = (unsigned)cnt;
    if (!total) return 0.0;
    double h = 0.0;
    for (int e = g.cand[ci].head[side]; e >= 0; e = g.ent[e].next) {
        double pr = (double)g.ent[e].cnt / (double)total;
        if (pr > 0.0) h -= pr * log2(pr);
    }
    return h;
}

int wctx_finish(void) {
    pthread_mutex_lock(&g_mu);
    if (!g.cand || g.ncand <= 0) { pthread_mutex_unlock(&g_mu); return 0; }

    int added = 0;
    for (int ci = 0; ci < g.ncand; ci++) {
        if (g.cand[ci].count == 0) continue;
        unsigned ln = 0, rn = 0;
        double hl = _entropy_of(ci, 0, &ln);
        double hr = _entropy_of(ci, 1, &rn);

        /* 词面还原：两个 token 直接拼接（CJK）或空格拼接（拉丁搭配）。
         * 与 word_emerge 的命名规则同源，否则查询端对不上。 */
        const char* a = g.tok[g.cand[ci].a];
        const char* b = g.tok[g.cand[ci].b];
        char word[2 * WCTX_KEY];
        if (_is_cjk1(a) && _is_cjk1(b)) snprintf(word, sizeof(word), "%s%s", a, b);
        else                            snprintf(word, sizeof(word), "%s %s", a, b);

        added += _rec_add(word, g.cand[ci].count, hl, hr,
                          g.cand[ci].lbound, g.cand[ci].rbound, ln, rn);
    }
    pthread_mutex_unlock(&g_mu);
    return added;
}

/* ============================ 查询 / 持久化 ============================ */

int wctx_query(const char* word, WctxStat* out) {
    if (!word || !word[0]) return 0;
    pthread_mutex_lock(&g_mu);
    int i = _rec_find(word);
    if (i >= 0 && out) {
        out->count   = g.rec[i].count;
        out->hl      = g.rec[i].hl;
        out->hr      = g.rec[i].hr;
        out->lbound  = g.rec[i].lbound;
        out->rbound  = g.rec[i].rbound;
        out->left_n  = g.rec[i].left_n;
        out->right_n = g.rec[i].right_n;
    }
    pthread_mutex_unlock(&g_mu);
    return i >= 0;
}

int wctx_size(void) {
    pthread_mutex_lock(&g_mu);
    int n = g.nrec;
    pthread_mutex_unlock(&g_mu);
    return n;
}

int wctx_ready(void) {
    pthread_mutex_lock(&g_mu);
    int r = g.built;
    pthread_mutex_unlock(&g_mu);
    return r;
}

void wctx_reset(void) {
    pthread_mutex_lock(&g_mu);
    _build_free();
    free(g.rec);   g.rec = NULL;   g.nrec = 0; g.cap_rec = 0;
    free(g.rhash); g.rhash = NULL; g.rhash_cap = 0;
    g.built = 0;
    memset(g.drop, 0, sizeof(g.drop));
    pthread_mutex_unlock(&g_mu);
}

void wctx_drop_stats(unsigned long long out[3]) {
    if (!out) return;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < 3; i++) out[i] = g.drop[i];
    pthread_mutex_unlock(&g_mu);
}

int wctx_save(const char* path) {
    if (!path) return -1;
    pthread_mutex_lock(&g_mu);
    FILE* fp = fopen(path, "wb");
    if (!fp) { pthread_mutex_unlock(&g_mu); return -1; }

    uint32_t hdr[3] = { WCTX_MAGIC, WCTX_VERSION, (uint32_t)g.nrec };
    int ok = (fwrite(hdr, sizeof(uint32_t), 3, fp) == 3);
    for (int i = 0; ok && i < g.nrec; i++) {
        unsigned char len = (unsigned char)strlen(g.rec[i].word);
        float hl = (float)g.rec[i].hl, hr = (float)g.rec[i].hr;
        uint32_t c   = (uint32_t)g.rec[i].count;
        uint32_t lb  = g.rec[i].lbound, rb = g.rec[i].rbound;
        uint32_t ln  = g.rec[i].left_n, rn = g.rec[i].right_n;
        if (fwrite(&len, 1, 1, fp) != 1 ||
            fwrite(g.rec[i].word, 1, len, fp) != len ||
            fwrite(&c, 4, 1, fp) != 1 ||
            fwrite(&hl, 4, 1, fp) != 1 ||
            fwrite(&hr, 4, 1, fp) != 1 ||
            fwrite(&lb, 4, 1, fp) != 1 ||
            fwrite(&rb, 4, 1, fp) != 1 ||
            fwrite(&ln, 4, 1, fp) != 1 ||
            fwrite(&rn, 4, 1, fp) != 1) ok = 0;
    }
    fclose(fp);
    pthread_mutex_unlock(&g_mu);
    return ok ? 0 : -1;
}

int wctx_load(const char* path) {
    if (!path) return -1;
    FILE* fp = fopen(path, "rb");
    if (!fp) return -1;                  /* 不存在 = 没测过，不是错误 */

    uint32_t hdr[3] = {0};
    if (fread(hdr, sizeof(uint32_t), 3, fp) != 3 || hdr[0] != WCTX_MAGIC || hdr[1] != WCTX_VERSION) {
        fclose(fp);
        LOG_WARNING("[word_ctx] %s 头无效（旧格式或损坏），忽略", path);
        return -1;
    }

    pthread_mutex_lock(&g_mu);
    _build_free();
    free(g.rec);   g.rec = NULL;   g.nrec = 0; g.cap_rec = 0;
    free(g.rhash); g.rhash = NULL; g.rhash_cap = 0;
    g.built = 0;

    uint32_t n = hdr[2];
    for (uint32_t i = 0; i < n; i++) {
        unsigned char len = 0;
        char word[WCTX_KEY];
        uint32_t c = 0, lb = 0, rb = 0, ln = 0, rn = 0;
        float hl = 0, hr = 0;
        if (fread(&len, 1, 1, fp) != 1 || len == 0 || len >= WCTX_KEY) break;
        if (fread(word, 1, len, fp) != len) break;
        word[len] = '\0';
        if (fread(&c, 4, 1, fp) != 1 || fread(&hl, 4, 1, fp) != 1 || fread(&hr, 4, 1, fp) != 1 ||
            fread(&lb, 4, 1, fp) != 1 || fread(&rb, 4, 1, fp) != 1 ||
            fread(&ln, 4, 1, fp) != 1 || fread(&rn, 4, 1, fp) != 1) break;
        _rec_add(word, c, (double)hl, (double)hr, lb, rb, ln, rn);
    }
    int loaded = g.nrec;
    pthread_mutex_unlock(&g_mu);
    fclose(fp);
    LOG_INFO("[word_ctx] 已装载 %d 条词级统计（%s，PMWC v%u）", loaded, path, WCTX_VERSION);
    return 0;
}
