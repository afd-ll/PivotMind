/**
 * @file feature_learn.c
 * @brief 特征学习 — 图拉普拉斯平滑
 *
 * 从冻结伪随机数升级为有意义的语义向量：
 * 用拓扑边权重作为共现信号，迭代平滑每个节点的特征向量。
 *
 * 原理：相似于 GloVe 的一步近似，但不需求解线性方程组。
 *       O(iter × edges × feature_dim) ≈ 毫秒级。
 */

#include "feature_learn.h"
#include "constants.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#ifdef _OPENMP
#include <omp.h>
#else
static inline int omp_get_num_threads(void) { return 1; }
static inline int omp_get_thread_num(void) { return 0; }
#endif

/* FNV-1a 哈希种子 (与 huarong_topology.c 中 concept_to_feature_seed 一致) */
static void feature_seed_from_concept(const char* concept, float* feats, int dim) {
    if (!concept || !feats || dim <= 0) return;
    unsigned hash = 2166136261u;
    for (const char* p = concept; *p; p++) {
        hash ^= (unsigned char)*p;
        hash *= 16777619u;
    }
    for (int i = 0; i < dim; i++) {
        unsigned h = hash ^ (unsigned)(i * 0x9E3779B9u);
        h = (h ^ (h >> 16)) * 0x85EBCA6Bu;
        h = (h ^ (h >> 13)) * 0xC2B2AE35u;
        h = h ^ (h >> 16);
        feats[i] = ((float)(h & 0xFFFF) / 32768.0f - 1.0f) * 0.1f;
    }
}

#ifndef NODE_FEATURE_DIM
#define NODE_FEATURE_DIM PM_NODE_FEATURE_DIM
#endif

/* 线程局部聚合缓冲池（复用，避免每轮分配/释放） */
#define MAX_SMOOTH_THREADS 64
typedef struct {
    float* feats;   /* total_nodes * dim */
    float* wsums;   /* total_nodes */
} SmoothThreadBuf;

