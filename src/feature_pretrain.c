/**
 * @file feature_pretrain.c
 * @brief 预训练嵌入 → 拓扑特征向量迁移桥接
 *
 * 将 Word2Vec (Skip-gram/CBOW) 预训练的高维嵌入
 * 通过随机投影 (Random Projection) 降维到 NODE_FEATURE_DIM,
 * 注入每个节点的 features 数组。
 *
 * 数学原理: Johnson-Lindenstrauss 引理保证随机投影
 * 在高概率下近似保持向量间的余弦相似度。
 */

#include "feature_pretrain.h"
#include "common.h"
#include "vocab.h"
#include "tensor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int feature_transfer_pretrained(MasterTopology* master, PretrainState* pretrain) {
    if (!master || !pretrain || !pretrain->vocab || !pretrain->embedding_layer) return -1;

    Layer* emb_layer = pretrain->embedding_layer;
    if (!emb_layer->weights || emb_layer->weights->ndim < 2) return -1;

    int src_dim = emb_layer->weights->shape[1];   /* 原始嵌入维度 (64/128) */
    int dst_dim = NODE_FEATURE_DIM;
    int transferred = 0;

    /* 创建随机投影矩阵 (dst_dim × src_dim)
     * 每个元素 ~ N(0, 1/sqrt(dst_dim)) 近似, 用均匀分布代替 */
    float* proj = (float*)malloc(dst_dim * src_dim * sizeof(float));
    if (!proj) return -1;

    /* 固定种子 42 确保可复现 */
    float proj_scale = 1.0f / sqrtf((float)dst_dim);
    for (int i = 0; i < dst_dim * src_dim; i++) {
        unsigned int seed = 42 + i;
        int r = (seed * 1103515245 + 12345) & 0x7FFFFFFF;
        proj[i] = proj_scale * ((float)r / (float)0x7FFFFFFF * 2.0f - 1.0f);
    }

    /* 临时缓冲区 */
    float* src_vec = (float*)malloc(src_dim * sizeof(float));
    if (!src_vec) {
        free(proj);
        return -1;
    }

    /* 遍历所有拓扑的所有节点 */
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;

        for (int i = 0; i < sub->net->node_count; i++) {
            ReasoningNode* node = sub->net->nodes[i];
            if (!node || !node->concept || strlen(node->concept) == 0) continue;

            /* 在预训练词表中查找 */
            if (pretrain_get_embedding(pretrain, node->concept, src_vec) != 0) continue;

            /* 确保节点有 features 缓冲区 */
            if (!node->features || node->feature_dim < dst_dim) {
                if (!node->features) {
                    node->features = (float*)malloc(dst_dim * sizeof(float));
                } else if (node->feature_dim < dst_dim) {
                    float* new_feat = (float*)realloc(node->features, dst_dim * sizeof(float));
                    if (!new_feat) continue;  /* realloc 失败，旧指针仍有效但尺寸不足，跳过 */
                    node->features = new_feat;
                }
                if (!node->features) continue;
                node->feature_dim = dst_dim;
            }

            /* 随机投影: dst[i] = Σ_j proj[i*src_dim + j] * src[j] */
            for (int d = 0; d < dst_dim; d++) {
                float val = 0.0f;
                for (int s = 0; s < src_dim; s++) {
                    val += proj[d * src_dim + s] * src_vec[s];
                }
                node->features[d] = val;
            }
            transferred++;
        }
    }

    free(src_vec);
    free(proj);

    if (transferred == 0) return -1;
    printf("[特征迁移] 预训练嵌入 → 拓扑特征: %d 节点 (src_dim=%d → dst_dim=%d)\n",
           transferred, src_dim, dst_dim);
    return transferred;
}
