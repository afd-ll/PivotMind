/* pos_cover.c — 全量测 POS 覆盖率（CX-38 前置：证明「词类补好了没有」）
 *
 * 两个数必须分开报：
 *   t1 = emergent_pos_tag()       运行时真实路径（seed_tag 精确匹配 → 特征相似度）
 *   t2 = emergent_pos_classify()  纯指纹层（跳过种子）—— 这才是「学到」的能力
 *
 * 种子词会天然撑高 t1，所以核心指标是 t2 对「非种子词」的覆盖率。
 * 用法: pos_cover <state_file>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "multi_topology.h"
#include "cognitive_controller.h"
#include "emergent_pos.h"

static MasterTopology* load_master(const char* path) {
    MasterTopology* master = master_topology_create(16);
    if (!master) return NULL;
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 60000, 10);
    master_add_sub_topology(master, TOPO_SEMANTIC,   "语义拓扑", 20000, 9);
    master_add_sub_topology(master, TOPO_EMOTION,    "情绪拓扑", 8000, 8);
    master_add_sub_topology(master, TOPO_SYNTAX,     "语法拓扑", 4000, 7);
    master_add_sub_topology(master, TOPO_CONTEXT,    "上下文拓扑", 4000, 6);
    master_add_sub_topology(master, TOPO_DOMAIN,     "领域拓扑", 4000, 5);
    master_add_sub_topology(master, TOPO_PRAGMA,     "语用拓扑", 4000, 4);
    master_add_sub_topology(master, TOPO_CULTURE,    "文化拓扑", 4000, 3);
    master_add_sub_topology(master, TOPO_CONCEPT,    "概念拓扑", 20000, 9);
    master_add_sub_topology(master, TOPO_MASTER,     "主拓扑", 100, 0);
    master_add_sub_topology(master, TOPO_TEMPLATE,   "模板拓扑", 8000, 8);
    master_add_sub_topology(master, TOPO_VISUAL,     "视觉拓扑", 8000, 7);
    if (master_load_state(master, path) < 0) {
        master_topology_destroy(master);
        return NULL;
    }
    return master;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "用法: %s <state_file>\n", argv[0]); return 2; }

    MasterTopology* master = load_master(argv[1]);
    if (!master) { fprintf(stderr, "加载失败\n"); return 1; }

    SubTopology* vocab = NULL;
    for (int t = 0; t < master->sub_topo_count; t++) {
        if (master->sub_topologies[t] && master->sub_topologies[t]->type == TOPO_VOCABULARY) {
            vocab = master->sub_topologies[t];
            break;
        }
    }
    if (!vocab || !vocab->net) { printf("vocab 缺失\n"); return 1; }

    EmergentPOS* ep = emergent_pos_create("zh");
    if (!ep) { printf("create 失败\n"); return 1; }
    emergent_pos_init_centroids(ep, master);

    int nc = vocab->net->node_count;
    int total = 0, seeds = 0;
    int s_t1 = 0, s_t2 = 0;
    int s_correct = 0;                   /* 种子词 t2 == 种子标签：指纹层的正确率金标准 */
    int n_t1 = 0, n_t2 = 0;
    int per_t1[POS_COUNT] = {0};
    int per_t2[POS_COUNT] = {0};
    int zero_feat = 0;

    for (int i = 0; i < nc; i++) {
        ReasoningNode* nd = vocab->net->nodes[i];
        if (!nd || !nd->concept || !nd->concept[0]) continue;
        total++;

        POSTag st = emergent_pos_seed_tag(ep, nd->concept);
        int is_seed = (st != POS_UNKNOWN);

        POSTag t1 = emergent_pos_tag(ep, master, nd->concept);
        POSTag t2 = POS_UNKNOWN;
        if (nd->features) t2 = emergent_pos_classify(ep, nd->features);
        else zero_feat++;

        if (t1 > POS_UNKNOWN && t1 < POS_COUNT) per_t1[t1]++;
        if (t2 > POS_UNKNOWN && t2 < POS_COUNT) per_t2[t2]++;

        if (is_seed) {
            seeds++;
            if (t1 != POS_UNKNOWN) s_t1++;
            if (t2 != POS_UNKNOWN) s_t2++;
            if (t2 == st) s_correct++;
        } else {
            if (t1 != POS_UNKNOWN) n_t1++;
            if (t2 != POS_UNKNOWN) n_t2++;
        }
    }

    int n_nonseed = total - seeds;
    printf("\n=== POS 覆盖率 ===\n");
    printf("词汇节点总数     : %d（无特征 %d）\n", total, zero_feat);
    printf("种子词           : %d  (%.1f%%)\n", seeds, total ? 100.0 * seeds / total : 0);
    printf("非种子词         : %d\n", n_nonseed);
    printf("  t1 全路径命中  : %d  (%.1f%%)\n", n_t1, n_nonseed ? 100.0 * n_t1 / n_nonseed : 0);
    printf("  t2 纯指纹命中  : %d  (%.1f%%)   <<< 核心指标\n", n_t2, n_nonseed ? 100.0 * n_t2 / n_nonseed : 0);
    printf("种子词 t2 命中   : %d/%d (%.1f%%)\n", s_t2, seeds, seeds ? 100.0 * s_t2 / seeds : 0);
    printf("种子自洽率(t2==标签): %d/%d (%.1f%%)   <<< 正确率金标准，低则覆盖率是假的\n",
           s_correct, seeds, seeds ? 100.0 * s_correct / seeds : 0);
    printf("\n各类分布（全部词）:\n");
    for (int t = POS_NOUN; t < POS_COUNT; t++) {
        printf("  %-10s t1=%5d  t2=%5d\n",
               ep->anchors[t].label_cn ? ep->anchors[t].label_cn : "?", per_t1[t], per_t2[t]);
    }
    emergent_pos_destroy(ep);
    master_topology_destroy(master);
    return 0;
}
