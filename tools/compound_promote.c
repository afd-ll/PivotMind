/**
 * compound_promote.c — 词涌现器：字拓扑 → 概念拓扑（晋升式）
 *
 * 架构：字拓扑保持纯单字（基本单元不动）；高共现字对按相对强度
 * （Hebbian）涌现为词节点，晋升到概念拓扑（TOPO_CONCEPT），
 * 通过 cross-link 连回组成它的字节点。词从语料统计自己长出来，
 * 不依赖外部词典。
 *
 * 涌现条件（双向确认，防单向噪声）：
 *   w(A→B) > avg_w(A) × THRESHOLD   且
 *   w(B→A) > avg_w(B) × THRESHOLD
 *   strength = min(w_ab/avg_a, w_ba/avg_b)
 *
 * 用法: compound_promote <state.dat> [threshold=3.0] [max_words=500]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multi_topology.h"
#include "huarong_topology.h"

#define DEFAULT_THRESHOLD 3.0f
#define DEFAULT_MAX_WORDS 500
#define MIN_EDGES 5   /* 节点边数下限：防稀疏噪声（单次学习的字对全是"强绑定"） */

typedef struct {
    char word[64];        /* 词节点名（两字拼接） */
    int  node_a, node_b;  /* 字节点 id */
    float strength;       /* 相对强度（双向取最小） */
} WordCandidate;

static int is_cjk_char(const char* s) {
    /* UTF-8 汉字 = 3 字节，且首字节 E4-EF（中文基本区） */
    unsigned char c = (unsigned char)s[0];
    if (c >= 0xE0 && c <= 0xEF) {
        return (s[1] != 0 && s[2] != 0 && s[3] == 0) ? 1 : 0;
    }
    return 0;
}

static int cmp_candidate(const void* a, const void* b) {
    float sa = ((const WordCandidate*)a)->strength;
    float sb = ((const WordCandidate*)b)->strength;
    return (sa > sb) ? -1 : (sa < sb) ? 1 : 0;
}

static SubTopology* find_topo(MasterTopology* master, int type) {
    for (int t = 0; t < master->sub_topo_count; t++) {
        if (master->sub_topologies[t] &&
            (int)master->sub_topologies[t]->type == type)
            return master->sub_topologies[t];
    }
    return NULL;
}

/* 找 B→A 的反向边索引（O(1) conn_hash） */
static int find_reverse_edge(ReasoningNode* b, ReasoningNode* a) {
    for (int i = 0; i < b->edge_count; i++) {
        if (b->edges && b->edges[i].target == a) return i;
    }
    return -1;
}

