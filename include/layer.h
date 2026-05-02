#ifndef LAYER_H
#define LAYER_H

#include "tensor.h"
#include <stdbool.h>

// 层类型枚举
typedef enum {
    LAYER_LINEAR,          // 全连接层
    LAYER_RELU,            // ReLU激活层
    LAYER_SIGMOID,         // Sigmoid激活层
    LAYER_TANH,            // Tanh激活层
    LAYER_SOFTMAX,         // Softmax层
    LAYER_DROPOUT,         // Dropout层
    LAYER_EMBEDDING,      // 嵌入层
    LAYER_SIMPLE_RNN       // 简单RNN层
} LayerType;

// 层基类结构
typedef struct Layer {
    LayerType type;
    Tensor* weights;        // 权重
    Tensor* bias;          // 偏置
    Tensor* output;        // 输出
    Tensor* grad_weights;   // 权重梯度
    Tensor* grad_bias;     // 偏置梯度
    bool trainable;        // 是否可训练
    void* private_data;    // 私有数据
} Layer;

// RNN层私有数据
typedef struct {
    int input_size;
    int hidden_size;
    Tensor* hidden;        // 隐藏状态
    Tensor* Wh;           // 隐藏到隐藏的权重
    Tensor* Wx;           // 输入到隐藏的权重
    Tensor* bh;           // 隐藏偏置
    Tensor* d_hidden;      // 隐藏状态梯度
    Tensor* input_saved;   // 保存的输入用于反向传播
    Tensor* outputs;       // 保存每步输出
    int seq_len;          // 序列长度
} RNNData;

// 创建全连接层
Layer* layer_create_linear(size_t input_size, size_t output_size, bool trainable);

// 创建激活层
Layer* layer_create_relu();
Layer* layer_create_sigmoid();
Layer* layer_create_tanh();
Layer* layer_create_softmax();

// 创建嵌入层
Layer* layer_create_embedding(int vocab_size, int embedding_dim);

// 创建简单RNN层
Layer* layer_create_simple_rnn(int input_size, int hidden_size);

// 层前向传播
void layer_forward(Layer* layer, Tensor* input);

// 层反向传播
void layer_backward(Layer* layer, Tensor* grad_output);

// 销毁层
void layer_destroy(Layer* layer);

#endif // LAYER_H
