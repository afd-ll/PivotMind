/**
 * @file reticular.c
 * @brief 网状结构 — 节点级注意力过滤器
 *
 * 每轮调度前预筛节点，减少后续模块的扫描开销。
 * 核心算法：activation × edge_count × recency_bonus
 *
 * v2: 存储 top-K 结果供下游消费（扩散引擎、丘脑调度等）
 */

#include "reticular.h"
#include "huarong_topology.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_CANDIDATES 2048

/* 注意力结果缓冲区（线程不安全，仅 brainstem 串行调用） */
static struct {
    int   node_ids[MAX_CANDIDATES];
    int   topo_types[MAX_CANDIDATES];
    float saliences[MAX_CANDIDATES];
    int   count;
    int   tick_stamp;  /* 递增时间戳，getter 可判断是否过期 */
} _attention_buf = {0};

static int _tick_counter = 0;

typedef struct {
    int node_id;
    int topo_type;
    float salience;
} ScoredNode;

static int _score_cmp_desc(const void* a, const void* b) {
    float sa = ((ScoredNode*)a)->salience;
    float sb = ((ScoredNode*)b)->salience;
    return (sa < sb) ? 1 : (sa > sb) ? -1 : 0;
}

int reticular_attend(MasterTopology* topology, int top_k) {
    if (!topology || top_k <= 0) return 0;
    if (top_k > MAX_CANDIDATES) top_k = MAX_CANDIDATES;

    int total = 0;
    for (int t = 0; t < topology->sub_topo_count; t++) {
        SubTopology* sub = topology->sub_topologies[t];
        if (sub && sub->net) total += sub->net->node_count;
    }

    /* 采样扫描 → 收集候选 */
    int sample = total < 5000 ? total : 5000;
    unsigned int rng = 42 + _tick_counter * 7;
    ScoredNode candidates[MAX_CANDIDATES];
    int cand_count = 0;

    for (int i = 0; i < sample; i++) {
        int ti = rng % topology->sub_topo_count;
        rng = rng * 1103515245 + 12345;
        SubTopology* sub = topology->sub_topologies[ti];
        if (!sub || !sub->net || sub->net->node_count == 0) continue;

        int ni = rng % sub->net->node_count;
        rng = rng * 1103515245 + 12345;
        ReasoningNode* node = sub->net->nodes[ni];
        if (!node || node->is_cooled) continue;

        float sal = node->activation * (1.0f + (float)node->edge_count * 0.05f);
        if (sal < 0.01f) continue;

        if (cand_count < MAX_CANDIDATES) {
            candidates[cand_count].node_id   = node->node_id;
            candidates[cand_count].topo_type = sub->type;
            candidates[cand_count].salience  = sal;
            cand_count++;
        }
    }

    if (cand_count == 0) return 0;

    /* 排序 → 取 top-K */
    qsort(candidates, cand_count, sizeof(ScoredNode), _score_cmp_desc);

    int k = cand_count < top_k ? cand_count : top_k;
    _tick_counter++;
    _attention_buf.count = k;
    _attention_buf.tick_stamp = _tick_counter;

    for (int i = 0; i < k; i++) {
        _attention_buf.node_ids[i]   = candidates[i].node_id;
        _attention_buf.topo_types[i] = candidates[i].topo_type;
        _attention_buf.saliences[i]  = candidates[i].salience;
    }

    return k;
}

int reticular_get_attended(int* out_node_ids, int* out_topo_types,
                            float* out_saliences, int max_count) {
    if (!out_node_ids || max_count <= 0) return 0;

    int n = _attention_buf.count < max_count ? _attention_buf.count : max_count;
    memcpy(out_node_ids, _attention_buf.node_ids, n * sizeof(int));
    if (out_topo_types)
        memcpy(out_topo_types, _attention_buf.topo_types, n * sizeof(int));
    if (out_saliences)
        memcpy(out_saliences, _attention_buf.saliences, n * sizeof(float));
    return n;
}

float reticular_node_salience(MasterTopology* topology, int node_id) {
    if (!topology) return 0.0f;

    /* 先从缓存查（更快） */
    for (int i = 0; i < _attention_buf.count; i++) {
        if (_attention_buf.node_ids[i] == node_id)
            return _attention_buf.saliences[i];
    }

    /* 缓存未命中 → 遍历全拓扑 */
    for (int t = 0; t < topology->sub_topo_count; t++) {
        SubTopology* sub = topology->sub_topologies[t];
        if (!sub || !sub->net) continue;
        for (int i = 0; i < sub->net->node_count; i++) {
            ReasoningNode* node = sub->net->nodes[i];
            if (node && node->node_id == node_id && !node->is_cooled)
                return node->activation * (1.0f + (float)node->edge_count * 0.05f);
        }
    }
    return 0.0f;
}
