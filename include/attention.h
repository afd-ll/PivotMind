#ifndef ATTENTION_H
#define ATTENTION_H

#include "tensor.h"

// 注意力类型
typedef enum {
    ATTENTION_BAHADANAU,      // Bahdanau注意力
    ATTENTION_LUONG,          // Luong注意力
    ATTENTION_SELF,           // 自注意力
    ATTENTION_MULTIHEAD       // 多头注意力
} AttentionType;

// Bahdanau注意力(加性注意力)
typedef struct {
    Tensor* W_q;   // Query权重
    Tensor* W_k;   // Key权重
    Tensor* v;     // 向量v
    Tensor* b;     // 偏置
} BahdanauAttention;

// Luong注意力(点积注意力)
typedef struct {
    Tensor* W;     // 权重矩阵
} LuongAttention;

// 自注意力
typedef struct {
    Tensor* W_q;   // Query投影
    Tensor* W_k;   // Key投影
    Tensor* W_v;   // Value投影
    Tensor* W_o;   // 输出投影
    int num_heads;
    int head_dim;
} SelfAttention;

// 多头注意力
typedef struct {
    SelfAttention* heads;  // 多个自注意力头
    int num_heads;
    int d_model;
    int d_k;
    int d_v;
} MultiHeadAttention;

// Bahdanau注意力
BahdanauAttention* bahdanau_attention_create(int query_size, int key_size, int hidden_size);

Tensor* bahdanau_attention_forward(BahdanauAttention* attn, Tensor* query, Tensor* keys, Tensor* values);

void bahdanau_attention_backward(BahdanauAttention* attn, Tensor* grad_output);

void bahdanau_attention_destroy(BahdanauAttention* attn);

// Luong注意力
LuongAttention* luong_attention_create(int query_size, int key_size);

Tensor* luong_attention_forward(LuongAttention* attn, Tensor* query, Tensor* keys, Tensor* values);

void luong_attention_backward(LuongAttention* attn, Tensor* grad_output);

void luong_attention_destroy(LuongAttention* attn);

// 自注意力
SelfAttention* self_attention_create(int d_model, int num_heads);

Tensor* self_attention_forward(SelfAttention* attn, Tensor* query, Tensor* key, Tensor* value, bool mask);

void self_attention_backward(SelfAttention* attn, Tensor* grad_output);

void self_attention_destroy(SelfAttention* attn);

// 多头注意力
MultiHeadAttention* multihead_attention_create(int d_model, int num_heads);

Tensor* multihead_attention_forward(MultiHeadAttention* attn, Tensor* query, Tensor* key, Tensor* value, bool mask);

void multihead_attention_backward(MultiHeadAttention* attn, Tensor* grad_output);

void multihead_attention_destroy(MultiHeadAttention* attn);

// 缩放点积注意力
Tensor* scaled_dot_product_attention(Tensor* query, Tensor* key, Tensor* value, Tensor* mask, float scale);

// 位置编码
Tensor* positional_encoding(int seq_len, int d_model);

// 辅助函数
Tensor* compute_attention_weights(Tensor* scores, float temperature, Tensor* mask);

Tensor* apply_attention_weights(Tensor* weights, Tensor* values);

#endif // ATTENTION_H
