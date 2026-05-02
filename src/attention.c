#include "../include/common.h"
#include "../include/attention.h"
#include "../include/error.h"

// 缩放点积注意力
Tensor* scaled_dot_product_attention(Tensor* query, Tensor* key, Tensor* value, Tensor* mask, float scale) {
    CHECK_NULL_RETURN(query, NULL);
    CHECK_NULL_RETURN(key, NULL);
    CHECK_NULL_RETURN(value, NULL);

    // query: (batch_size, seq_len_q, d_k)
    // key: (batch_size, seq_len_k, d_k)
    // value: (batch_size, seq_len_v, d_v)
    // 通常 seq_len_k == seq_len_v

    // 计算注意力分数: scores = Q @ K^T / sqrt(d_k)
    Tensor* key_transposed = tensor_transpose(key, 1, 2);
    Tensor* scores = tensor_matmul(query, key_transposed);

    // 缩放
    if (scale > 0) {
        float* scores_data = (float*)scores->data;
        for (size_t i = 0; i < scores->size; i++) {
            scores_data[i] /= scale;
        }
    }

    // 应用mask(如果提供)
    if (mask) {
        float* scores_data = (float*)scores->data;
        float* mask_data = (float*)mask->data;

        // 将被mask的位置设为负无穷大
        for (size_t i = 0; i < scores->size; i++) {
            if (mask_data[i] < 0.5f) {  // mask为0表示需要mask
                scores_data[i] = -1e9f;
            }
        }
    }

    // Softmax归一化
    Tensor* attention_weights = tensor_softmax(scores, 2);

    // 应用注意力权重到value
    Tensor* output = tensor_matmul(attention_weights, value);

    // 清理
    tensor_destroy(key_transposed);
    tensor_destroy(scores);
    tensor_destroy(attention_weights);

    return output;
}

// Bahdanau注意力创建
BahdanauAttention* bahdanau_attention_create(int query_size, int key_size, int hidden_size) {
    BahdanauAttention* attn = malloc(sizeof(BahdanauAttention));
    if (!attn) {
        LOG_ERROR("Failed to allocate Bahdanau attention");
        return NULL;
    }

    size_t wq_shape[] = {(size_t)query_size, (size_t)hidden_size};
    size_t wk_shape[] = {(size_t)key_size, (size_t)hidden_size};
    size_t v_shape[] = {(size_t)hidden_size, 1};
    size_t b_shape[] = {(size_t)hidden_size};

    attn->W_q = tensor_create(DT_FLOAT32, 2, wq_shape);
    attn->W_k = tensor_create(DT_FLOAT32, 2, wk_shape);
    attn->v = tensor_create(DT_FLOAT32, 2, v_shape);
    attn->b = tensor_zeros(DT_FLOAT32, 1, b_shape);

    // Initialize random seed
    init_random();

    float* wq_data = (float*)attn->W_q->data;
    float* wk_data = (float*)attn->W_k->data;
    float* v_data = (float*)attn->v->data;

    for (size_t i = 0; i < attn->W_q->size; i++) {
        wq_data[i] = xavier_init(query_size, hidden_size);
    }
    for (size_t i = 0; i < attn->W_k->size; i++) {
        wk_data[i] = xavier_init(key_size, hidden_size);
    }
    for (size_t i = 0; i < attn->v->size; i++) {
        v_data[i] = xavier_init(hidden_size, 1);
    }

    LOG_INFO("Bahdanau attention created: query=%d, key=%d, hidden=%d",
             query_size, key_size, hidden_size);

    return attn;
}

// Bahdanau注意力前向传播
Tensor* bahdanau_attention_forward(BahdanauAttention* attn, Tensor* query, Tensor* keys, Tensor* values) {
    CHECK_NULL_RETURN(attn, NULL);
    CHECK_NULL_RETURN(query, NULL);
    CHECK_NULL_RETURN(keys, NULL);
    CHECK_NULL_RETURN(values, NULL);

    // Bahdanau注意力: score = v^T * tanh(W_q * q + W_k * k + b)

    // 投影query
    Tensor* W_q_q = tensor_matmul(query, attn->W_q);

    // 投影keys
    Tensor* W_k_k = tensor_matmul(keys, attn->W_k);

    // 相加
    Tensor* sum = tensor_add(W_q_q, W_k_k);

    // 加偏置
    Tensor* sum_bias = tensor_add(sum, attn->b);

    // Tanh
    Tensor* tanh_out = tensor_tanh(sum_bias);

    // 乘以v
    Tensor* scores = tensor_matmul(tanh_out, attn->v);

    // Softmax
    Tensor* attention_weights = tensor_softmax(scores, 1);

    // 应用到values
    Tensor* context = tensor_matmul(attention_weights, values);

    // 清理
    tensor_destroy(W_q_q);
    tensor_destroy(W_k_k);
    tensor_destroy(sum);
    tensor_destroy(sum_bias);
    tensor_destroy(tanh_out);
    tensor_destroy(scores);
    tensor_destroy(attention_weights);

    return context;
}

