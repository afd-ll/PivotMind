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
#include "multi_topology.h"
#include "huarong_topology.h"
#include "error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define WE_MAXENT 200000

typedef struct { char a[8]; char b[8]; unsigned int c; int used; } WEnt;
static WEnt* g_e = NULL;
static int    g_ne = 0;

static void collect_cb(const char* a, const char* b, uint32_t c, void* ud) {
    (void)ud;
    if (g_ne >= WE_MAXENT) return;
    snprintf(g_e[g_ne].a, 8, "%s", a);
    snprintf(g_e[g_ne].b, 8, "%s", b);
    g_e[g_ne].c = c;
    g_e[g_ne].used = 0;
    g_ne++;
}

static int is_cjk_char3(const char* s) {
    if (!s) return 0;
    if (strlen(s) != 3) return 0;
    unsigned cp = (((unsigned char)s[0] & 0x0F) << 12) |
                  (((unsigned char)s[1] & 0x3F) << 6) |
                   ((unsigned char)s[2] & 0x3F);
    return cp >= 0x4E00 && cp <= 0x9FFF;
}

/* out(X) = X 作为首字的计数和（只算中文单字对，与写入口径一致） */
static unsigned long long out_sum(const char* X) {
    unsigned long long t = 0;
    for (int i = 0; i < g_ne; i++)
        if (g_e[i].c && is_cjk_char3(g_e[i].b) && strcmp(g_e[i].a, X) == 0) t += g_e[i].c;
    return t;
}
/* 左熵：H(前一个字 | A) */
static double ent_left(const char* A) {
    unsigned long long t = 0;
    for (int i = 0; i < g_ne; i++)
        if (g_e[i].c && is_cjk_char3(g_e[i].a) && strcmp(g_e[i].b, A) == 0) t += g_e[i].c;
    if (!t) return 0.0;
    double h = 0;
    for (int i = 0; i < g_ne; i++) {
        if (!g_e[i].c || !is_cjk_char3(g_e[i].a) || strcmp(g_e[i].b, A) != 0) continue;
        double p = (double)g_e[i].c / (double)t;
        h -= p * log2(p);
    }
    return h;
}
/* 右熵：H(后一个字 | B) */
static double ent_right(const char* B) {
    unsigned long long t = 0;
    for (int i = 0; i < g_ne; i++)
        if (g_e[i].c && is_cjk_char3(g_e[i].b) && strcmp(g_e[i].a, B) == 0) t += g_e[i].c;
    if (!t) return 0.0;
    double h = 0;
    for (int i = 0; i < g_ne; i++) {
        if (!g_e[i].c || !is_cjk_char3(g_e[i].b) || strcmp(g_e[i].a, B) != 0) continue;
        double p = (double)g_e[i].c / (double)t;
        h -= p * log2(p);
    }
    return h;
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
        if (!is_cjk_char3(A) || !is_cjk_char3(B)) continue;
        if (g_e[i].c < mc) continue;

        unsigned long long oa = out_sum(A), ob = out_sum(B);
        if (!oa || !ob) continue;
        double pmi = log2(((double)g_e[i].c / (double)total) /
                          (((double)oa / (double)total) * ((double)ob / (double)total)));
        if (pmi < mp) continue;
        double hl = ent_left(A), hr = ent_right(B);
        double mn = hl < hr ? hl : hr;
        if (mn < me) continue;

        /* 组成字必须在词汇拓扑里（否则 cross-link 无从建立） */
        int aid = huarong_net_find_concept(vocab->net, A);
        int bid = huarong_net_find_concept(vocab->net, B);
        if (aid < 0 || bid < 0) continue;

        char word[24];
        snprintf(word, sizeof(word), "%s%s", A, B);
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

        LOG_INFO("[词涌现] 新增「%s」次数=%u PMI=%.2f 左熵=%.2f 右熵=%.2f (字=%s+%s w=%.2f)",
                 word, g_e[i].c, pmi, hl, hr, A, B, (double)w);
        created++;
    }
    return created;
}
