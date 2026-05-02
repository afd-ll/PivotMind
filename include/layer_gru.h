#ifndef LAYER_GRU_H
#define LAYER_GRU_H

#include "tensor.h"

// GRU层配置
typedef struct {
    int input_size;
    int hidden_size;
    bool use_bias;
    bool bidirectional;
} GRUConfig;

// GRU层
typedef struct GRULayer {
    int input_size;
    int hidden_size;
    bool use_bias;
    bool bidirectional;

    // 更新门权重
    Tensor* W_ir;   // 输入权重
    Tensor* W_ih;   // 隐藏权重
    Tensor* b_ir;   // 输入偏置
    Tensor* b_ih;   // 隐藏偏置

    // 重置门权重
    Tensor* W_zr;   // 输入权重
    Tensor* W_zh;   // 隐藏权重
    Tensor* b_zr;   // 输入偏置
    Tensor* b_zh;   // 隐藏偏置

    // 候选隐藏状态权重
    Tensor* W_hr;   // 输入权重
    Tensor* W_hh;   // 隐藏权重
    Tensor* b_hr;   // 输入偏置
    Tensor* b_hh;   // 隐藏偏置

    // 梯度
    Tensor* dW_ir;
    Tensor* dW_ih;
    Tensor* dW_zr;
    Tensor* dW_zh;
    Tensor* dW_hr;
    Tensor* dW_hh;

    // 缓存(用于反向传播)
    Tensor* h_prev;      // 上一时刻隐藏状态
    Tensor* r_t;         // 重置门
    Tensor* z_t;         // 更新门
    Tensor* h_tilde;     // 候选隐藏状态
    Tensor* h_t;         // 当前隐藏状态
} GRULayer;

// 创建GRU层
GRULayer* gru_layer_create(GRUConfig config);

// GRU前向传播(单步)
Tensor* gru_forward_step(GRULayer* layer, Tensor* x_t, Tensor* h_prev);

// GRU前向传播(序列)
Tensor* gru_forward_sequence(GRULayer* layer, Tensor* x_seq, Tensor* h_0);

// GRU反向传播(单步)
void gru_backward_step(GRULayer* layer, Tensor* dh_next);

// GRU反向传播(序列)
void gru_backward_sequence(GRULayer* layer, Tensor* dh_seq);

// 初始化隐藏状态
void gru_init_state(GRULayer* layer, Tensor** h_0);

// 获取隐藏状态
Tensor* gru_get_hidden_state(GRULayer* layer);

// GRU权重初始化
void gru_init_weights(GRULayer* layer);

// 销毁GRU层
void gru_layer_destroy(GRULayer* layer);

#endif // LAYER_GRU_H