// Luong注意力创建
LuongAttention* luong_attention_create(int query_size, int key_size) {
    LuongAttention* attn = malloc(sizeof(LuongAttention));
    if (!attn) {
        LOG_ERROR("Failed to allocate Luong attention");
        return NULL;
    }

    size_t w_shape[] = {(size_t)query_size, (size_t)key_size};
    attn->W = tensor_create(DT_FLOAT32, 2, w_shape);

    // Initialize random seed
    init_random();

    float* w_data = (float*)attn->W->data;
    for (size_t i = 0; i < attn->W->size; i++) {
        w_data[i] = glorot_init(query_size, key_size);
    }

    LOG_INFO("Luong attention created: query=%d, key=%d", query_size, key_size);

    return attn;
}

// Luong注意力前向传播
Tensor* luong_attention_forward(LuongAttention* attn, Tensor* query, Tensor* keys, Tensor* values) {
    CHECK_NULL_RETURN(attn, NULL);
    CHECK_NULL_RETURN(query, NULL);
    CHECK_NULL_RETURN(keys, NULL);
    CHECK_NULL_RETURN(values, NULL);

    // Luong注意力(通用型): score = q @ W @ k

    // 投影query
    Tensor* W_q = tensor_matmul(query, attn->W);

    // 计算注意力分数
    Tensor* scores = tensor_matmul(W_q, keys);

    // Softmax
    Tensor* attention_weights = tensor_softmax(scores, 1);

    // 应用到values
    Tensor* context = tensor_matmul(attention_weights, values);

    // 清理
    tensor_destroy(W_q);
    tensor_destroy(scores);
    tensor_destroy(attention_weights);

    return context;
}

// 位置编码
Tensor* positional_encoding(int seq_len, int d_model) {
    size_t shape[] = {(size_t)seq_len, (size_t)d_model};
    Tensor* pos = tensor_create(DT_FLOAT32, 2, shape);

    float* pos_data = (float*)pos->data;

    for (int pos_i = 0; pos_i < seq_len; pos_i++) {
        for (int i = 0; i < d_model; i++) {
            float freq = powf(10000.0f, -2.0f * i / d_model);

            if (i % 2 == 0) {
                pos_data[pos_i * d_model + i] = sinf(freq * pos_i);
            } else {
                pos_data[pos_i * d_model + i] = cosf(freq * pos_i);
            }
        }
    }

    return pos;
}

// 自注意力创建
SelfAttention* self_attention_create(int d_model, int num_heads) {
    SelfAttention* attn = malloc(sizeof(SelfAttention));
    if (!attn) {
        LOG_ERROR("Failed to allocate self attention");
        return NULL;
    }

    attn->num_heads = num_heads;
    attn->head_dim = d_model / num_heads;

    size_t w_shape[] = {(size_t)d_model, (size_t)d_model};

    attn->W_q = tensor_create(DT_FLOAT32, 2, w_shape);
    attn->W_k = tensor_create(DT_FLOAT32, 2, w_shape);
    attn->W_v = tensor_create(DT_FLOAT32, 2, w_shape);
    attn->W_o = tensor_create(DT_FLOAT32, 2, w_shape);

    // Initialize random seed
    init_random();

    float init_scale = sqrtf(1.0f / d_model);

    float* wq_data = (float*)attn->W_q->data;
    float* wk_data = (float*)attn->W_k->data;
    float* wv_data = (float*)attn->W_v->data;
    float* wo_data = (float*)attn->W_o->data;

    for (size_t i = 0; i < attn->W_q->size; i++) {
        wq_data[i] = ((float)rand() / (float)RAND_MAX * 2.0f - 1.0f) * init_scale;
    }
    for (size_t i = 0; i < attn->W_k->size; i++) {
        wk_data[i] = ((float)rand() / (float)RAND_MAX * 2.0f - 1.0f) * init_scale;
    }
    for (size_t i = 0; i < attn->W_v->size; i++) {
        wv_data[i] = ((float)rand() / (float)RAND_MAX * 2.0f - 1.0f) * init_scale;
    }
    for (size_t i = 0; i < attn->W_o->size; i++) {
        wo_data[i] = ((float)rand() / (float)RAND_MAX * 2.0f - 1.0f) * init_scale;
    }

    LOG_INFO("Self attention created: d_model=%d, num_heads=%d", d_model, num_heads);

    return attn;
}