int feature_learn_graph_smooth(HuarongTopologyNet* net, int iterations) {
    if (!net || net->node_count <= 0 || iterations <= 0) return -1;

    pthread_rwlock_wrlock(&net->mutex);

    int total_nodes = net->node_count;
    int dim = NODE_FEATURE_DIM;

    /* 自动分配缺失的特征向量（首次运行时的延迟初始化） */
    int auto_allocated = 0;
    for (int i = 0; i < total_nodes; i++) {
        ReasoningNode* node = net->nodes[i];
        if (!node) continue;
        if (!node->features) {
            node->features = (float*)calloc(dim, sizeof(float));
            if (node->features) {
                feature_seed_from_concept(node->concept, node->features, dim);
                node->feature_dim = dim;
                auto_allocated++;
            }
        }
    }
    if (auto_allocated > 0) {
        printf("[特征学习] 自动初始化 %d 个节点特征向量\n", auto_allocated);
    }

    size_t feat_bytes = (size_t)total_nodes * dim * sizeof(float);
    size_t ws_bytes   = (size_t)total_nodes * sizeof(float);

    /* 线程局部缓冲区: 每线程独立累积，零锁竞争 */
    int nthreads = 1;
    float** thread_feats = NULL;
    float** thread_ws    = NULL;

    #pragma omp parallel
    {
        #pragma omp single
        {
            nthreads = omp_get_num_threads();
            if (nthreads > 1) {
                thread_feats = (float**)calloc(nthreads, sizeof(float*));
                thread_ws    = (float**)calloc(nthreads, sizeof(float*));
            }
        }
    }

    /* 全局合并缓冲区 */
    float* merged_feats = (float*)calloc(total_nodes * dim, sizeof(float));
    float* merged_ws    = (float*)calloc(total_nodes, sizeof(float));
    if (!merged_feats || !merged_ws) {
        free(merged_feats); free(merged_ws);
        free(thread_feats); free(thread_ws);
        pthread_rwlock_unlock(&net->mutex); return -1;
    }

    /* 分配线程局部缓冲区（并行区外分配，一次） */
    if (nthreads > 1) {
        for (int t = 0; t < nthreads; t++) {
            thread_feats[t] = (float*)calloc(total_nodes * dim, sizeof(float));
            thread_ws[t]    = (float*)calloc(total_nodes, sizeof(float));
            if (!thread_feats[t] || !thread_ws[t]) {
                /* 分配失败降级为 nthreads=1（串行） */
                nthreads = 1;
                for (int j = 0; j < t; j++) {
                    free(thread_feats[j]); free(thread_ws[j]);
                }
                free(thread_feats); free(thread_ws);
                thread_feats = NULL; thread_ws = NULL;
                break;
            }
        }
    }

    int iter;
    for (iter = 0; iter < iterations; iter++) {
        memset(merged_feats, 0, feat_bytes);
        memset(merged_ws, 0, ws_bytes);

        if (nthreads <= 1) {
            /* 单线程快速路径 */
            for (int i = 0; i < total_nodes; i++) {
                ReasoningNode* node = net->nodes[i];
                if (!node || !node->features || node->edge_count <= 0) continue;
                float* src_feat = node->features;
                for (int c = 0; c < node->edge_count; c++) {
                    ReasoningNode* nb = node->edges[c].target;
                    if (!nb || !nb->features || nb->node_id < 0 || nb->node_id >= total_nodes) continue;
                    int nb_id = nb->node_id;
                    float w = node->edges[c].weight;
                    float conf = (node->edges && c < node->edge_count)
                                 ? node->edges[c].confidence : 0.5f;
                    float tw = w * conf;
                    if (tw < 0.01f) continue;
                    float* dst_nb = merged_feats + nb_id * dim;
                    float* dst_i  = merged_feats + i * dim;
                    float* nbf = nb->features;
                    merged_ws[nb_id] += tw;
                    merged_ws[i]     += tw;
                    for (int d = 0; d < dim; d++) {
                        dst_nb[d] += tw * src_feat[d];
                        dst_i[d]  += tw * nbf[d];
                    }
                }
            }
        } else {
            /* 多线程: 清零局部缓冲区 */
            #pragma omp parallel for schedule(static)
            for (int t = 0; t < nthreads; t++) {
                memset(thread_feats[t], 0, feat_bytes);
                memset(thread_ws[t], 0, ws_bytes);
            }

            /* 并行累积: 每线程写入自己的缓冲区，零锁 */
            #pragma omp parallel for schedule(dynamic, 50)
            for (int i = 0; i < total_nodes; i++) {
                int tid = omp_get_thread_num();
                ReasoningNode* node = net->nodes[i];
                if (!node || !node->features || node->edge_count <= 0) continue;
                float* src_feat = node->features;
                float* loc_f = thread_feats[tid];
                float* loc_w = thread_ws[tid];

                for (int c = 0; c < node->edge_count; c++) {
                    ReasoningNode* nb = node->edges[c].target;
                    if (!nb || !nb->features || nb->node_id < 0 || nb->node_id >= total_nodes) continue;
                    int nb_id = nb->node_id;
                    float w = node->edges[c].weight;
                    float conf = (node->edges && c < node->edge_count)
                                 ? node->edges[c].confidence : 0.5f;
                    float tw = w * conf;
                    if (tw < 0.01f) continue;

                    float* dst_nb = loc_f + nb_id * dim;
                    float* dst_i  = loc_f + i * dim;
                    float* nbf = nb->features;
                    loc_w[nb_id] += tw;
                    loc_w[i]     += tw;
                    for (int d = 0; d < dim; d++) {
                        dst_nb[d] += tw * src_feat[d];
                        dst_i[d]  += tw * nbf[d];
                    }
                }
            }

            /* 并行合并: 每个节点由一线程归并所有线程的贡献 */
            #pragma omp parallel for schedule(static, 100)
            for (int i = 0; i < total_nodes; i++) {
                float* dst_f = merged_feats + i * dim;
                float ws = 0.0f;
                for (int t = 0; t < nthreads; t++) {
                    float* src_f = thread_feats[t] + i * dim;
                    ws += thread_ws[t][i];
                    if (thread_ws[t][i] > 0.0f) {
                        for (int d = 0; d < dim; d++)
                            dst_f[d] += src_f[d];
                    }
                }
                merged_ws[i] = ws;
            }
        }

        /* 并行归一化 + 收敛检测 */
        {
            float max_delta = 0.0f;
            float delta_buf[64] = {0};  /* 每线程一个 */
            #pragma omp parallel
            {
                int tid = omp_get_thread_num();
                float local_max = 0.0f;
                #pragma omp for schedule(static)
                for (int i = 0; i < total_nodes; i++) {
                    if (merged_ws[i] > 0.001f && net->nodes[i] && net->nodes[i]->features) {
                        float inv = 1.0f / merged_ws[i];
                        float* dst = net->nodes[i]->features;
                        float* src = merged_feats + i * dim;
                        /* 每 8 个节点检测一次位移（采样 12.5% 节省开销） */
                        if ((i & 7) == 0) {
                            float d = fabsf(src[0] * inv - dst[0]);
                            if (d > local_max) local_max = d;
                        }
                        for (int d = 0; d < dim; d++)
                            dst[d] = src[d] * inv;
                    }
                }
                if (tid < 64) delta_buf[tid] = local_max;
            }
            for (int t = 0; t < nthreads && t < 64; t++)
                if (delta_buf[t] > max_delta) max_delta = delta_buf[t];

            /* 收敛检测: 位移极小则提前结束后续迭代 */
            if (iter > 0 && max_delta < 0.0005f) {
                printf("[特征学习] 图平滑收敛 (iter=%d/%d, max_delta=%.6f)\n",
                       iter + 1, iterations, max_delta);
                break;
            }
        }

        int updated = 0;
        for (int i = 0; i < total_nodes && updated == 0; i++)
            if (merged_ws[i] > 0.001f) updated = 1;
        if (updated == 0 && iter == 0) {
            free(merged_feats); free(merged_ws);
            if (thread_feats) {
                for (int t = 0; t < nthreads; t++) { free(thread_feats[t]); free(thread_ws[t]); }
                free(thread_feats); free(thread_ws);
            }
            pthread_rwlock_unlock(&net->mutex); return -1;
        }
    }

    free(merged_feats);
    free(merged_ws);
    if (thread_feats) {
        for (int t = 0; t < nthreads; t++) { free(thread_feats[t]); free(thread_ws[t]); }
        free(thread_feats); free(thread_ws);
    }

    pthread_rwlock_unlock(&net->mutex);
    printf("[特征学习] 图平滑完成 (%d 节点, %d 轮)\n", total_nodes, iter + 1);
    return 0;
}