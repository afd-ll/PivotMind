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
#include <omp.h>

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

    pthread_mutex_lock(&net->mutex);

    int total_nodes = net->node_count;
    int dim = NODE_FEATURE_DIM;

    int has_features = 0;
    for (int i = 0; i < total_nodes; i++) {
        if (net->nodes[i] && net->nodes[i]->features) { has_features = 1; break; }
    }
    if (!has_features) { pthread_mutex_unlock(&net->mutex); return -1; }

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
        pthread_mutex_unlock(&net->mutex); return -1;
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

    for (int iter = 0; iter < iterations; iter++) {
        memset(merged_feats, 0, feat_bytes);
        memset(merged_ws, 0, ws_bytes);

        if (nthreads <= 1) {
            /* 单线程快速路径 */
            for (int i = 0; i < total_nodes; i++) {
                ReasoningNode* node = net->nodes[i];
                if (!node || !node->features || node->connection_count <= 0) continue;
                float* src_feat = node->features;
                for (int c = 0; c < node->connection_count; c++) {
                    ReasoningNode* nb = node->connections[c];
                    if (!nb || !nb->features || nb->node_id < 0 || nb->node_id >= total_nodes) continue;
                    int nb_id = nb->node_id;
                    float w = node->connection_weights[c];
                    float conf = (node->connection_confidences && c < node->connection_count)
                                 ? node->connection_confidences[c] : 0.5f;
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
                if (!node || !node->features || node->connection_count <= 0) continue;
                float* src_feat = node->features;
                float* loc_f = thread_feats[tid];
                float* loc_w = thread_ws[tid];

                for (int c = 0; c < node->connection_count; c++) {
                    ReasoningNode* nb = node->connections[c];
                    if (!nb || !nb->features || nb->node_id < 0 || nb->node_id >= total_nodes) continue;
                    int nb_id = nb->node_id;
                    float w = node->connection_weights[c];
                    float conf = (node->connection_confidences && c < node->connection_count)
                                 ? node->connection_confidences[c] : 0.5f;
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

        /* 并行归一化 */
        #pragma omp parallel for schedule(static)
        for (int i = 0; i < total_nodes; i++) {
            if (merged_ws[i] > 0.001f && net->nodes[i] && net->nodes[i]->features) {
                float inv = 1.0f / merged_ws[i];
                float* dst = net->nodes[i]->features;
                float* src = merged_feats + i * dim;
                for (int d = 0; d < dim; d++)
                    dst[d] = src[d] * inv;
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
            pthread_mutex_unlock(&net->mutex); return -1;
        }
    }

    free(merged_feats);
    free(merged_ws);
    if (thread_feats) {
        for (int t = 0; t < nthreads; t++) { free(thread_feats[t]); free(thread_ws[t]); }
        free(thread_feats); free(thread_ws);
    }

    pthread_mutex_unlock(&net->mutex);
    printf("[特征学习] 图平滑完成 (%d 节点, %d 轮)\n", total_nodes, iterations);
    return 0;
}