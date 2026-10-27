/* tools/pos_probe.c — 词类聚类诊断探针
 *
 * 用法: ./pos_probe <state_file>
 *
 * 查「POS 锚点初始化: 0 个」到底是哪一环断：
 *   ① vocab 拓扑本身（节点数 / 种子词是否在表里 / features 是否挂上）
 *   ② emergent_pos_init_centroids 的返回与逐类结果
 *
 * 为什么要有这个：聚类系统（10 硬编码类 + 32 类可涌现 + 26 维分布签名）
 * 早就写完了，但线上一个锚点都没立起来，等于空转。
 */
#include "pivotmind_paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multi_topology.h"
#include "huarong_topology.h"
#include "emergent_pos.h"
#include "cognitive_controller.h"
#include "common.h"

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
    if (argc < 2) {
        fprintf(stderr, "用法: %s <state_file>\n", argv[0]);
        return 2;
    }
    MasterTopology* master = load_master(argv[1]);
    if (!master) { fprintf(stderr, "加载失败: %s\n", argv[1]); return 1; }

    SubTopology* vocab = NULL;
    for (int t = 0; t < master->sub_topo_count; t++) {
        if (master->sub_topologies[t] && master->sub_topologies[t]->type == TOPO_VOCABULARY) {
            vocab = master->sub_topologies[t];
            break;
        }
    }
    if (!vocab || !vocab->net) {
        printf("[探针] vocab 拓扑缺失\n");
        master_topology_destroy(master);
        return 1;
    }

    int nc = vocab->net->node_count;
    int with_feat = 0, null_feat = 0, zero_feat = 0;
    for (int i = 0; i < nc; i++) {
        ReasoningNode* nd = vocab->net->nodes[i];
        if (!nd) continue;
        if (!nd->features) { null_feat++; continue; }
        with_feat++;
        int all_zero = 1;
        for (int d = 0; d < PM_NODE_FEATURE_DIM; d++)
            if (nd->features[d] != 0.0f) { all_zero = 0; break; }
        if (all_zero) zero_feat++;
    }
    printf("[探针] vocab 节点=%d 有特征=%d 无特征=%d 全零特征=%d\n",
           nc, with_feat, null_feat, zero_feat);

    /* 逐个种子词查在不在（每类前 5 个） */
    printf("[探针] 种子词命中情况：\n");
    EmergentPOS* ep = emergent_pos_create("zh");
    if (!ep) { printf("  emergent_pos_create 失败\n"); master_topology_destroy(master); return 1; }

    for (int tag = POS_NOUN; tag < POS_COUNT; tag++) {
        char line[512];
        int off = 0;
        int hits = 0, seeds = 0;
        off += snprintf(line + off, sizeof(line) - off, "  %-2d %-10s", tag,
                        emergent_pos_class_name(ep, tag));
        /* 通过内部结构读种子（探针直读，生产路径不会这么用） */
        for (int s = 0; s < POS_ANCHOR_MAX_SEEDS; s++) {
            const char* w = ep->anchors[tag].seeds[s];
            if (!w || !w[0]) continue;
            seeds++;
            int id = huarong_net_find_concept(vocab->net, w);
            int has_feat = 0, ec = 0;
            if (id >= 0 && id < nc && vocab->net->nodes[id]) {
                ReasoningNode* nd = vocab->net->nodes[id];
                has_feat = (nd->features != NULL);
                ec = nd->edge_count;
                hits++;
            }
            if (off < (int)sizeof(line) - 40)
                off += snprintf(line + off, sizeof(line) - off, " %s%s(%d边%s)",
                                w, id >= 0 ? "" : "✗", ec, has_feat ? "" : ",无特征");
            if (seeds >= 5) break;
        }
        printf("%s  → 命中 %d/%d\n", line, hits, seeds);
    }

    /* 关键判据：gateway 走的顺序是「先 load 磁盘 → 再 init_centroids」。
     * load 成功则各锚点 is_active=1，init 会全部 continue ⇒ 返回值 0，
     * 但【活跃锚点数】并不为 0。所以「初始化 N 个」这条日志天生会被误读。 */
    int nload = emergent_pos_load(ep, NULL);
    printf("[探针] emergent_pos_load 返回 = %d, 载入后 anchor_count = %d\n",
           nload, emergent_pos_anchor_count(ep));
    for (int tag = POS_NOUN; tag < POS_COUNT; tag++)
        printf("  [load后] %-10s active=%d member=%d\n", ep->anchors[tag].label_cn,
               ep->anchors[tag].is_active, ep->anchors[tag].member_count);

    int inited = emergent_pos_init_centroids(ep, master);
    printf("[探针] emergent_pos_init_centroids 返回 = %d（0 可能只是「已激活，无需再初始化」）\n", inited);
    printf("[探针] emergent_pos_anchor_count = %d\n", emergent_pos_anchor_count(ep));
    for (int tag = POS_NOUN; tag < POS_COUNT; tag++) {
        POSAnchor* a = &ep->anchors[tag];
        printf("  %-10s active=%d member=%d stability=%.2f\n",
               a->label_cn ? a->label_cn : "?", a->is_active, a->member_count,
               (double)a->centroid_stability);
    }

    emergent_pos_destroy(ep);
    master_topology_destroy(master);
    return 0;
}
