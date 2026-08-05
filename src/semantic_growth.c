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

/* v0.5.7: 激活降序比较（语义生长采样用） */
typedef struct { int id; float act; } SgActItem;
static int sg_act_cmp(const void* a, const void* b) {
    float da = ((const SgActItem*)a)->act, db = ((const SgActItem*)b)->act;
    return (da > db) ? -1 : (da < db) ? 1 : 0;
}

int semantic_grow_from_topology(MasterTopology* master, int topo_type) {
    SubTopology* vocab = master_get_sub_topology_by_type(master, topo_type);
    SubTopology* sem   = master_get_sub_topology_by_type(master, TOPO_SEMANTIC);
    if (!vocab || !vocab->net || vocab->net->node_count < SG_MIN_CLUSTER_SIZE) return 0;
    if (!sem   || !sem->net)   return 0;

    HuarongTopologyNet* vnet = vocab->net;
    int vcount = vnet->node_count;
    int sample_n = (vcount < SG_MAX_SAMPLE) ? vcount : SG_MAX_SAMPLE;

    /* v0.5.7: 采样按激活值降序排序（原等间隔采样漏高频词——"时间"
     * 可能不在采样点，聚类成员缺高频核心词，语义场查询失败）。
     * 选激活最高的前 sample_n 个节点。 */
    int* sample_ids = (int*)malloc((size_t)sample_n * sizeof(int));
    int* assigned   = (int*)calloc((size_t)sample_n, sizeof(int));
    if (!sample_ids || !assigned) { free(sample_ids); free(assigned); return 0; }

    SgActItem* items = (SgActItem*)malloc((size_t)vcount * sizeof(SgActItem));
    if (!items) { free(sample_ids); free(assigned); return 0; }
    int icnt = 0;
    for (int i = 0; i < vcount; i++) {
        ReasoningNode* n = vnet->nodes[i];
        if (!n || !n->features || n->feature_dim != NODE_FEATURE_DIM) continue;
        items[icnt].id = i;
        items[icnt].act = n->activation;
        icnt++;
    }
    qsort(items, (size_t)icnt, sizeof(SgActItem), sg_act_cmp);
    int actual = (icnt < sample_n) ? icnt : sample_n;
    for (int k = 0; k < actual; k++) sample_ids[k] = items[k].id;
    free(items);
    if (actual < SG_MIN_CLUSTER_SIZE) { free(sample_ids); free(assigned); return 0; }

    /* 预计算所有采样节点的 L2 范数平方（避免内层重复计算） */
    float* norms = (float*)calloc((size_t)actual, sizeof(float));
    if (!norms) { free(sample_ids); free(assigned); return 0; }
    for (int k = 0; k < actual; k++) {
        ReasoningNode* n = vnet->nodes[sample_ids[k]];
        if (!n) continue;
        if (!n->features) lazy_alloc_node_features(n);
        if (!n->features) continue;
        float* f = n->features;
        /* v0.5.7: 词节点特征为 0（建节点时 NULL，从未训练）→ 用
         * cross-link 的成员字特征均值填充——"时间"= 时+间 特征均值，
         * 与"过去"（同语境词）特征相似，词聚类才能形成语义场 */
        if (topo_type == TOPO_CONCEPT && master->cross_links &&
            master->cross_link_count > 0) {
            int is_zero = 1;
            for (int d = 0; d < NODE_FEATURE_DIM; d++)
                if (f[d] != 0.0f) { is_zero = 0; break; }
            if (is_zero) {
                float* acc = (float*)calloc((size_t)NODE_FEATURE_DIM, sizeof(float));
                int cnt = 0;
                for (int li = 0; li < master->cross_link_count; li++) {
                    CrossTopologyLink* l = master->cross_links[li];
                    if (!l || l->from_topo_id != TOPO_CONCEPT ||
                        l->from_node_id != n->node_id ||
                        l->to_topo_id != TOPO_VOCABULARY) continue;
                    if (l->to_node_id >= vnet->node_count) continue;
                    ReasoningNode* cn = vnet->nodes[l->to_node_id];
                    if (!cn || !cn->features) continue;
                    for (int d = 0; d < NODE_FEATURE_DIM; d++) acc[d] += cn->features[d];
                    cnt++;
                }
                if (cnt > 0) {
                    for (int d = 0; d < NODE_FEATURE_DIM; d++) f[d] = acc[d] / (float)cnt;
                }
                free(acc);
            }
        }
        float n2 = 0.0f;
        for (int d = 0; d < NODE_FEATURE_DIM; d++) n2 += f[d] * f[d];
        norms[k] = n2;
    }

    /* node_id → sample_ids 索引映射（O(1) 成员标记） */
    int* pos_by_node = (int*)malloc((size_t)vnet->max_nodes * sizeof(int));
    if (pos_by_node) {
        for (int k = 0; k < vnet->max_nodes; k++) pos_by_node[k] = -1;
    }
    if (pos_by_node) {
        for (int k = 0; k < actual; k++) pos_by_node[sample_ids[k]] = k;
    }

    /* v0.5.7: 重复建簇修复——预扫描跨拓扑链接，标记"已有语义场"的
     * 源节点。此前每轮采样滚动，已成簇成员下轮又当 seed 重新建场
     * （实测 sem_你 49 次/sem_delightful 32 次 → 节点虚胖+检索稀释）。
     * 建簇前检查：seed/成员若已归属某个语义场（有 cross-link 指向
     * TOPO_SEMANTIC），则跳过。 */
    unsigned char* has_sem_link = (unsigned char*)calloc((size_t)vnet->max_nodes, 1);
    if (has_sem_link && master->cross_links && master->cross_link_count > 0) {
        for (int li = 0; li < master->cross_link_count; li++) {
            CrossTopologyLink* l = master->cross_links[li];
            if (!l) continue;
            if (l->to_topo_id == TOPO_SEMANTIC &&
                l->from_topo_id == topo_type &&
                l->from_node_id >= 0 && l->from_node_id < vnet->max_nodes) {
                has_sem_link[l->from_node_id] = 1;
            }
        }
    }

    int created = 0;
    for (int i = 0; i < actual && created < SG_MAX_NEW_NODES; i++) {
        if (assigned[i]) continue;

        ReasoningNode* seed = vnet->nodes[sample_ids[i]];
        if (!seed) continue;
        if (!seed->features) lazy_alloc_node_features(seed);
        if (!seed->features) continue;
        /* 已有语义场的 seed 跳过（重复建簇修复） */
        if (has_sem_link && has_sem_link[sample_ids[i]]) continue;
        float* sf = seed->features;
        float na = norms[i];

        int* members = (int*)malloc((size_t)actual * sizeof(int));
        int mcnt = 0;
        members[mcnt++] = sample_ids[i];

        for (int j = i + 1; j < actual; j++) {
            if (assigned[j]) continue;
            ReasoningNode* cand = vnet->nodes[sample_ids[j]];
            if (!cand) continue;
            if (!cand->features) lazy_alloc_node_features(cand);
            if (!cand->features) continue;
            /* 已是其他语义场成员的候选跳过（重复建簇修复） */
            if (has_sem_link && has_sem_link[sample_ids[j]]) continue;

            float nb = norms[j];
            if (na <= 0.0f || nb <= 0.0f) continue;
            float dot = 0.0f;
            float* cf = cand->features;
            for (int d = 0; d < NODE_FEATURE_DIM; d++)
                dot += sf[d] * cf[d];
            float sim = dot / (sqrtf(na) * sqrtf(nb));

            if (sim >= SG_COSINE_THRESHOLD)
                members[mcnt++] = sample_ids[j];
        }

        if (mcnt < SG_MIN_CLUSTER_SIZE) { free(members); continue; }

        /* 标记成员为已分配 — 用 pos_by_node 做 O(1) 查找 */
        assigned[i] = 1;
        if (pos_by_node) {
            for (int m = 1; m < mcnt; m++) {
                int k = pos_by_node[members[m]];
                if (k >= 0) assigned[k] = 1;
            }
        } else {
            for (int m = 1; m < mcnt; m++)
                for (int k = 0; k < actual; k++)
                    if (sample_ids[k] == members[m]) { assigned[k] = 1; break; }
        }

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
    free(norms);
    free(pos_by_node);
    free(has_sem_link);
    return created;
}

/* 兼容旧名：字拓扑语义生长（brainstem 原调用） */
int semantic_grow_from_vocab(MasterTopology* master) {
    return semantic_grow_from_topology(master, TOPO_VOCABULARY);
}

/* v0.5.7: 词级语义场——从概念拓扑（词节点）聚类生成语义概念。
 * 词特征（共现分布）相似者聚类（时间/过去/钟表 → 时间概念场），
 * 语义节点 cross-link 回成员词——语义场查询（词→概念→成员词）。 */
int semantic_grow_from_concepts(MasterTopology* master) {
    return semantic_grow_from_topology(master, TOPO_CONCEPT);
}
