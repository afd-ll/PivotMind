#ifndef LAYER_LSTM_H
#define LAYER_LSTM_H

#include "tensor.h"
#include "layer.h"

// LSTM层配置
typedef struct {
    int input_size;
    int hidden_size;
    bool use_bias;
    bool bidirectional;
} LSTMConfig;

// LSTM层
typedef struct LSTMLayer {
    int input_size;
    int hidden_size;
    bool use_bias;
    bool bidirectional;

    // 前向权重
    Tensor* W_if;   // 输入门权重
    Tensor* W_ig;   // 遗忘门权重
    Tensor* W_io;   // 输出门权重
    Tensor* W_ii;   // 候选状态权重

    // 隐藏权重
    Tensor* R_if;   // 输入门隐藏权重
    Tensor* R_ig;   // 遗忘门隐藏权重
    Tensor* R_io;   // 输出门隐藏权重
    Tensor* R_ii;   // 候选状态隐藏权重

    // 偏置
    Tensor* b_if;
    Tensor* b_ig;
    Tensor* b_io;
    Tensor* b_ii;

    // 梯度
    Tensor* dW_if;
    Tensor* dW_ig;
    Tensor* dW_io;
    Tensor* dW_ii;
    Tensor* dR_if;
    Tensor* dR_ig;
    Tensor* dR_io;
    Tensor* dR_ii;

    // 缓存(用于反向传播)
    Tensor* x_t;         // 当前输入（用于梯度计算）
    Tensor* h_prev;      // 上一时刻隐藏状态
    Tensor* c_prev;      // 上一时刻细胞状态
    Tensor* f_t;         // 遗忘门输出
    Tensor* i_t;         // 输入门输出
    Tensor* o_t;         // 输出门输出
    Tensor* g_t;         // 候选状态
    Tensor* c_t;         // 当前细胞状态
    Tensor* h_t;         // 当前隐藏状态
} LSTMLayer;

// 创建LSTM层
LSTMLayer* lstm_layer_create(LSTMConfig config);

// LSTM前向传播(单步)
Tensor* lstm_forward_step(LSTMLayer* layer, Tensor* x_t, Tensor* h_prev, Tensor* c_prev);

// LSTM前向传播(序列)
Tensor* lstm_forward_sequence(LSTMLayer* layer, Tensor* x_seq, Tensor* h_0, Tensor* c_0);

// LSTM反向传播(单步)
void lstm_backward_step(LSTMLayer* layer, Tensor* dh_next, Tensor* dc_next);

// LSTM反向传播(序列)
void lstm_backward_sequence(LSTMLayer* layer, Tensor* dh_seq);

// 初始化隐藏状态和细胞状态
void lstm_init_state(LSTMLayer* layer, Tensor** h_0, Tensor** c_0);

// 获取隐藏状态
Tensor* lstm_get_hidden_state(LSTMLayer* layer);

// 获取细胞状态
Tensor* lstm_get_cell_state(LSTMLayer* layer);

// 销毁LSTM层
void lstm_layer_destroy(LSTMLayer* layer);

// LSTM权重初始化
void lstm_init_weights(LSTMLayer* layer);

// 梯度管理
void lstm_init_gradients(LSTMLayer* layer);
void lstm_zero_gradients(LSTMLayer* layer);

#endif // LAYER_LSTM_H
