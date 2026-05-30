/**
 * @file bptt_learner.c
 * @brief BPTT 在线学习器实现
 */

#include "bptt_learner.h"
#include "huarong_topology.h"
#include "utf8_tokenizer.h"
#include "context.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ==================== 创建/销毁 ====================

BpttLearner* bptt_learner_create(MasterTopology* master,
                                 int hidden_dim, int max_seq) {
    if (!master) return NULL;
    if (hidden_dim <= 0) hidden_dim = 256;
    if (max_seq <= 0) max_seq = 32;

    BpttLearner* bl = (BpttLearner*)calloc(1, sizeof(BpttLearner));
    if (!bl) return NULL;

    bl->master = master;
    bl->input_dim = NODE_FEATURE_DIM;
    bl->hidden_dim = hidden_dim;
    bl->max_seq = max_seq;
    bl->total_loss = 0.0f;
    bl->steps = 0;

    // 构建模型：RNN(input_dim → hidden_dim) → Linear(hidden_dim → input_dim)
    bl->model = model_create();
    if (!bl->model) { free(bl); return NULL; }

    Layer* rnn = layer_create_simple_rnn(bl->input_dim, bl->hidden_dim);
    if (!rnn) { model_destroy(bl->model); free(bl); return NULL; }
    model_add_layer(bl->model, rnn);

    Layer* linear = layer_create_linear(bl->hidden_dim, bl->input_dim, true);
    if (!linear) { model_destroy(bl->model); free(bl); return NULL; }
    model_add_layer(bl->model, linear);

    // Adam 优化器：小学习率，稳定在线学习
    bl->optimizer = optimizer_create_adam(0.001f, 0.9f, 0.999f, 1e-8f);
    if (!bl->optimizer) { model_destroy(bl->model); free(bl); return NULL; }

    // 工作缓冲
    size_t buf_size = (size_t)max_seq * bl->input_dim;
    bl->feat_buf_in = (float*)calloc(buf_size, sizeof(float));
    bl->feat_buf_tgt = (float*)calloc(buf_size, sizeof(float));

    printf("[BPTT] 创建: RNN(%d→%d)→Linear(%d→%d), Adam lr=0.001\n",
           bl->input_dim, bl->hidden_dim, bl->hidden_dim, bl->input_dim);
    return bl;
}

void bptt_learner_destroy(BpttLearner* bl) {
    if (!bl) return;
    if (bl->model) model_destroy(bl->model);
    if (bl->optimizer) optimizer_destroy(bl->optimizer);
    free(bl->feat_buf_in);
    free(bl->feat_buf_tgt);
    free(bl);
}

// ==================== 辅助：从文本提取特征序列 ====================

/**
 * 将 UTF-8 文本映射为节点特征序列
 * 在词汇拓扑中查找每个字对应的节点，取其 features[NODE_FEATURE_DIM]
 * 返回实际序列长度
 */
static int text_to_features(MasterTopology* master, const char* text,
                            float* feat_out, int max_seq, int feat_dim) {
    if (!master || !text || !feat_out) return 0;

    // 找到词汇拓扑
    SubTopology* vocab = NULL;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (sub && sub->type == TOPO_VOCABULARY) {
            vocab = sub;
            break;
        }
    }
    if (!vocab || !vocab->net) return 0;

    const char* p = text;
    int seq = 0;

    while (*p && seq < max_seq) {
        int clen = utf8_char_len((unsigned char)*p);
        if (clen <= 0) { p++; continue; }

        // 跳过空白
        if (clen == 1 && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            p++;
            continue;
        }

        // 提取单字
        char ch[8] = {0};
        memcpy(ch, p, clen);
        ch[clen] = '\0';

        // 在词汇拓扑中查找节点
        ReasoningNode* node = NULL;
        if (vocab->node_hash) {
            node = node_hash_find(vocab->node_hash, ch);
        }
        if (!node) {
            // 未找到 → 零向量（节点尚未创建）
            memset(feat_out + seq * feat_dim, 0, feat_dim * sizeof(float));
        } else if (node->features) {
            memcpy(feat_out + seq * feat_dim, node->features,
                   feat_dim * sizeof(float));
        } else {
            memset(feat_out + seq * feat_dim, 0, feat_dim * sizeof(float));
        }

        seq++;
        p += clen;
    }

    return seq;
}

