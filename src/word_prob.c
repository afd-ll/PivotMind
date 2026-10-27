/**
 * @file word_prob.c
 * @brief 词概率/左右熵/PMI 实现 —— 见 include/word_prob.h
 *
 * 数据来源只有一处：token 对计数表（charpair，字符串键，已落盘）。
 * 本模块持有的是【派生缓存】：重启后从表重建，不写任何文件。
 */

#include "word_prob.h"
#include "charpair.h"
#include "word_ctx.h"
#include "pivotmind_paths.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define WP_TOK_MAX   65536      /* 不同 token 上限 */
#define WP_PAIR_MAX  1200000    /* 参与构建的 token 对上限（实测 91.9 万对，600k 会抽样掉 1/3） */
#define WP_KEY       32
#define WP_REBUILD_SEC 30

#define WP_GAIN_MIN  0.45f
#define WP_GAIN_MAX  1.25f

typedef struct {
    char tok[WP_KEY];
    unsigned long long out_sum;   /* 作为左成员的计数和 */
    unsigned long long in_sum;    /* 作为右成员的计数和 */
    int  left_n, right_n;
    int  left_off, right_off;
} WTok;

typedef struct { int idx; unsigned int cnt; } WNb;

static struct {
    WTok* tok;
    int   ntok, cap;
    WNb*  nb;                 /* 左邻 + 右邻 合并区（CSR） */
    int   nb_n, nb_cap;
    int*  map;                /* token → idx（开放寻址，-1 空） */
    int   map_cap;
    unsigned long long total; /* 对计数总和 */
    unsigned long long max_cnt; /* 单 token 最大次数（构建时预存，供对数压缩） */
    int   pairs;
    int   last_pairs;
    time_t last_ts;
    int   ready;
} g;

static pthread_rwlock_t g_lock = PTHREAD_RWLOCK_INITIALIZER;

/* ---------------- 小工具 ---------------- */