// 自注意力前向传播
Tensor* self_attention_forward(SelfAttention* attn, Tensor* query, Tensor* key, Tensor* value, bool /*mask*/) {
    CHECK_NULL_RETURN(attn, NULL);
    CHECK_NULL_RETURN(query, NULL);
    CHECK_NULL_RETURN(key, NULL);
    CHECK_NULL_RETURN(value, NULL);

    // 投影
    Tensor* Q = tensor_matmul(query, attn->W_q);
    Tensor* K = tensor_matmul(key, attn->W_k);
    Tensor* V = tensor_matmul(value, attn->W_v);

    // 缩放
    float scale = 1.0f / sqrtf((float)attn->head_dim);

    // 缩放点积注意力
    Tensor* mask_tensor = NULL;  // 简化:不实现复杂的mask
    Tensor* attention_output = scaled_dot_product_attention(Q, K, V, mask_tensor, scale);

    // 输出投影
    Tensor* output = tensor_matmul(attention_output, attn->W_o);

    // 清理
    tensor_destroy(Q);
    tensor_destroy(K);
    tensor_destroy(V);
    tensor_destroy(attention_output);

    return output;
}

// 多头注意力创建
MultiHeadAttention* multihead_attention_create(int d_model, int num_heads) {
    MultiHeadAttention* attn = malloc(sizeof(MultiHeadAttention));
    if (!attn) {
        LOG_ERROR("Failed to allocate multihead attention");
        return NULL;
    }

    attn->num_heads = num_heads;
    attn->d_model = d_model;
    attn->d_k = d_model / num_heads;
    attn->d_v = d_model / num_heads;

    // 这里简化:实际应该创建多个独立的自注意力头
    attn->heads = NULL;

    LOG_INFO("Multihead attention created: d_model=%d, num_heads=%d", d_model, num_heads);

    return attn;
}

// 多头注意力前向传播
Tensor* multihead_attention_forward(MultiHeadAttention* attn, Tensor* query, Tensor* key, Tensor* value, bool /*mask*/) {
    CHECK_NULL_RETURN(attn, NULL);
    CHECK_NULL_RETURN(query, NULL);
    CHECK_NULL_RETURN(key, NULL);
    CHECK_NULL_RETURN(value, NULL);

    // TODO: 实现完整的多头注意力
    LOG_WARNING("Multihead attention forward pass not fully implemented");

    // 简化:返回零张量
    size_t output_shape[] = {query->shape[0], query->shape[1], (size_t)attn->d_model};
    return tensor_zeros(DT_FLOAT32, 3, output_shape);
}

// 销毁函数
void bahdanau_attention_destroy(BahdanauAttention* attn) {
    if (!attn) return;
    tensor_destroy(attn->W_q);
    tensor_destroy(attn->W_k);
    tensor_destroy(attn->v);
    tensor_destroy(attn->b);
    free(attn);
}

void luong_attention_destroy(LuongAttention* attn) {
    if (!attn) return;
    tensor_destroy(attn->W);
    free(attn);
}

void self_attention_destroy(SelfAttention* attn) {
    if (!attn) return;
    tensor_destroy(attn->W_q);
    tensor_destroy(attn->W_k);
    tensor_destroy(attn->W_v);
    tensor_destroy(attn->W_o);
    free(attn);
}

void multihead_attention_destroy(MultiHeadAttention* attn) {
    if (!attn) return;
    if (attn->heads) {
        for (int i = 0; i < attn->num_heads; i++) {
            self_attention_destroy(&attn->heads[i]);
        }
        free(attn->heads);
    }
    free(attn);
}

void bahdanau_attention_backward(BahdanauAttention* /*attn*/, Tensor* /*grad_output*/) {
    LOG_WARNING("Bahdanau attention backward not implemented");
}

void luong_attention_backward(LuongAttention* /*attn*/, Tensor* /*grad_output*/) {
    LOG_WARNING("Luong attention backward not implemented");
}

void self_attention_backward(SelfAttention* /*attn*/, Tensor* /*grad_output*/) {
    LOG_WARNING("Self attention backward not implemented");
}

void multihead_attention_backward(MultiHeadAttention* /*attn*/, Tensor* /*grad_output*/) {
    LOG_WARNING("Multihead attention backward not implemented");
}