// ==================== 核心：从对话中学习 ====================

float bptt_learn_from_dialog(BpttLearner* bl,
                             const char* user_input,
                             const char* ai_response) {
    if (!bl || !bl->model || !user_input || !ai_response) return -1.0f;
    if (strlen(user_input) == 0 || strlen(ai_response) == 0) return -1.0f;

    int feat_dim = bl->input_dim;
    int max_seq = bl->max_seq;

    // 文本 → 特征序列
    int in_len = text_to_features(bl->master, user_input,
                                  bl->feat_buf_in, max_seq, feat_dim);
    int tgt_len = text_to_features(bl->master, ai_response,
                                   bl->feat_buf_tgt, max_seq, feat_dim);
    if (in_len == 0 || tgt_len == 0) return -1.0f;

    // 使用较短的序列长度
    int seq_len = (in_len < tgt_len) ? in_len : tgt_len;
    if (seq_len < 2) return -1.0f;

    // 构建输入张量 [seq_len, feat_dim]
    size_t in_shape[] = { (size_t)seq_len, (size_t)feat_dim };
    Tensor* input = tensor_create_from_data(DT_FLOAT32, 2, in_shape, bl->feat_buf_in);
    if (!input) return -1.0f;

    // 构建目标张量 [seq_len, feat_dim]
    Tensor* target = tensor_create_from_data(DT_FLOAT32, 2, in_shape, bl->feat_buf_tgt);
    if (!target) { tensor_destroy(input); return -1.0f; }

    // 前向传播
    model_set_mode((void*)bl->model, MODE_TRAINING);
    Tensor* output = model_forward(bl->model, input);
    if (!output) {
        tensor_destroy(input);
        tensor_destroy(target);
        return -1.0f;
    }

    // 损失计算（MSE）
    Tensor* loss_tensor = model_mse_loss(output, target);
    float loss = 0.0f;
    if (loss_tensor) {
        float* loss_data = (float*)loss_tensor->data;
        if (loss_data) loss = loss_data[0];
        tensor_destroy(loss_tensor);
    }

    // 梯度计算（dL/dy = 2*(y - target)/N）
    float* out_data = (float*)output->data;
    float* tgt_data = (float*)target->data;
    size_t total_elems = output->size;
    float scale = 2.0f / (float)total_elems;

    // 分配梯度张量
    Tensor* grad_output = tensor_zeros(DT_FLOAT32, 2, in_shape);
    if (grad_output) {
        float* grad_data = (float*)grad_output->data;
        for (size_t i = 0; i < total_elems; i++) {
            grad_data[i] = scale * (out_data[i] - tgt_data[i]);
        }

        // 逐层反向传播
        for (int li = (int)bl->model->num_layers - 1; li >= 0; li--) {
            Layer* layer = bl->model->layers[li];
            layer_backward(layer, grad_output);
            if (layer->output && layer->output->grad) {
                grad_output = layer->output->grad;
            } else {
                break;
            }
        }

        tensor_destroy(grad_output);
    }

    // 优化器步进
    {
        Tensor** params = NULL;
        size_t num_params = 0;
        model_get_trainable_params(bl->model, &params, &num_params);
        if (params && num_params > 0) {
            optimizer_step(bl->optimizer, params, num_params);
        }
    }

    // 累计统计
    bl->total_loss += loss;
    bl->steps++;

    tensor_destroy(input);
    tensor_destroy(target);
    tensor_destroy(output);

    return loss;
}

// ==================== 统计 ====================

void bptt_learner_stats(BpttLearner* bl, float* out_avg_loss, int* out_steps) {
    if (!bl) return;
    if (out_avg_loss) {
        *out_avg_loss = (bl->steps > 0) ? bl->total_loss / bl->steps : 0.0f;
    }
    if (out_steps) *out_steps = bl->steps;
}
