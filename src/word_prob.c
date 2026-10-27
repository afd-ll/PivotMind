/**
 * @file word_prob.c
 * @brief 词概率/左右熵/PMI 实现 —— 见 include/word_prob.h
 *
 * 数据来源只有一处：token 对计数表（charpair，字符串键，已落盘）。
 * 本模块持有的是【派生缓存】：重启后从表重建，不写任何文件。
 */

#include "word_prob.h"
#include "charpair.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define WP_TOK_MAX   65536      /* 不同 token 上限 */
#define WP_PAIR_MAX  600000     /* 参与构建的 token 对上限 */
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
typedef struct { char a[WP_KEY], b[WP_KEY]; unsigned int c; } WPair;
static WPair* g_pairs = NULL;
static int    g_npairs = 0;

static void _collect(const char* a, const char* b, uint32_t c, void* ud) {
    /* ud = 写入游标（&g_npairs）——真用它，不做 (void) 丢弃：
     * 接线门禁判据 B 会把「显式丢弃的形参」计成断链。 */
    int* np = (int*)ud;
    if (!np || *np < 0 || *np >= WP_PAIR_MAX) return;
    snprintf(g_pairs[*np].a, WP_KEY, "%s", a);
    snprintf(g_pairs[*np].b, WP_KEY, "%s", b);
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

/* ---------------- 查询 ---------------- */

static int _find_locked(const char* tok) { return (tok && tok[0]) ? _map_get(tok) : -1; }

unsigned long long wp_count(const char* tok) {
    wp_ensure();
    pthread_rwlock_rdlock(&g_lock);
    int i = _find_locked(tok);
    unsigned long long c = (i >= 0) ? (g.tok[i].out_sum + g.tok[i].in_sum) / 2 : 0;
    pthread_rwlock_unlock(&g_lock);
    return c;
}

double wp_p(const char* tok) {
    wp_ensure();
    pthread_rwlock_rdlock(&g_lock);
    int i = _find_locked(tok);
    double p = 0.0;
    if (i >= 0 && g.total)
        p = (double)((g.tok[i].out_sum + g.tok[i].in_sum) / 2) / (double)g.total;
    pthread_rwlock_unlock(&g_lock);
    return p;
}

float wp_pnorm(const char* tok) {
    wp_ensure();
    pthread_rwlock_rdlock(&g_lock);
    int i = _find_locked(tok);
    float r = 0.0f;
    if (i >= 0 && g.max_cnt > 0) {
        unsigned long long c = (g.tok[i].out_sum + g.tok[i].in_sum) / 2;
        r = (float)(log10(1.0 + (double)c) / log10(1.0 + (double)g.max_cnt));
        if (r < 0.0f) r = 0.0f;
        if (r > 1.0f) r = 1.0f;
    }
    pthread_rwlock_unlock(&g_lock);
    return r;
}

/* side: 0=左熵（左边 token 分布）, 1=右熵 */
double wp_entropy(const char* tok, int side) {
    wp_ensure();
    pthread_rwlock_rdlock(&g_lock);
    int i = _find_locked(tok);
    double h = 0.0;
    if (i >= 0) {
        int off = side ? g.tok[i].right_off : g.tok[i].left_off;
        int n   = side ? g.tok[i].right_n   : g.tok[i].left_n;
        unsigned long long t = 0;
        for (int k = 0; k < n; k++) t += g.nb[off + k].cnt;
        if (t > 0) {
            for (int k = 0; k < n; k++) {
                double p = (double)g.nb[off + k].cnt / (double)t;
                if (p > 0.0) h -= p * log2(p);
            }
        }
    }
    pthread_rwlock_unlock(&g_lock);
    return h;
}

double wp_min_entropy(const char* tok) {
    double hl = wp_entropy(tok, 0);
    double hr = wp_entropy(tok, 1);
    return hl < hr ? hl : hr;
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

/* CJK 码点数（只看 3 字节 UTF-8 的 U+4E00..U+9FFF） */
static int _cjk_len(const char* s) {
    int n = 0;
    for (const unsigned char* p = (const unsigned char*)s; p && *p; ) {
        if ((p[0] & 0xF0) == 0xE0) {
            unsigned int cp = ((unsigned int)(p[0] & 0x0F) << 12)
                            | ((unsigned int)(p[1] & 0x3F) << 6)
                            |  (unsigned int)(p[2] & 0x3F);
            if (cp >= 0x4E00 && cp <= 0x9FFF) n++;
            p += 3;
        } else if ((p[0] & 0xE0) == 0xC0) p += 2;
        else if ((p[0] & 0xF8) == 0xF0) p += 4;
        else p++;
    }
    return n;
}

float wp_gain(const char* tok) {
    if (!tok || !tok[0]) return 1.0f;
    /* 单字不奖不罚：中文单字是合法单位，用熵去罚会把正常单字全压死 */
    if (_cjk_len(tok) == 1 && tok[3] == '\0') return 1.0f;
    if (!wp_ensure()) return 1.0f;
    pthread_rwlock_rdlock(&g_lock);
    int i = _find_locked(tok);
    float gain = 1.0f;
    if (i >= 0) {
        unsigned long long c = (g.tok[i].out_sum + g.tok[i].in_sum) / 2;
        if (c < 2) {
            gain = 0.70f;                     /* 只见过 1 次：不敢用 */
        } else {
            /* 概率项：对数压缩，避免头部长词通吃（max 在构建时预存） */
            float pn = (g.max_cnt > 0)
                     ? (float)(log10(1.0 + (double)c) / log10(1.0 + (double)g.max_cnt))
                     : 0.0f;
            /* 熵项：min(HL,HR) 归一到 [0,1]（4 bit 以上视为搭配自由） */
            unsigned long long tl = 0, tr = 0;
            for (int k = 0; k < g.tok[i].left_n; k++)  tl += g.nb[g.tok[i].left_off  + k].cnt;
            for (int k = 0; k < g.tok[i].right_n; k++) tr += g.nb[g.tok[i].right_off + k].cnt;
            double hl = 0, hr = 0;
            if (tl) for (int k = 0; k < g.tok[i].left_n; k++) {
                double p = (double)g.nb[g.tok[i].left_off + k].cnt / (double)tl;
                if (p > 0) hl -= p * log2(p);
            }
            if (tr) for (int k = 0; k < g.tok[i].right_n; k++) {
                double p = (double)g.nb[g.tok[i].right_off + k].cnt / (double)tr;
                if (p > 0) hr -= p * log2(p);
            }
            double mn = hl < hr ? hl : hr;
            float en = (float)(mn > 4.0 ? 1.0 : mn / 4.0);
            gain = 0.55f + 0.45f * pn + 0.20f * en;
        }
    }
    pthread_rwlock_unlock(&g_lock);
    if (gain < WP_GAIN_MIN) gain = WP_GAIN_MIN;
    if (gain > WP_GAIN_MAX) gain = WP_GAIN_MAX;
    return gain;
}

int wp_is_wordlike(const char* tok, double min_ent, unsigned long long min_count) {
    if (!tok || !tok[0]) return 0;
    int cjk = _cjk_len(tok);
    unsigned long long c = wp_count(tok);
    if (cjk == 1 && tok[3] == '\0') return 1;          /* 单字豁免 */
    if (c < min_count) return 0;
    return wp_min_entropy(tok) >= min_ent;
}