static unsigned int _hash(const char* s) {
    unsigned int h = 2166136261u;
    for (const char* p = s; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
    return h;
}

static int _map_get(const char* tok) {
    if (!g.map) return -1;
    unsigned int h = _hash(tok) & (unsigned int)(g.map_cap - 1);
    for (int i = 0; i < g.map_cap; i++) {
        int j = (int)((h + (unsigned int)i) & (unsigned int)(g.map_cap - 1));
        if (g.map[j] < 0) return -1;
        if (strcmp(g.tok[g.map[j]].tok, tok) == 0) return g.map[j];
    }
    return -1;
}

static void _map_put(int idx) {
    unsigned int h = _hash(g.tok[idx].tok) & (unsigned int)(g.map_cap - 1);
    for (int i = 0; i < g.map_cap; i++) {
        int j = (int)((h + (unsigned int)i) & (unsigned int)(g.map_cap - 1));
        if (g.map[j] < 0) { g.map[j] = idx; return; }
        if (g.map[j] == idx) return;
    }
}

static int _intern(const char* tok) {
    int idx = _map_get(tok);
    if (idx >= 0) return idx;
    if (g.ntok >= g.cap) return -1;
    idx = g.ntok++;
    snprintf(g.tok[idx].tok, WP_KEY, "%s", tok);
    _map_put(idx);
    return idx;
}

/* 收集用临时缓冲 */
typedef struct { char a[18], b[18]; unsigned int c; } WPair;   /* CJK 3 字节 / 拉丁 ≤17，够用且省内存 */
static WPair* g_pairs = NULL;
static int    g_npairs = 0;

static void _collect(const char* a, const char* b, uint32_t c, void* ud) {
    /* ud = 写入游标（&g_npairs）——真用它，不做 (void) 丢弃：
     * 接线门禁判据 B 会把「显式丢弃的形参」计成断链。 */
    int* np = (int*)ud;
    if (!np || *np < 0 || *np >= WP_PAIR_MAX) return;
    snprintf(g_pairs[*np].a, sizeof(g_pairs[0].a), "%s", a);
    snprintf(g_pairs[*np].b, sizeof(g_pairs[0].b), "%s", b);
    g_pairs[*np].c = c;
    (*np)++;
}

/* ---------------- 构建 ---------------- */

static int _rebuild_locked(void) {
    CharPairTable* cp = cpt_global();
    if (!cp) return 0;
    if (!g_pairs) {
        g_pairs = (WPair*)calloc(WP_PAIR_MAX, sizeof(WPair));
        if (!g_pairs) return 0;
    }
    g_npairs = 0;
    cpt_foreach(cp, _collect, &g_npairs);
    if (g_npairs <= 0) { g.ready = 0; return 0; }

    /* token 表：先按经验容量分配（不同 token 通常远小于对数） */
    if (!g.tok) {
        g.cap = WP_TOK_MAX;
        g.tok = (WTok*)calloc((size_t)g.cap, sizeof(WTok));
        if (!g.tok) return 0;
    }
    memset(g.tok, 0, (size_t)g.cap * sizeof(WTok));
    g.ntok = 0;

    int mcap = 1;
    while (mcap < g.cap * 2) mcap <<= 1;
    if (!g.map) g.map = (int*)malloc((size_t)mcap * sizeof(int));
    if (!g.map) return 0;
    g.map_cap = mcap;
    for (int i = 0; i < mcap; i++) g.map[i] = -1;

    g.total = 0;
    /* 第一趟：intern + 计数 + 邻接度数 */
    for (int i = 0; i < g_npairs; i++) {
        int ia = _intern(g_pairs[i].a);
        int ib = _intern(g_pairs[i].b);
        if (ia < 0 || ib < 0) continue;
        g.tok[ia].out_sum += g_pairs[i].c;
        g.tok[ib].in_sum  += g_pairs[i].c;
        g.tok[ia].right_n++;
        g.tok[ib].left_n++;
        g.total += g_pairs[i].c;
    }
    if (!g.total) { g.ready = 0; return 0; }

    /* CSR 偏移 */
    int off = 0;
    for (int i = 0; i < g.ntok; i++) {
        g.tok[i].left_off  = off; off += g.tok[i].left_n;
        g.tok[i].right_off = off; off += g.tok[i].right_n;
    }
    if (g.nb_cap < off) {
        WNb* nn = (WNb*)realloc(g.nb, (size_t)off * sizeof(WNb));
        if (!nn) return 0;
        g.nb = nn;
        g.nb_cap = off;
    }
    g.nb_n = off;
    /* 第二趟：填邻接（用临时游标） */
    int* cur_l = (int*)malloc((size_t)g.ntok * sizeof(int));
    int* cur_r = (int*)malloc((size_t)g.ntok * sizeof(int));
    if (!cur_l || !cur_r) { free(cur_l); free(cur_r); return 0; }
    for (int i = 0; i < g.ntok; i++) { cur_l[i] = g.tok[i].left_off; cur_r[i] = g.tok[i].right_off; }
    for (int i = 0; i < g_npairs; i++) {
        int ia = _map_get(g_pairs[i].a);
        int ib = _map_get(g_pairs[i].b);
        if (ia < 0 || ib < 0) continue;
        if (cur_r[ia] < g.tok[ia].right_off + g.tok[ia].right_n) {
            g.nb[cur_r[ia]].idx = ib; g.nb[cur_r[ia]].cnt = g_pairs[i].c; cur_r[ia]++;
        }
        if (cur_l[ib] < g.tok[ib].left_off + g.tok[ib].left_n) {
            g.nb[cur_l[ib]].idx = ia; g.nb[cur_l[ib]].cnt = g_pairs[i].c; cur_l[ib]++;
        }
    }
    free(cur_l); free(cur_r);

    g.max_cnt = 1;
    for (int i = 0; i < g.ntok; i++) {
        unsigned long long c = (g.tok[i].out_sum + g.tok[i].in_sum) / 2;
        if (c > g.max_cnt) g.max_cnt = c;
    }
    g.pairs = g_npairs;
    g.last_pairs = cpt_pair_count(cp);
    g.last_ts = time(NULL);
    g.ready = 1;
    return 1;
}

int wp_ensure(void) {
    CharPairTable* cp = cpt_global();
    if (!cp) return 0;
    int pc = cpt_pair_count(cp);
    time_t now = time(NULL);
    if (g.ready && pc == g.last_pairs && (now - g.last_ts) < WP_REBUILD_SEC) return 1;
    pthread_rwlock_wrlock(&g_lock);
    int ok = 0;
    /* 双检：可能已被别的线程重建 */
    if (!(g.ready && pc == g.last_pairs && (now - g.last_ts) < WP_REBUILD_SEC))
        ok = _rebuild_locked();
    else ok = 1;
    pthread_rwlock_unlock(&g_lock);
    return ok;
}

void wp_invalidate(void) {
    pthread_rwlock_wrlock(&g_lock);
    g.ready = 0;
    g.last_pairs = -1;
    pthread_rwlock_unlock(&g_lock);
}

void wp_stats(int* tokens, int* pairs, unsigned long long* total) {
    wp_ensure();
    pthread_rwlock_rdlock(&g_lock);
    if (tokens) *tokens = g.ntok;
    if (pairs)  *pairs  = g.pairs;
    if (total)  *total  = g.total;
    pthread_rwlock_unlock(&g_lock);
}

/* ---------------- 查询（含中文多字词的字对合成） ---------------- */

static int _find_locked(const char* tok) { return (tok && tok[0]) ? _map_get(tok) : -1; }

/* 纯 CJK 切分：返回汉字数；first/last 取首末字（各 4 字节缓冲，可传 NULL）。
 * 非纯 CJK（含拉丁/标点）返回 -1。 */
static int _cjk_split(const char* w, char* first, char* last) {
    int n = 0;
    const char* p = w;
    while (p && *p) {
        unsigned char c = (unsigned char)*p;
        int len = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
        if (len != 3) return -1;
        unsigned int cp = ((unsigned int)(p[0] & 0x0F) << 12)
                        | ((unsigned int)(p[1] & 0x3F) << 6)
                        |  (unsigned int)(p[2] & 0x3F);
        if (cp < 0x4E00 || cp > 0x9FFF) return -1;
        if (n == 0 && first) { memcpy(first, p, 3); first[3] = 0; }
        if (last) { memcpy(last, p, 3); last[3] = 0; }
        n++;
        p += 3;
    }
    return n;
}

typedef struct {
    unsigned long long count;
    double hl, hr;
    int    known;     /* 1 = 有统计 */
    int    nchar;     /* CJK 字数；非纯 CJK = -1 */
} WInfo;

/* 统计解析（**字级口径**）：token 直接命中优先；否则若是纯 CJK 多字词，只合成
 *   计数 = 内部相邻字对的最小计数（最弱环节）
 * 熵【不再合成】——见下方 _wctx_fill 与 CX-40。
 *
 * 为什么必须合成计数：中文 token 是【单字】，词不在表里，不合成的话
 * 「天气/学习」这类词概率恒 0、生成增益恒 1（等于没接）。
 *
 * 为什么熵不能合成：charpair 存的是 (X,A)、(A,B)、(B,Y) 三条**独立**计数，
 * 拿不出条件分布 (X | A 后接 B)。用「首字左熵 + 末字右熵」冒充，会把切片词
 * 唯一的可识别特征（组成字自己用得很广、但词整体左邻唯一）正好抹掉：
 * 实测「公吵」左熵报 6.58（=「公」的左熵），真值 0.00（左邻 100% 是「老」）。
 * ⇒ 词级熵只认 word_ctx 里的语料实测值；没测过就是【未知】(0)，
 *   绝不拿首末字冒充。 */
static void _resolve_locked(const char* tok, WInfo* out) {
    memset(out, 0, sizeof(*out));
    out->nchar = _cjk_split(tok, NULL, NULL);
    int i = _find_locked(tok);
    if (i >= 0) {
        out->count = (g.tok[i].out_sum + g.tok[i].in_sum) / 2;
        out->known = 1;
        unsigned long long tl = 0, tr = 0;
        for (int k = 0; k < g.tok[i].left_n; k++)  tl += g.nb[g.tok[i].left_off  + k].cnt;
        for (int k = 0; k < g.tok[i].right_n; k++) tr += g.nb[g.tok[i].right_off + k].cnt;
        if (tl) for (int k = 0; k < g.tok[i].left_n; k++) {
            double pr = (double)g.nb[g.tok[i].left_off + k].cnt / (double)tl;
            if (pr > 0) out->hl -= pr * log2(pr);
        }
        if (tr) for (int k = 0; k < g.tok[i].right_n; k++) {
            double pr = (double)g.nb[g.tok[i].right_off + k].cnt / (double)tr;
            if (pr > 0) out->hr -= pr * log2(pr);
        }
        return;
    }
    if (out->nchar < 2) return;

    CharPairTable* cp = cpt_global();
    if (!cp) return;

    unsigned long long mn = 0;
    char prev[4] = {0};
    const char* p = tok;
    int got = 1;
    while (*p) {
        char cur[4] = {0};
        memcpy(cur, p, 3);
        if (prev[0]) {
            uint32_t c = cpt_get(cp, prev, cur);
            if (!c) { got = 0; break; }
            if (mn == 0 || c < mn) mn = c;
        }
        memcpy(prev, cur, 4);
        p += 3;
    }
    if (!got || !mn) return;

    /* 只合成计数；熵保持 0（未知）。词级实测见 _wctx_fill。 */
    out->count = mn;
    out->known = 1;
}

/* 词级实测（CX-40）：只对「纯 CJK 多字词」有定义。
 * 单字与拉丁词本身就是 token，charpair 给的是精确值，不需要也不该走词级口径。
 * 命中返回 1（count/hl/hr 全部来自语料实测）；未测量返回 0（调用方回落字级）。
 * ⚠ 本函数在 g_lock **之外**调用：word_ctx 有自己的锁，嵌套会引入锁序风险。 */
static pthread_once_t g_wctx_once = PTHREAD_ONCE_INIT;

/* 懒加载 word_ctx.bin：第一次用到多字词时才读。文件不存在 ⇒ 空表，
 * 多字词熵一律「未知」（宁缺毋滥），绝不影响启动。
 * 同进程内若已经构建过（qa_retrain 就是），不覆盖 —— 否则工具会把自己的实测结果读没。 */
static void _wctx_load_once(void) {
    if (wctx_ready()) return;      /* 同进程内已构建（qa_retrain）⇒ 不覆盖 */
    const char* p = pm_file(PM_FILE_WORD_CTX);
    if (p) wctx_load(p);
}

static int _wctx_fill(const char* tok, WInfo* out) {
    memset(out, 0, sizeof(*out));
    out->nchar = _cjk_split(tok, NULL, NULL);
    if (out->nchar < 2) return 0;
    pthread_once(&g_wctx_once, _wctx_load_once);
    WctxStat st;
    if (!wctx_query(tok, &st)) return 0;
    out->count = st.count;
    out->hl = st.hl;
    out->hr = st.hr;
    out->known = 1;
    return 1;
}

/* 统一解析：先词级实测，再回落字级合成。 */
static void _resolve(const char* tok, WInfo* out) {
    if (_wctx_fill(tok, out)) return;
    pthread_rwlock_rdlock(&g_lock);
    _resolve_locked(tok, out);
    pthread_rwlock_unlock(&g_lock);
}

unsigned long long wp_count(const char* tok) {
    wp_ensure();
    WInfo wi; _resolve(tok, &wi);
    return wi.count;
}

double wp_p(const char* tok) {
    wp_ensure();
    WInfo wi; _resolve(tok, &wi);
    pthread_rwlock_rdlock(&g_lock);
    double total = (double)g.total;
    pthread_rwlock_unlock(&g_lock);
    return (wi.known && total > 0.0) ? (double)wi.count / total : 0.0;
}

float wp_pnorm(const char* tok) {
    wp_ensure();
    WInfo wi; _resolve(tok, &wi);
    pthread_rwlock_rdlock(&g_lock);
    double maxc = (double)g.max_cnt;
    pthread_rwlock_unlock(&g_lock);
    float r = 0.0f;
    if (wi.known && maxc > 0.0)
        r = (float)(log10(1.0 + (double)wi.count) / log10(1.0 + maxc));
    if (r < 0.0f) r = 0.0f;
    if (r > 1.0f) r = 1.0f;
    return r;
}

double wp_entropy(const char* tok, int side) {
    wp_ensure();
    WInfo wi; _resolve(tok, &wi);
    return side ? wi.hr : wi.hl;
}

double wp_min_entropy(const char* tok) {
    wp_ensure();
    WInfo wi; _resolve(tok, &wi);
    return wi.hl < wi.hr ? wi.hl : wi.hr;
}

double wp_pmi(const char* tok_a, const char* tok_b) {
    if (!tok_a || !tok_b) return 0.0;
    CharPairTable* cp = cpt_global();
    if (!cp) return 0.0;
    uint32_t cab = cpt_get(cp, tok_a, tok_b);
    if (!cab) return 0.0;
    wp_ensure();
    pthread_rwlock_rdlock(&g_lock);
    int ia = _find_locked(tok_a), ib = _find_locked(tok_b);
    double r = 0.0;
    if (ia >= 0 && ib >= 0 && g.total > 0 && g.tok[ia].out_sum && g.tok[ib].out_sum) {
        double pab = (double)cab / (double)g.total;
        double pa  = (double)g.tok[ia].out_sum / (double)g.total;
        double pb  = (double)g.tok[ib].out_sum / (double)g.total;
        r = log2(pab / (pa * pb));
    }
    pthread_rwlock_unlock(&g_lock);
    return r;
}

float wp_gain(const char* tok) {
    if (!tok || !tok[0]) return 1.0f;
    char first[4] = {0};
    /* 单字不奖不罚：中文单字是合法单位，用熵去罚会把正常单字全压死 */
    if (_cjk_split(tok, first, NULL) == 1) return 1.0f;
    if (!wp_ensure()) return 1.0f;
    WInfo wi; _resolve(tok, &wi);
    pthread_rwlock_rdlock(&g_lock);
    double maxc = (double)g.max_cnt;
    pthread_rwlock_unlock(&g_lock);
    float gain = 1.0f;
    if (wi.known) {
        if (wi.count < 2) {
            gain = 0.70f;                       /* 只见过 1 次：不敢用 */
        } else {
            float pn = (maxc > 0.0)
                     ? (float)(log10(1.0 + (double)wi.count) / log10(1.0 + maxc))
                     : 0.0f;
            double mn = wi.hl < wi.hr ? wi.hl : wi.hr;
            float en = (float)(mn > 4.0 ? 1.0 : mn / 4.0);
            gain = 0.55f + 0.45f * pn + 0.20f * en;
        }
    }
    if (gain < WP_GAIN_MIN) gain = WP_GAIN_MIN;
    if (gain > WP_GAIN_MAX) gain = WP_GAIN_MAX;
    return gain;
}

/* ==================== 写侧结构闸门（CX-27） ====================
 * 线上实测（10-27）：vocab 22,758 节点里真词只占 42.9%，非法 UTF-8 2,039、
 * 纯符号 2,172、纯拉丁 5,650、中英混 4,146，且都有边。这些是【进库时】就没拦住。
 * 本闸门只判结构（不依赖统计），因此新词第一句话就能拦，不受冷启动影响。 */

static unsigned long long g_gate[5] = {0, 0, 0, 0, 0};

void wp_gate_stats(unsigned long long out[5]) {
    if (!out) return;
    for (int i = 0; i < 5; i++) out[i] = g_gate[i];
}

/* 扫描：汉字数 / 拉丁字母数 / 数字数 / 是否有实义字符 / 是否非法或损坏 */
typedef struct { int cjk, latin, digit, bad, repl, ctrl; } GateScan;

static GateScan _gate_scan(const char* w) {
    GateScan g;
    memset(&g, 0, sizeof(g));
    const unsigned char* p = (const unsigned char*)w;
    while (p && *p) {
        if (p[0] == 0xEF && p[1] == 0xBF && p[2] == 0xBD) { g.repl = 1; p += 3; continue; }
        if (p[0] < 0x20 || p[0] == 0x7F) { g.ctrl = 1; p++; continue; }
        if (p[0] < 0x80) {
            if ((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z')) g.latin++;
            else if (p[0] >= '0' && p[0] <= '9') g.digit++;
            p++;
            continue;
        }
        int len = (p[0] & 0xE0) == 0xC0 ? 2 : ((p[0] & 0xF0) == 0xE0 ? 3 : ((p[0] & 0xF8) == 0xF0 ? 4 : 0));
        if (len == 0) { g.bad = 1; p++; continue; }
        int ok = 1;
        for (int k = 1; k < len; k++) if ((p[k] & 0xC0) != 0x80) { ok = 0; break; }
        if (!ok) { g.bad = 1; p++; continue; }
        if (len == 3) {
            unsigned int cp = ((unsigned int)(p[0] & 0x0F) << 12)
                            | ((unsigned int)(p[1] & 0x3F) << 6)
                            |  (unsigned int)(p[2] & 0x3F);
            if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF)) g.cjk++;
        }
        p += len;
    }
    return g;
}

int wp_word_gate(const char* w, int* reason) {
    int r = 0;
    if (!w || !w[0]) {
        r = 2;
    } else {
        GateScan g = _gate_scan(w);
        if (g.bad || g.repl || g.ctrl)      r = 1;
        else if (g.cjk == 0 && g.latin == 0 && g.digit == 0) r = 2;
        else if (g.cjk > 6)                 r = 3;
        else if (strlen(w) >= WP_KEY - 1)   r = 4;
    }
    if (r) g_gate[r]++;
    if (reason) *reason = r;
    return r == 0;
}

int wp_text_has_content(const char* text) {
    if (!text || !text[0]) return 0;
    GateScan g = _gate_scan(text);
    if (g.bad || g.repl || g.ctrl) return 0;
    return (g.cjk > 0 || g.latin > 0 || g.digit > 0);
}

int wp_is_wordlike(const char* tok, double min_ent, unsigned long long min_count) {
    if (!tok || !tok[0]) return 0;
    char first[4] = {0};
    if (_cjk_split(tok, first, NULL) == 1) return 1;      /* 单字豁免 */
    unsigned long long c = wp_count(tok);
    if (c < min_count) return 0;
    return wp_min_entropy(tok) >= min_ent;
}