int main(int argc, char** argv) {
    const char* state_path = argc > 1 ? argv[1] : "pivotmind_state.dat";
    float threshold = argc > 2 ? (float)atof(argv[2]) : DEFAULT_THRESHOLD;
    int max_words = argc > 3 ? atoi(argv[3]) : DEFAULT_MAX_WORDS;
    setbuf(stdout, NULL);

    printf("╔═══════════════════════════════════════════╗\n");
    printf("║  词涌现器 (字拓扑→概念拓扑) 阈值=%.1f 上限=%d ║\n", threshold, max_words);
    printf("╚═══════════════════════════════════════════╝\n\n");

    MasterTopology* master = master_topology_create(11);
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 100000, 10);
    master_add_sub_topology(master, TOPO_SEMANTIC,   "语义拓扑", 50000, 9);
    master_add_sub_topology(master, TOPO_EMOTION,    "情绪拓扑", 4000, 8);
    master_add_sub_topology(master, TOPO_SYNTAX,     "语法拓扑", 1000, 7);
    master_add_sub_topology(master, TOPO_CONTEXT,    "上下文拓扑", 1000, 6);
    master_add_sub_topology(master, TOPO_DOMAIN,     "领域拓扑", 1000, 5);
    master_add_sub_topology(master, TOPO_PRAGMA,     "语用拓扑", 1000, 4);
    master_add_sub_topology(master, TOPO_CULTURE,    "文化拓扑", 1000, 3);
    master_add_sub_topology(master, TOPO_CONCEPT,    "概念拓扑", 50000, 9);
    master_add_sub_topology(master, TOPO_MASTER,     "主拓扑", 100, 0);
    master_add_sub_topology(master, TOPO_TEMPLATE,   "模板拓扑", 20000, 8);

    int loaded = master_load_state(master, state_path);
    if (loaded <= 10) {
        printf("× 状态加载异常 (%d)\n", loaded);
        master_topology_destroy(master);
        return 1;
    }
    printf("✓ 状态加载: %d 节点\n", loaded);

    SubTopology* vocab = find_topo(master, TOPO_VOCABULARY);
    SubTopology* concept = find_topo(master, TOPO_CONCEPT);
    if (!vocab || !vocab->net || !concept || !concept->net) {
        printf("× 找不到字拓扑/概念拓扑\n");
        master_topology_destroy(master);
        return 1;
    }
    printf("  字拓扑: %d 节点, 概念拓扑: %d 节点\n\n",
           vocab->net->node_count, concept->net->node_count);

    /* 1. 计算每个字节点的边权均值 */
    int vn = vocab->net->node_count;
    float* avg_w = (float*)calloc((size_t)vn, sizeof(float));
    if (!avg_w) { printf("× OOM\n"); master_topology_destroy(master); return 1; }
    for (int i = 0; i < vn; i++) {
        ReasoningNode* n = vocab->net->nodes[i];
        if (!n || !n->edges || n->edge_count == 0) continue;
        float sum = 0.0f;
        for (int j = 0; j < n->edge_count; j++)
            sum += n->edges[j].weight;
        avg_w[i] = sum / (float)n->edge_count;
    }

    /* 2. 收集候选（双向确认） */
    WordCandidate* cands = (WordCandidate*)malloc(sizeof(WordCandidate) * (size_t)(vn * 4));
    if (!cands) { free(avg_w); printf("× OOM\n"); master_topology_destroy(master); return 1; }
    int cand_count = 0;

    for (int i = 0; i < vn && cand_count < vn * 4; i++) {
        ReasoningNode* a = vocab->net->nodes[i];
        if (!a || !a->edges || a->edge_count == 0 || avg_w[i] <= 0.001f) continue;
        if (!a->concept || !is_cjk_char(a->concept)) continue;   /* 只处理中文单字 */
        if (a->edge_count < MIN_EDGES) continue;                  /* 稀疏节点无统计意义 */

        for (int j = 0; j < a->edge_count; j++) {
            Edge* e = &a->edges[j];
            if (!e->target || e->weight <= 0.0f) continue;
            ReasoningNode* b = e->target;

            /* B 也是中文单字（同一字拓扑、不是自己、词节点名已有排序） */
            if (b->node_id <= a->node_id) continue;
            if (!b->concept || !is_cjk_char(b->concept)) continue;

            float rel_ab = e->weight / avg_w[i];
            if (rel_ab < threshold) continue;

            /* 反向确认 */
            int ri = find_reverse_edge(b, a);
            if (ri < 0) continue;
            float w_ba = b->edges[ri].weight;
            float rel_ba = w_ba / avg_w[b->node_id];
            if (rel_ba < threshold) continue;

            /* 候选：strength 取双向相对强度最小值 */
            float strength = rel_ab < rel_ba ? rel_ab : rel_ba;
            WordCandidate* c = &cands[cand_count++];
            c->node_a = a->node_id;
            c->node_b = b->node_id;
            c->strength = strength;
            snprintf(c->word, sizeof(c->word), "%s%s", a->concept, b->concept);
        }
    }
    printf("候选字对: %d\n\n", cand_count);

    /* 3. 按强度排序 + 截断 */
    qsort(cands, (size_t)cand_count, sizeof(WordCandidate), cmp_candidate);
    if (cand_count > max_words) cand_count = max_words;
    printf("涌现前 %d 个词节点:\n", cand_count);

    /* 4. 建词节点 + cross-link */
    int created = 0, skipped = 0;
    for (int k = 0; k < cand_count; k++) {
        WordCandidate* c = &cands[k];

        /* 词节点已存在？ */
        if (huarong_net_find_concept(concept->net, c->word) >= 0) { skipped++; continue; }

        ReasoningNode* wn = huarong_net_add_node(concept->net, c->word, NULL, 0);
        if (!wn) { skipped++; continue; }
        node_hash_add(concept->node_hash, wn);

        /* cross-link: 词节点 → 两个组成字（权重 = 归一化相对强度 0.3~0.9） */
        float w = 0.3f + c->strength * 0.15f;
        if (w > 0.9f) w = 0.9f;
        master_add_cross_link(master, TOPO_CONCEPT, wn->node_id,
                              TOPO_VOCABULARY, c->node_a, w, "compound");
        master_add_cross_link(master, TOPO_CONCEPT, wn->node_id,
                              TOPO_VOCABULARY, c->node_b, w, "compound");

        if (created < 25 || k >= cand_count - 3)
            printf("  [%d] %s (强度=%.1f, 字%d+字%d)\n",
                   k + 1, c->word, c->strength, c->node_a, c->node_b);
        created++;
    }
    printf("\n✓ 涌现词节点: %d (跳过已存在: %d)\n", created, skipped);

    /* 5. 保存 */
    printf("保存状态 -> %s ...\n", state_path);
    int saved = master_save_state(master, state_path);
    printf("保存: %s (%d)\n", saved > 0 ? "OK" : "FAIL", saved);
    master_topology_destroy(master);
    return 0;
}
