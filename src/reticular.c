/**
 * @file reticular.c
 * @brief 网状结构 — 节点级注意力过滤器
 *
 * 每轮调度前预筛节点，减少后续模块的扫描开销。
 * 核心算法：activation × connection_count × recency_bonus
 */

#include "reticular.h"
#include "huarong_topology.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CANDIDATES 2048

typedef struct {
    int node_id;
    float salience;
} ScoredNode;

static int _score_cmp_desc(const void* a, const void* b) {
    float sa = ((ScoredNode*)a)->salience;
    float sb = ((ScoredNode*)b)->salience;
    return (sa < sb) ? 1 : (sa > sb) ? -1 : 0;
}

int reticular_attend(MasterTopology* topology, int top_k) {
    if (!topology || top_k <= 0) return 0;

    int total = 0;
    for (int t = 0; t < topology->sub_topo_count; t++) {
        SubTopology* sub = topology->sub_topologies[t];
        if (sub && sub->net) total += sub->net->node_count;
    }

    int scored = 0;
    int sample = total < 5000 ? total : 5000;  /* 最多采5000个 */
    unsigned int rng = 42;

    for (int i = 0; i < sample; i++) {
        int ti = rng % topology->sub_topo_count;
        rng = rng * 1103515245 + 12345;
        SubTopology* sub = topology->sub_topologies[ti];
        if (!sub || !sub->net || sub->net->node_count == 0) continue;

        int ni = rng % sub->net->node_count;
        rng = rng * 1103515245 + 12345;
        ReasoningNode* node = sub->net->nodes[ni];
        if (!node || node->is_cooled) continue;

        /* salience = activation * log2(connections+1) * recency */
        float sal = node->activation * (1.0f + (float)node->connection_count * 0.05f);
        if (sal < 0.01f) continue;
        scored++;
    }

    return scored;
}

float reticular_node_salience(MasterTopology* topology, int node_id) {
    if (!topology) return 0.0f;

    /* 遍历所有子拓扑找节点（非性能路径，仅调试用） */
    for (int t = 0; t < topology->sub_topo_count; t++) {
        SubTopology* sub = topology->sub_topologies[t];
        if (!sub || !sub->net) continue;
        for (int i = 0; i < sub->net->node_count; i++) {
            ReasoningNode* node = sub->net->nodes[i];
            if (node && node->node_id == node_id && !node->is_cooled)
                return node->activation * (1.0f + (float)node->connection_count * 0.05f);
        }
    }
    return 0.0f;
}
