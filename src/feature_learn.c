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

#ifndef NODE_FEATURE_DIM
#define NODE_FEATURE_DIM PM_NODE_FEATURE_DIM
#endif

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

    float* new_features = (float*)calloc(total_nodes * dim, sizeof(float));
    float* weight_sums = (float*)calloc(total_nodes, sizeof(float));
    if (!new_features || !weight_sums) {
        free(new_features); free(weight_sums);
        pthread_mutex_unlock(&net->mutex); return -1;
    }

    for (int iter = 0; iter < iterations; iter++) {
        memset(new_features, 0, total_nodes * dim * sizeof(float));
        memset(weight_sums, 0, total_nodes * sizeof(float));

        /* 并行累积邻居特征贡献 (OpenMP, critical section 保护写冲突) */
        #pragma omp parallel for schedule(dynamic, 100)
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

                #pragma omp critical
                {
                    float* dst_nb = new_features + nb_id * dim;
                    float* dst_i  = new_features + i * dim;
                    float* nbf = nb->features;
                    weight_sums[nb_id] += tw;
                    weight_sums[i]     += tw;
                    for (int d = 0; d < dim; d++) {
                        dst_nb[d] += tw * src_feat[d];
                        dst_i[d]  += tw * nbf[d];
                    }
                }
            }
        }

        /* 并行归一化 */
        #pragma omp parallel for schedule(static)
        for (int i = 0; i < total_nodes; i++) {
            if (weight_sums[i] > 0.001f && net->nodes[i] && net->nodes[i]->features) {
                float inv = 1.0f / weight_sums[i];
                float* dst = net->nodes[i]->features;
                float* src = new_features + i * dim;
                for (int d = 0; d < dim; d++)
                    dst[d] = src[d] * inv;
            }
        }

        /* 快速失败检查 */
        int updated = 0;
        for (int i = 0; i < total_nodes && updated == 0; i++)
            if (weight_sums[i] > 0.001f) updated = 1;
        if (updated == 0 && iter == 0) {
            free(new_features); free(weight_sums);
            pthread_mutex_unlock(&net->mutex); return -1;
        }
    }

    free(new_features);
    free(weight_sums);

    pthread_mutex_unlock(&net->mutex);
    printf("[特征学习] 图平滑完成 (%d 节点, %d 轮)\n", total_nodes, iterations);
    return 0;
}