/**
 * @file word_emerge.c
 * @brief 段二 · 词涌现实现 —— 见 include/word_emerge.h
 *
 * 数据来源只有一处：字对计数表（charpair，字符串键）。
 * 因此每一次晋升都能倒查：凭哪两个字、多少次、PMI 多少、左右熵多少。
 * 不读任何词典、不做任何模糊匹配。
 */

#include "word_emerge.h"
#include "charpair.h"
#include "word_prob.h"
#include "multi_topology.h"
#include "huarong_topology.h"
#include "error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define WE_MAXENT 200000

typedef struct { char a[32]; char b[32]; unsigned int c; int used; } WEnt;
static WEnt* g_e = NULL;
static int    g_ne = 0;

static void collect_cb(const char* a, const char* b, uint32_t c, void* ud) {
    (void)ud;
    if (g_ne >= WE_MAXENT) return;
    snprintf(g_e[g_ne].a, 32, "%s", a);
    snprintf(g_e[g_ne].b, 32, "%s", b);
    g_e[g_ne].c = c;
    g_e[g_ne].used = 0;
    g_ne++;
}

/* token 判据（v0.6.12 中英双轨）：
 *   · CJK 单字（3 字节 U+4E00..U+9FFF）
 *   · 纯拉丁/数字 token（1..31 字节，小写）—— 英文同样走熵/PMI 判据
 * 用同一套公式，只是切分单位不同：中文按字，英文按词。 */
static int we_is_token(const char* s) {
    if (!s || !s[0]) return 0;
    size_t n = strlen(s);
    if (n == 3) {
        unsigned cp = (((unsigned char)s[0] & 0x0F) << 12) |
                      (((unsigned char)s[1] & 0x3F) << 6) |
                       ((unsigned char)s[2] & 0x3F);
        if (cp >= 0x4E00 && cp <= 0x9FFF) return 1;
    }
    if (n >= 1 && n <= 31) {
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)s[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return 0;
        }
        return 1;
    }
    return 0;
}

static int we_is_cjk1(const char* s) {
    if (!s || strlen(s) != 3) return 0;
    unsigned cp = (((unsigned char)s[0] & 0x0F) << 12) |
                  (((unsigned char)s[1] & 0x3F) << 6) |
                   ((unsigned char)s[2] & 0x3F);
    return cp >= 0x4E00 && cp <= 0x9FFF;
}

void word_emerge_thresholds(unsigned* min_count, double* min_pmi, double* min_ent, int* max_new) {
    const char* e;
    unsigned mc = 20; double mp = 2.5, me = 2.0; int mx = 50;
    if ((e = getenv("PIVOTMIND_EMERGE_MINCOUNT"))) mc = (unsigned)atoi(e);
    if ((e = getenv("PIVOTMIND_EMERGE_MINPMI")))    mp = atof(e);
    if ((e = getenv("PIVOTMIND_EMERGE_MINENT")))    me = atof(e);
    if ((e = getenv("PIVOTMIND_EMERGE_MAX")))       mx = atoi(e);
    if (min_count) *min_count = mc;
    if (min_pmi)   *min_pmi = mp;
    if (min_ent)   *min_ent = me;
    if (max_new)   *max_new = mx;
}

int word_emerge_tick(MasterTopology* master) {
    if (!master) return 0;
    CharPairTable* cp = cpt_global();
    if (!cp || cpt_pair_count(cp) == 0) return 0;

    SubTopology* vocab = NULL, *concept = NULL;
    for (int i = 0; i < master->sub_topo_count; i++) {
        SubTopology* s = master->sub_topologies[i];
        if (!s) continue;
        if (s->type == TOPO_VOCABULARY) vocab = s;
        if (s->type == TOPO_CONCEPT)    concept = s;
    }
    if (!vocab || !vocab->net || !concept || !concept->net) return 0;

    if (!g_e) {
        g_e = (WEnt*)calloc(WE_MAXENT, sizeof(WEnt));
        if (!g_e) return 0;
    }
    g_ne = 0;
    cpt_foreach(cp, collect_cb, NULL);
    if (g_ne < 2) return 0;

    unsigned mc; double mp, me; int mx;
    word_emerge_thresholds(&mc, &mp, &me, &mx);

    unsigned long long total = 0;
    for (int i = 0; i < g_ne; i++) total += g_e[i].c;
    if (!total) return 0;

    int created = 0;
    for (int i = 0; i < g_ne && created < mx; i++) {
        if (g_e[i].used) continue;
        const char* A = g_e[i].a;
        const char* B = g_e[i].b;
        if (!we_is_token(A) || !we_is_token(B)) continue;
        if (g_e[i].c < mc) continue;

        /* PMI 与左右熵统一走 word_prob（与生成侧同一套统计，口径只此一处） */
        double pmi = wp_pmi(A, B);
        if (pmi < mp) continue;
        /* ⚠ 这里用的是【字级】代理：A 的左熵 + B 的右熵。
         * CX-40 已证明它对切片词失效（「公吵」拿「公」的高左熵蒙混过关），
         * 但本函数跑在网关上、手上没有语料可回扫，词整体熵只能由离线的
         * qa_retrain + word_ctx 提供 ⇒ 在线路径暂时只能维持代理判据。
         * 别把这个写法抄到有语料的路径上去；那边一律走 word_ctx。 */
        double hl = wp_entropy(A, 0);     /* A 的左熵 */
        double hr = wp_entropy(B, 1);     /* B 的右熵 */
        double mn = hl < hr ? hl : hr;
        if (mn < me) continue;

        /* 组成字必须在词汇拓扑里（否则 cross-link 无从建立） */
        int aid = huarong_net_find_concept(vocab->net, A);
        int bid = huarong_net_find_concept(vocab->net, B);
        if (aid < 0 || bid < 0) continue;

        char word[96];
        if (we_is_cjk1(A) && we_is_cjk1(B))
            snprintf(word, sizeof(word), "%s%s", A, B);   /* 中文：拼成词 */
        else
            snprintf(word, sizeof(word), "%s %s", A, B);  /* 英文：保留空格（搭配） */
        if (huarong_net_find_concept(concept->net, word) >= 0) continue;   /* 已有 */

        ReasoningNode* wn = huarong_net_add_node(concept->net, word, NULL, 0);
        if (!wn) continue;
        node_hash_add(concept->node_hash, wn);

        /* cross-link：词 → 组成字（权重随 PMI 归一，0.30~0.90） */
        float w = (float)(0.30 + (pmi - mp) * 0.10);
        if (w > 0.90f) w = 0.90f;
        if (w < 0.30f) w = 0.30f;
        master_add_cross_link(master, TOPO_CONCEPT, wn->node_id, TOPO_VOCABULARY, aid, w, "emerge");
        master_add_cross_link(master, TOPO_CONCEPT, wn->node_id, TOPO_VOCABULARY, bid, w, "emerge");

        LOG_INFO("[词涌现] 新增「%s」次数=%u PMI=%.2f 左熵=%.2f 右熵=%.2f P=%.5f (成分=%s+%s w=%.2f)",
                 word, g_e[i].c, pmi, hl, hr, wp_p(word), A, B, (double)w);
        created++;
    }
    return created;
}
