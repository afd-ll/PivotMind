#include "../include/layer.h"
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <stdio.h>

// 创建嵌入层
Layer* layer_create_embedding(int vocab_size, int embedding_dim) {
    Layer* layer = malloc(sizeof(Layer));
    if (!layer) return NULL;

    layer->type = LAYER_EMBEDDING;
    layer->trainable = true;
    layer->output = NULL;
    layer->grad_weights = NULL;
    layer->grad_bias = NULL;
    layer->private_data = NULL;

    // 创建嵌入矩阵 (vocab_size x embedding_dim)
    size_t weight_shape[] = {vocab_size, embedding_dim};
    layer->weights = tensor_create(DT_FLOAT32, 2, weight_shape);
    layer->bias = NULL;

    // 随机初始化 - 使用更大的范围
    float* weight_data = (float*)layer->weights->data;
    float scale = sqrtf(1.0f / embedding_dim) * 2.0f;  // Xavier初始化，增加2倍
    for (size_t i = 0; i < layer->weights->size; i++) {
        weight_data[i] = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * scale;
    }

    layer->weights->requires_grad = true;

    return layer;
}

// 嵌入层前向传播
void layer_embedding_forward(Layer* layer, Tensor* input) {
    if (!layer || !input) return;

    // 保存输入用于反向传播
    if (layer->private_data) {
        tensor_destroy(layer->private_data);
    }
    layer->private_data = tensor_clone(input);

    float* input_data = (float*)input->data;
    float* weight_data = (float*)layer->weights->data;
    int vocab_size = layer->weights->shape[0];
    int embedding_dim = layer->weights->shape[1];
    int seq_len = input->shape[0];

    // 创建输出 (seq_len x embedding_dim)
    size_t output_shape[] = {seq_len, embedding_dim};
    if (layer->output) tensor_destroy(layer->output);
    layer->output = tensor_zeros(DT_FLOAT32, 2, output_shape);
    float* output_data = (float*)layer->output->data;

    // 查找每个词ID的嵌入向量
    for (int t = 0; t < seq_len; t++) {
        int word_id = (int)input_data[t];
        if (word_id >= 0 && word_id < vocab_size) {
            for (int d = 0; d < embedding_dim; d++) {
                output_data[t * embedding_dim + d] =
                    weight_data[word_id * embedding_dim + d];
            }
        }
    }
}

// 创建简单RNN层
Layer* layer_create_simple_rnn(int input_size, int hidden_size) {
    Layer* layer = malloc(sizeof(Layer));
    if (!layer) return NULL;

    layer->type = LAYER_SIMPLE_RNN;
    layer->trainable = true;
    layer->output = NULL;
    layer->grad_weights = NULL;
    layer->grad_bias = NULL;

    // 分配私有数据
    RNNData* data = (RNNData*)malloc(sizeof(RNNData));
    data->input_size = input_size;
    data->hidden_size = hidden_size;

    // 创建权重
    size_t Wx_shape[] = {input_size, hidden_size};
    data->Wx = tensor_create(DT_FLOAT32, 2, Wx_shape);

    size_t Wh_shape[] = {hidden_size, hidden_size};
    data->Wh = tensor_create(DT_FLOAT32, 2, Wh_shape);

    data->bh = tensor_zeros(DT_FLOAT32, 1, (size_t[]){hidden_size});
    data->hidden = tensor_zeros(DT_FLOAT32, 1, (size_t[]){hidden_size});
    data->input_saved = NULL;
    data->d_hidden = NULL;
    data->outputs = NULL;
    data->seq_len = 0;

    // Xavier初始化 - 增加缩放因子
    float scale = sqrtf(2.0f / (input_size + hidden_size)) * 3.0f;

    float* Wx_data = (float*)data->Wx->data;
    for (size_t i = 0; i < data->Wx->size; i++) {
        Wx_data[i] = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * scale;
    }

    float* Wh_data = (float*)data->Wh->data;
    for (size_t i = 0; i < data->Wh->size; i++) {
        Wh_data[i] = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * scale;
    }

    data->Wx->requires_grad = true;
    data->Wh->requires_grad = true;
    data->bh->requires_grad = true;

    layer->private_data = data;

    return layer;
}

// RNN层前向传播（计算单步或序列的隐藏状态输出）
void layer_rnn_forward(Layer* layer, Tensor* input) {
    if (!layer || !input) return;

    RNNData* data = (RNNData*)layer->private_data;
    int seq_len = input->shape[0];
    int input_size = data->input_size;
    int hidden_size = data->hidden_size;

    // printf("RNN forward: seq_len=%d, input_size=%d, hidden_size=%d\n",
    //        seq_len, input_size, hidden_size);
    // fflush(stdout);

    float* input_data = (float*)input->data;
    float* Wx_data = (float*)data->Wx->data;
    float* Wh_data = (float*)data->Wh->data;
    float* bh_data = (float*)data->bh->data;
    float* hidden_data = (float*)data->hidden->data;

    // 如果序列长度为1，不重置隐藏状态（用于解码器的逐步生成）
    // 否则初始化隐藏状态为零（用于编码器）
    if (seq_len > 1) {
        memset(hidden_data, 0, hidden_size * sizeof(float));
    }

    // 保存输入用于反向传播 - 始终保存
    if (data->input_saved) tensor_destroy(data->input_saved);
    data->input_saved = tensor_clone(input);
    data->seq_len = seq_len;

    // 逐时间步计算
    for (int t = 0; t < seq_len; t++) {
        float* xt = input_data + t * input_size;

        // 保存上一时间步的隐藏状态副本
        float* h_prev = (float*)malloc(hidden_size * sizeof(float));
        memcpy(h_prev, hidden_data, hidden_size * sizeof(float));

        // h_t = tanh(Wx * x_t + Wh * h_{t-1} + bh)
        for (int h = 0; h < hidden_size; h++) {
            float sum = bh_data[h];

            // 输入到隐藏
            for (int i = 0; i < input_size; i++) {
                sum += xt[i] * Wx_data[i * hidden_size + h];
            }

            // 隐藏到隐藏 (使用上一时间步的隐藏状态)
            for (int hh = 0; hh < hidden_size; hh++) {
                sum += h_prev[hh] * Wh_data[hh * hidden_size + h];
            }

            // tanh激活
            hidden_data[h] = tanhf(sum);
        }

        free(h_prev);
    }

    // 输出最后的隐藏状态 (二维张量: 1 x hidden_size)
    if (layer->output) tensor_destroy(layer->output);
    size_t output_shape[] = {1, hidden_size};
    layer->output = tensor_create(DT_FLOAT32, 2, output_shape);
    float* output_data = (float*)layer->output->data;
    memcpy(output_data, hidden_data, hidden_size * sizeof(float));

    // printf("RNN forward completed: output shape [%zu, %zu]\n",
    //        layer->output->shape[0], layer->output->shape[1]);
    // fflush(stdout);
}
