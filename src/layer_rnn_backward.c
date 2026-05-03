#include "../include/layer.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

// RNN反向传播
void layer_rnn_backward(Layer* layer, Tensor* grad_output) {
    if (!layer || !grad_output || layer->type != LAYER_SIMPLE_RNN) return;

    RNNData* data = (RNNData*)layer->private_data;
    if (!data) return;

    int hidden_size = data->hidden_size;
    int input_size = data->input_size;
    int seq_len = data->seq_len;

    if (!data->input_saved) {
        return;
    }

    float* grad_out_data = (float*)grad_output->data;
    float* hidden_data = (float*)data->hidden->data;
    float* Wh_data = (float*)data->Wh->data;
    float* input_data = (float*)data->input_saved->data;

    // 初始化梯度
    if (!data->d_hidden) {
        data->d_hidden = tensor_zeros(DT_FLOAT32, 1, (size_t[]){hidden_size});
    }
    if (!data->Wh->grad) {
        size_t shape[] = {hidden_size, hidden_size};
        data->Wh->grad = tensor_zeros(DT_FLOAT32, 2, shape);
    }
    if (!data->Wx->grad) {
        size_t shape[] = {input_size, hidden_size};
        data->Wx->grad = tensor_zeros(DT_FLOAT32, 2, shape);
    }
    if (!data->bh->grad) {
        data->bh->grad = tensor_zeros(DT_FLOAT32, 1, (size_t[]){hidden_size});
    }

    float* d_hidden_data = (float*)data->d_hidden->data;
    float* d_Wx_data = (float*)data->Wx->grad->data;
    float* d_Wh_data = (float*)data->Wh->grad->data;
    float* d_bh_data = (float*)data->bh->grad->data;

    // 注意：不再清零梯度，允许累积
    // 梯度清零应该在样本处理完成后进行，而不是在每次反向传播时

    // 从后向前反向传播
    // 注意:当前RNN实现只保存最终隐藏状态，所以需要重构每个时间步的隐藏状态
    // 简化处理:假设seq_len=1(解码器逐步生成)，直接使用当前隐藏状态
    for (int t = seq_len - 1; t >= 0; t--) {
        float* xt = input_data + t * input_size;
        float* h_prev_zero = NULL;

        // 对于seq_len=1的情况，使用当前隐藏状态作为h_t
        // 对于seq_len>1的情况，由于没有保存所有中间状态，使用简化处理
        float* h_t = hidden_data;  // 使用最终隐藏状态

        // 如果t=0,使用零向量作为h_prev
        float* h_prev = h_prev_zero = (float*)calloc(hidden_size, sizeof(float));

        // 计算tanh的梯度: dh_out * (1 - tanh^2)
        float tanh_grad[hidden_size];
        for (int h = 0; h < hidden_size; h++) {
            float tanh_val = h_t[h];
            tanh_grad[h] = 1.0f - tanh_val * tanh_val;
        }

        // 累加输出梯度
        // 如果seq_len=1，grad_out_data只有hidden_size个元素
        // 如果seq_len>1，grad_out_data有seq_len*hidden_size个元素
        if (seq_len == 1) {
            for (int h = 0; h < hidden_size; h++) {
                d_hidden_data[h] += grad_out_data[h];
            }
        } else {
            for (int h = 0; h < hidden_size; h++) {
                d_hidden_data[h] += grad_out_data[t * hidden_size + h];
            }
        }

        // 计算偏置梯度
        for (int h = 0; h < hidden_size; h++) {
            d_bh_data[h] += d_hidden_data[h] * tanh_grad[h];
        }

        // 计算Wh梯度: dh * h_prev^T
        for (int i = 0; i < hidden_size; i++) {
            for (int j = 0; j < hidden_size; j++) {
                d_Wh_data[i * hidden_size + j] += d_hidden_data[j] * tanh_grad[j] * h_prev[i];
            }
        }

        // 计算Wx梯度: dh * x_t^T
        for (int i = 0; i < input_size; i++) {
            for (int j = 0; j < hidden_size; j++) {
                d_Wx_data[i * hidden_size + j] += d_hidden_data[j] * tanh_grad[j] * xt[i];
            }
        }

        // 计算前一隐藏状态梯度
        float* d_h_prev = (float*)malloc(hidden_size * sizeof(float));
        for (int h = 0; h < hidden_size; h++) {
            d_h_prev[h] = 0.0f;
            for (int j = 0; j < hidden_size; j++) {
                d_h_prev[h] += d_hidden_data[j] * tanh_grad[j] * Wh_data[h * hidden_size + j];
            }
        }

        // 累加到当前隐藏状态梯度(用于下一轮反向传播)
        for (int h = 0; h < hidden_size; h++) {
            d_hidden_data[h] = d_h_prev[h];
        }

        free(d_h_prev);
        free(h_prev_zero);  // 释放h_prev_zero
    }
}

// 嵌入层反向传播
void layer_embedding_backward(Layer* layer, Tensor* grad_output) {
    if (!layer || !grad_output || layer->type != LAYER_EMBEDDING) return;

    float* grad_out_data = (float*)grad_output->data;

    if (!layer->grad_weights) {
        size_t shape[] = {layer->weights->shape[0], layer->weights->shape[1]};
        layer->grad_weights = tensor_zeros(DT_FLOAT32, 2, shape);
    }

    float* grad_weights_data = (float*)layer->grad_weights->data;
    int vocab_size = layer->weights->shape[0];
    int embedding_dim = layer->weights->shape[1];
    int seq_len = grad_output->shape[0];

    // 使用保存的输入获取词ID
    Tensor* input_saved = (Tensor*)layer->private_data;
    if (!input_saved) {
        printf("      警告: 嵌入层没有保存输入\n");
        return;
    }

    float* input_data = (float*)input_saved->data;
    int input_seq_len = input_saved->shape[0];

    // 对于每个时间步,只更新对应词ID的嵌入向量
    for (int t = 0; t < seq_len && t < input_seq_len; t++) {
        int word_id = (int)input_data[t];

        if (word_id >= 0 && word_id < vocab_size) {
            for (int d = 0; d < embedding_dim; d++) {
                grad_weights_data[word_id * embedding_dim + d] +=
                    grad_out_data[t * embedding_dim + d];
            }
        }
    }
}
