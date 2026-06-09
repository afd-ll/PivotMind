/**
 * @file dream_engine.c
 * @brief 梦境引擎实现 — 空闲时知识重组，基于联想推理引擎
 *
 * 流程：
 *   1. 采样：从词汇/语义/情绪拓扑随机选取节点
 *   2. 联想：用采样节点的概念名调用联想引擎 (associate_from_text)
 *   3. 强化：对关联概念之间的弱边进行强化
 *   4. 跨拓扑：语义相关但无连接的节点对创建跨拓扑连接
 *   5. 清理：全局衰减梦境激活
 *
 * 线程安全：联想引擎为梦境独立实例，边修改使用 node_locks[256]
 */

#include "dream_engine.h"
#include "associative_reasoning.h"
#include "huarong_topology.h"
#include "node_hash.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <time.h>

/* 线程本地 RNG */
static unsigned int _dream_rng_seed(void) {
    static unsigned int seed = 0;
    if (!seed) seed = (unsigned int)time(NULL) ^ (unsigned int)(uintptr_t)&seed;
    seed = seed * 1103515245 + 12345;
    return (seed >> 16) & 0x7FFF;
}

static int _dream_rand_range(int max) {
    if (max <= 0) return 0;
    return (int)(_dream_rng_seed() % (unsigned int)max);
}

#define DREAM_ASSOC_MAX_HOPS 3  /* 梦境联想跳数（比正式对话更深） */

/* ================================================================
 *  梦境核心循环
 * ================================================================ */

