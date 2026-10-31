/* edge_wdist.c — 统计 state 里词汇拓扑的边权重分布
 *
 * 目的：决定「用边做表示」的数据源。
 *   - 权重 ~1.0 的边是语序边 / 组成字边（结构性的，不是语义距离）
 *   - 权重 0.05~0.30 的边是 PPMI 语义边
 * 所以要先把两类分开，看各自占多少、覆盖多少词。
 *
 * 用法: edge_wdist <state_file>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "multi_topology.h"

#define NBUCKET 12

static int cmp_int(const void* a, const void* b) {
    int x = *(const int*)a, y = *(const int*)b;
    return (x > y) - (x < y);
}

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
    if (master_load_state(master, path) < 0) { master_topology_destroy(master); return NULL; }
    return master;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "用法: %s <state_file>\n", argv[0]); return 2; }
    MasterTopology* master = load_master(argv[1]);
    if (!master) { fprintf(stderr, "加载失败\n"); return 1; }

    SubTopology* vocab = NULL;
    for (int t = 0; t < master->sub_topo_count; t++) {
        if (master->sub_topologies[t] && master->sub_topologies[t]->type == TOPO_VOCABULARY) {
            vocab = master->sub_topologies[t]; break;
        }
    }
    if (!vocab || !vocab->net) { printf("vocab 缺失\n"); return 1; }

    int nc = vocab->net->node_count;
    long total = 0;
    long bucket[NBUCKET]; memset(bucket, 0, sizeof(bucket));
    float mn = 1e9f, mx = -1e9f;

    /* 每词「语义边」（weight < 0.5）数量分布 */
    int *semdeg = (int*)calloc(nc, sizeof(int));
    int *alldeg = (int*)calloc(nc, sizeof(int));
    if (!semdeg || !alldeg) { printf("malloc 失败\n"); return 1; }

    for (int i = 0; i < nc; i++) {
        ReasoningNode* nd = vocab->net->nodes[i];
        if (!nd) continue;
        for (int e = 0; e < nd->edge_count; e++) {
            float w = nd->edges[e].weight;
            if (w < 0) w = -w;
            total++;
            alldeg[i]++;
            if (w < 0.5f) semdeg[i]++;
            if (w < mn) mn = w;
            if (w > mx) mx = w;
            int b = (int)(w * NBUCKET);
            if (b < 0) b = 0;
            if (b >= NBUCKET) b = NBUCKET - 1;
            bucket[b]++;
        }
    }

    printf("\n=== state 词汇拓扑边权重分布 ===\n");
    printf("节点 %d，出边总数 %ld（平均 %.1f 条/词）\n", nc, total, nc ? (double)total / nc : 0);
    printf("权重范围 [%.3f, %.3f]\n\n", mn, mx);
    printf("分桶（每桶宽 1/12）：\n");
    for (int b = 0; b < NBUCKET; b++) {
        if (!bucket[b]) continue;
        printf("  [%4.2f,%4.2f)  %8ld  %5.1f%%\n",
               (float)b / NBUCKET, (float)(b + 1) / NBUCKET,
               bucket[b], total ? 100.0 * bucket[b] / total : 0);
    }

    /* 语义边（w<0.5）的每词度数分布 */
    int with_sem = 0;
    long sem_total = 0;
    for (int i = 0; i < nc; i++) {
        if (semdeg[i] > 0) { with_sem++; sem_total += semdeg[i]; }
    }
    int *v = (int*)malloc(sizeof(int) * (with_sem ? with_sem : 1));
    int k = 0;
    for (int i = 0; i < nc; i++) if (semdeg[i] > 0) v[k++] = semdeg[i];
    /* 简单排序（with_sem 通常 < 40000） */
    qsort(v, k, sizeof(int), cmp_int);
    printf("\n语义边（weight<0.5）：%ld 条，覆盖 %d 个词（%.1f%%）\n",
           sem_total, with_sem, nc ? 100.0 * with_sem / nc : 0);
    if (k) {
        printf("  每词语义度数：min=%d p10=%d p25=%d p50=%d p75=%d p90=%d max=%d\n",
               v[0], v[k / 10], v[k / 4], v[k / 2], v[k * 3 / 4], v[k * 9 / 10], v[k - 1]);
    }
    free(v); free(semdeg); free(alldeg);
    master_topology_destroy(master);
    return 0;
}
