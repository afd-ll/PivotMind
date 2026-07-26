/**
 * @file semantic_growth.c
 * @brief 语义拓扑自动生长 — 从词汇拓扑特征向量聚类生成语义节点
 */
#include "semantic_growth.h"
#include "multi_topology.h"
#include "huarong_topology.h"
#include "cognitive_params.h"
#include "common.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int semantic_grow_from_vocab(MasterTopology* master) {
    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    SubTopology* sem   = master_get_sub_topology_by_type(master, TOPO_SEMANTIC);
    if (!vocab || !vocab->net || vocab->net->node_count < SG_MIN_CLUSTER_SIZE) return 0;
    if (!sem   || !sem->net)   return 0;

    HuarongTopologyNet* vnet = vocab->net;
    int vcount = vnet->node_count;
    int sample_n = (vcount < SG_MAX_SAMPLE) ? vcount : SG_MAX_SAMPLE;

    /* 采样：选激活值最高的前 sample_n 个节点 */
    int* sample_ids = (int*)malloc((size_t)sample_n * sizeof(int));
    int* assigned   = (int*)calloc((size_t)sample_n, sizeof(int));
    if (!sample_ids || !assigned) { free(sample_ids); free(assigned); return 0; }

    int step = (vcount > sample_n) ? (vcount / sample_n) : 1;
    if (step < 1) step = 1;
    int si = 0;
    for (int i = 0; i < vcount && si < sample_n; i += step) {
        ReasoningNode* n = vnet->nodes[i];
        if (!n || !n->features || n->feature_dim != NODE_FEATURE_DIM) continue;
        sample_ids[si] = i;
        si++;
    }
    int actual = si;
    if (actual < SG_MIN_CLUSTER_SIZE) { free(sample_ids); free(assigned); return 0; }

    int created = 0;
    for (int i = 0; i < actual && created < SG_MAX_NEW_NODES; i++) {
        if (assigned[i]) continue;

        ReasoningNode* seed = vnet->nodes[sample_ids[i]];
        if (!seed) continue;
        if (!seed->features) lazy_alloc_node_features(seed);
        if (!seed->features) continue;
        float* sf = seed->features;

        /* 预计算种子的 L2 范数平方 */
        float na = 0.0f;
        for (int d = 0; d < NODE_FEATURE_DIM; d++) na += sf[d] * sf[d];

        int* members = (int*)malloc((size_t)actual * sizeof(int));
        int mcnt = 0;
        members[mcnt++] = sample_ids[i];

        for (int j = i + 1; j < actual; j++) {
            if (assigned[j]) continue;
            ReasoningNode* cand = vnet->nodes[sample_ids[j]];
            if (!cand) continue;
            if (!cand->features) lazy_alloc_node_features(cand);
            if (!cand->features) continue;

            float dot = 0.0f, nb = 0.0f;
            for (int d = 0; d < NODE_FEATURE_DIM; d++) {
                dot += sf[d] * cand->features[d];
                nb  += cand->features[d] * cand->features[d];
            }
            float sim = (na > 0.0f && nb > 0.0f)
                        ? dot / (sqrtf(na) * sqrtf(nb)) : 0.0f;

            if (sim >= SG_COSINE_THRESHOLD)
                members[mcnt++] = sample_ids[j];
        }

        if (mcnt < SG_MIN_CLUSTER_SIZE) { free(members); continue; }

        /* 标记成员为已分配 */
        assigned[i] = 1;
        for (int m = 1; m < mcnt; m++)
            for (int k = 0; k < actual; k++)
                if (sample_ids[k] == members[m]) { assigned[k] = 1; break; }

        /* 构建语义节点名 */
        char sname[128];
        snprintf(sname, sizeof(sname), "sem_%s_%d",
                 seed->concept ? seed->concept : "?", mcnt);

        /* 特征向量 = 成员均值 */
        float feat[NODE_FEATURE_DIM];
        memset(feat, 0, sizeof(feat));
        for (int m = 0; m < mcnt; m++) {
            ReasoningNode* mn = vnet->nodes[members[m]];
            if (!mn) continue;
            if (!mn->features) lazy_alloc_node_features(mn);
            if (!mn->features) continue;
            for (int d = 0; d < NODE_FEATURE_DIM; d++) feat[d] += mn->features[d];
        }
        for (int d = 0; d < NODE_FEATURE_DIM; d++) feat[d] /= (float)mcnt;

        ReasoningNode* sn = huarong_net_add_node(sem->net, sname, feat, NODE_FEATURE_DIM);
        if (!sn) { free(members); continue; }

        /* 跨拓扑链接 */
        for (int m = 0; m < mcnt; m++)
            master_add_cross_link(master, vocab->topo_id, members[m],
                                  sem->topo_id, sn->node_id, 0.65f, "semantic_cluster");
        created++;
        free(members);
    }

    free(sample_ids);
    free(assigned);
    return created;
}