int dream_cycle(MasterTopology* master, MemorySystem* memory,
                const DreamConfig* config) {
    if (!master) return 0;

    DreamConfig cfg;
    if (config) { cfg = *config; }
    else { DreamConfig def = DREAM_DEFAULT_CONFIG; cfg = def; }
    (void)memory;  /* 预留 */

    SubTopology* vocab    = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    SubTopology* semantic = master_get_sub_topology_by_type(master, TOPO_SEMANTIC);
    SubTopology* emotion  = master_get_sub_topology_by_type(master, TOPO_EMOTION);
    SubTopology* concept  = master_get_sub_topology_by_type(master, TOPO_CONCEPT);
    SubTopology* context  = master_get_sub_topology_by_type(master, TOPO_CONTEXT);

    int total_mods = 0;
    int edge_boosted = 0;
    int cross_created = 0;

    /* 采样起始节点（最多32个） */
    ReasoningNode* seeds[32];
    int seed_count = 0;

    #define PICK(sub, cnt) \
        if ((sub) && (sub)->net && (sub)->net->node_count > 0) { \
            for (int _s = 0; _s < (cnt) && seed_count < 32; _s++) { \
                int _ni = _dream_rand_range((sub)->net->node_count); \
                if ((sub)->net->nodes[_ni] && (sub)->net->nodes[_ni]->concept) \
                    seeds[seed_count++] = (sub)->net->nodes[_ni]; \
            } \
        }

    PICK(vocab,    cfg.sample_vocab);
    PICK(semantic, cfg.sample_semantic);
    PICK(emotion,  cfg.sample_emotion);
    #undef PICK

    if (cfg.verbose) {
        fprintf(stderr, "[梦境] 采样 %d 个起始节点, 启动联想引擎...\n", seed_count);
    }

    /* 创建临时联想引擎（梦境专用，不影响正式对话） */
    AssociativeEngine* assoc = assoc_engine_create(master);

    for (int si = 0; si < seed_count; si++) {
        ReasoningNode* seed = seeds[si];
        if (!seed || !seed->concept) continue;

        /* 用节点概念名触发联想 */
        int n_assoc = associate_from_text(assoc, seed->concept, DREAM_ASSOC_MAX_HOPS);
        if (n_assoc <= 1) continue;  /* 只有自己，无意义 */

        /* 收集联想结果 */
        const char* assoc_concepts[50];
        float assoc_activations[50];
        int assoc_topos[50];
        int assoc_count = n_assoc < 50 ? n_assoc : 50;

        for (int ai = 0; ai < assoc_count; ai++) {
            assoc_concepts[ai] = assoc_get_concept(assoc, ai,
                &assoc_activations[ai], &assoc_topos[ai], NULL);
        }

        /* ============================================================
         *  强化：对关联概念间已有的弱边进行增强
         *  思路：如果联想路径中相邻的两个概念之间存在弱边，加重权重
         * ============================================================ */
        for (int ai = 0; ai < assoc_count - 1; ai++) {
            if (!assoc_concepts[ai] || !assoc_concepts[ai+1]) continue;

            /* 在词汇拓扑中查找这两个概念节点 */
            int t0 = assoc_topos[ai];
            int t1 = assoc_topos[ai+1];
            SubTopology* st0 = master_get_sub_topology(master, t0);
            SubTopology* st1 = master_get_sub_topology(master, t1);
            if (!st0 || !st0->net || !st1 || !st1->net) continue;

            ReasoningNode* n0 = node_hash_find(st0->node_hash, assoc_concepts[ai]);
            ReasoningNode* n1 = node_hash_find(st1->node_hash, assoc_concepts[ai+1]);
            if (!n0 || !n1 || n0 == n1) continue;

            /* 同拓扑内：检查并强化内部连接 */
            if (t0 == t1 && st0->net == st1->net) {
                for (int c = 0; c < n0->connection_count; c++) {
                    if (n0->connections[c] == n1) {
                        float w = n0->connection_weights ? n0->connection_weights[c] : 0.0f;
                        if (w < 0.4f && w > 0.0f) {
                            int lk = n0->node_id & 255;
                            pthread_mutex_lock(&st0->net->node_locks[lk]);
                            n0->connection_weights[c] += cfg.weak_edge_boost;
                            if (n0->connection_weights[c] > 1.0f)
                                n0->connection_weights[c] = 1.0f;
                            pthread_mutex_unlock(&st0->net->node_locks[lk]);
                            edge_boosted++;
                            total_mods++;
                        }
                        break;
                    }
                }
            }

            /* 跨拓扑：语义相关但无连接 → 创建跨拓扑连接 */
            if (t0 != t1) {
                if (!cross_link_exists(master, t0, n0->node_id, t1, n1->node_id)) {
                    /* 用联想激活值作为初始权重 */
                    float init_w = 0.25f + 0.25f * assoc_activations[ai+1];
                    if (init_w > 0.6f) init_w = 0.6f;
                    master_add_cross_link(master, t0, n0->node_id,
                                         t1, n1->node_id, init_w, "dream-assoc");
                    cross_created++;
                    total_mods++;
                    if (cfg.verbose) {
                        fprintf(stderr, "[梦境] 联想跨拓扑 %s(%d) ↔ %s(%d) w=%.3f\n",
                                assoc_concepts[ai], t0,
                                assoc_concepts[ai+1], t1, init_w);
                    }
                }
            }
        }
    }

    assoc_engine_free(assoc);

    /* ================================================================
     *  清理：全局衰减梦境激活
     * ================================================================ */
    #define DECAY(sub) \
        if ((sub) && (sub)->net) { \
            for (int _i = 0; _i < (sub)->net->node_count; _i++) { \
                ReasoningNode* _n = (sub)->net->nodes[_i]; \
                if (!_n || _n->activation < 0.01f) continue; \
                int _lk = _n->node_id & 255; \
                pthread_mutex_lock(&(sub)->net->node_locks[_lk]); \
                _n->activation *= cfg.dream_decay; \
                if (_n->activation < 0.01f) _n->activation = 0.0f; \
                pthread_mutex_unlock(&(sub)->net->node_locks[_lk]); \
            } \
        }

    DECAY(vocab);
    DECAY(semantic);
    DECAY(emotion);
    DECAY(concept);
    DECAY(context);
    #undef DECAY

    if (cfg.verbose && total_mods > 0) {
        fprintf(stderr, "[梦境] 完成: 强化 %d 条弱边, 创建 %d 条跨拓扑连接\n",
                edge_boosted, cross_created);
    }

    return total_mods;
}
