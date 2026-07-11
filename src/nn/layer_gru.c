#include "layer.h"
#include "common.h"
#include "layer_gru.h"
#include "error.h"

// 创建GRU层
GRULayer* gru_layer_create(GRUConfig config) {
    GRULayer* layer = malloc(sizeof(GRULayer));
    if (!layer) {
        LOG_ERROR("Failed to allocate GRU layer");
        return NULL;
    }

    layer->input_size = config.input_size;
    layer->hidden_size = config.hidden_size;
    layer->use_bias = config.use_bias;
    layer->bidirectional = config.bidirectional;

    // Initialize random seed
    init_random();

    // 创建权重矩阵
    size_t input_shape[] = {(size_t)config.input_size, (size_t)config.hidden_size};
    size_t hidden_shape[] = {(size_t)config.hidden_size, (size_t)config.hidden_size};
    size_t bias_shape[] = {(size_t)config.hidden_size};

    // 更新门 (update gate)
    layer->W_ir = tensor_create(DT_FLOAT32, 2, input_shape);
    layer->W_ih = tensor_create(DT_FLOAT32, 2, hidden_shape);

    // 重置门 (reset gate)
    layer->W_zr = tensor_create(DT_FLOAT32, 2, input_shape);
    layer->W_zh = tensor_create(DT_FLOAT32, 2, hidden_shape);

    // 候选隐藏状态 (candidate hidden state)
    layer->W_hr = tensor_create(DT_FLOAT32, 2, input_shape);
    layer->W_hh = tensor_create(DT_FLOAT32, 2, hidden_shape);

    if (config.use_bias) {
        layer->b_ir = tensor_zeros(DT_FLOAT32, 1, bias_shape);
        layer->b_ih = tensor_zeros(DT_FLOAT32, 1, bias_shape);
        layer->b_zr = tensor_zeros(DT_FLOAT32, 1, bias_shape);
        layer->b_zh = tensor_zeros(DT_FLOAT32, 1, bias_shape);
        layer->b_hr = tensor_zeros(DT_FLOAT32, 1, bias_shape);
        layer->b_hh = tensor_zeros(DT_FLOAT32, 1, bias_shape);
    } else {
        layer->b_ir = NULL;
        layer->b_ih = NULL;
        layer->b_zr = NULL;
        layer->b_zh = NULL;
        layer->b_hr = NULL;
        layer->b_hh = NULL;
    }

    // 初始化缓存为NULL
    layer->x_t = NULL;
    layer->h_prev = NULL;
    layer->r_t = NULL;
    layer->z_t = NULL;
    layer->h_tilde = NULL;
    layer->h_t = NULL;
    layer->dh_prev_out = NULL;

    // 权重梯度(暂时为NULL)
    layer->dW_ir = NULL;
    layer->dW_ih = NULL;
    layer->dW_zr = NULL;
    layer->dW_zh = NULL;
    layer->dW_hr = NULL;
    layer->dW_hh = NULL;

    // 偏置梯度(暂时为NULL)
    layer->db_ir = NULL;
    layer->db_ih = NULL;
    layer->db_zr = NULL;
    layer->db_zh = NULL;
    layer->db_hr = NULL;
    layer->db_hh = NULL;

    LOG_INFO("GRU layer created: input=%d, hidden=%d",
             config.input_size, config.hidden_size);

    // 初始化输入权重
    float* w_ir_data = (float*)layer->W_ir->data;
    float* w_zr_data = (float*)layer->W_zr->data;
    float* w_hr_data = (float*)layer->W_hr->data;

    for (size_t i = 0; i < layer->W_ir->size; i++) {
        w_ir_data[i] = xavier_init(layer->input_size, layer->hidden_size);
    }
    for (size_t i = 0; i < layer->W_zr->size; i++) {
        w_zr_data[i] = xavier_init(layer->input_size, layer->hidden_size);
    }
    for (size_t i = 0; i < layer->W_hr->size; i++) {
        w_hr_data[i] = xavier_init(layer->input_size, layer->hidden_size);
    }

    // 初始化隐藏权重
    float* w_ih_data = (float*)layer->W_ih->data;
    float* w_zh_data = (float*)layer->W_zh->data;
    float* w_hh_data = (float*)layer->W_hh->data;

    for (size_t i = 0; i < layer->W_ih->size; i++) {
        w_ih_data[i] = xavier_init(layer->hidden_size, layer->hidden_size);
    }
    for (size_t i = 0; i < layer->W_zh->size; i++) {
        w_zh_data[i] = xavier_init(layer->hidden_size, layer->hidden_size);
    }
    for (size_t i = 0; i < layer->W_hh->size; i++) {
        w_hh_data[i] = xavier_init(layer->hidden_size, layer->hidden_size);
    }

    return layer;
}

// GRU前向传播(单步)
Tensor* gru_forward_step(GRULayer* layer, Tensor* x_t, Tensor* h_prev) {
    CHECK_NULL_RETURN(layer, NULL);
    CHECK_NULL_RETURN(x_t, NULL);

    // 使用零初始化如果未提供前一状态
    if (!h_prev) {
        size_t shape[] = {(size_t)layer->hidden_size};
        h_prev = tensor_zeros(DT_FLOAT32, 1, shape);
    }

    // GRU计算:
    // r_t = sigmoid(W_ir * x_t + W_ih * h_prev + b_ir + b_ih)  [重置门]
    // z_t = sigmoid(W_zr * x_t + W_zh * h_prev + b_zr + b_zh)  [更新门]
    // h_tilde = tanh(W_hr * x_t + W_hh * (r_t ⊙ h_prev) + b_hr + b_hh)
    // h_t = (1 - z_t) ⊙ h_tilde + z_t ⊙ h_prev

    // 计算重置门
    Tensor* W_ir_x = tensor_matmul(x_t, layer->W_ir);
    Tensor* W_ih_h = tensor_matmul(h_prev, layer->W_ih);
    Tensor* r_pre = tensor_add(W_ir_x, W_ih_h);

    if (layer->use_bias) {
        Tensor* r_bias_sum = tensor_add(r_pre, layer->b_ir);
        Tensor* r_bias_sum2 = tensor_add(r_bias_sum, layer->b_ih);
        tensor_destroy(r_pre);
        tensor_destroy(r_bias_sum);
        r_pre = r_bias_sum2;
    }

    layer->r_t = tensor_sigmoid(r_pre);

    // 计算更新门
    Tensor* W_zr_x = tensor_matmul(x_t, layer->W_zr);
    Tensor* W_zh_h = tensor_matmul(h_prev, layer->W_zh);
    Tensor* z_pre = tensor_add(W_zr_x, W_zh_h);

    if (layer->use_bias) {
        Tensor* z_bias_sum = tensor_add(z_pre, layer->b_zr);
        Tensor* z_bias_sum2 = tensor_add(z_bias_sum, layer->b_zh);
        tensor_destroy(z_pre);
        tensor_destroy(z_bias_sum);
        z_pre = z_bias_sum2;
    }

    layer->z_t = tensor_sigmoid(z_pre);

    // 计算候选隐藏状态
    Tensor* W_hr_x = tensor_matmul(x_t, layer->W_hr);

    // r_t ⊙ h_prev
    Tensor* r_h = tensor_mul(layer->r_t, h_prev);
    Tensor* W_hh_rh = tensor_matmul(r_h, layer->W_hh);
    Tensor* h_tilde_pre = tensor_add(W_hr_x, W_hh_rh);

    if (layer->use_bias) {
        Tensor* h_bias_sum = tensor_add(h_tilde_pre, layer->b_hr);
        Tensor* h_bias_sum2 = tensor_add(h_bias_sum, layer->b_hh);
        tensor_destroy(h_tilde_pre);
        tensor_destroy(h_bias_sum);
        h_tilde_pre = h_bias_sum2;
    }

    layer->h_tilde = tensor_tanh(h_tilde_pre);

    // 计算最终隐藏状态
    // (1 - z_t) ⊙ h_tilde
    Tensor* one = tensor_ones(DT_FLOAT32, layer->z_t->ndim, layer->z_t->shape);
    Tensor* one_minus_z = tensor_sub(one, layer->z_t);
    Tensor* part1 = tensor_mul(one_minus_z, layer->h_tilde);

    // z_t ⊙ h_prev
    Tensor* part2 = tensor_mul(layer->z_t, h_prev);

    // h_t = part1 + part2
    layer->h_t = tensor_add(part1, part2);

    // 清理临时张量
    tensor_destroy(W_ir_x);
    tensor_destroy(W_ih_h);
    tensor_destroy(r_pre);
    tensor_destroy(W_zr_x);
    tensor_destroy(W_zh_h);
    tensor_destroy(z_pre);
    tensor_destroy(W_hr_x);
    tensor_destroy(r_h);
    tensor_destroy(W_hh_rh);
    tensor_destroy(h_tilde_pre);
    tensor_destroy(one);
    tensor_destroy(one_minus_z);
    tensor_destroy(part1);
    tensor_destroy(part2);

    // 缓存前一状态
    if (layer->h_prev) tensor_destroy(layer->h_prev);
    layer->h_prev = tensor_clone(h_prev);
    
    // 缓存当前输入
    if (layer->x_t) tensor_destroy(layer->x_t);
    layer->x_t = tensor_clone(x_t);

    return tensor_clone(layer->h_t);
}

// GRU前向传播(序列)
Tensor* gru_forward_sequence(GRULayer* layer, Tensor* x_seq, Tensor* h_0) {
    CHECK_NULL_RETURN(layer, NULL);
    CHECK_NULL_RETURN(x_seq, NULL);

    size_t seq_len = x_seq->shape[0];
    size_t hidden_size = (size_t)layer->hidden_size;

    size_t output_shape[] = {seq_len, hidden_size};
    Tensor* output_seq = tensor_create(DT_FLOAT32, 2, output_shape);

    Tensor* h_prev = h_0 ? tensor_clone(h_0) : NULL;

    for (size_t t = 0; t < seq_len; t++) {
        Tensor* x_t = tensor_slice(x_seq, 0, t, t + 1);
        Tensor* h_t = gru_forward_step(layer, x_t, h_prev);

        if (h_t) {
            size_t element_size = tensor_element_size(h_t->dtype);
            float* out_data = (float*)output_seq->data;
            float* h_data = (float*)h_t->data;
            memcpy(out_data + t * hidden_size, h_data, hidden_size * element_size);

            if (h_prev) tensor_destroy(h_prev);
            h_prev = layer->h_t ? tensor_clone(layer->h_t) : NULL;
        }

        tensor_destroy(x_t);
        tensor_destroy(h_t);
    }

    if (h_prev) tensor_destroy(h_prev);

    return output_seq;
}

// 初始化隐藏状态
void gru_init_state(GRULayer* layer, Tensor** h_0) {
    if (!layer) return;

    size_t shape[] = {(size_t)layer->hidden_size};

    if (h_0) {
        *h_0 = tensor_zeros(DT_FLOAT32, 1, shape);
    }
}

// 获取隐藏状态
Tensor* gru_get_hidden_state(GRULayer* layer) {
    return layer ? tensor_clone(layer->h_t) : NULL;
}

// GRU反向传播(单步)
void gru_backward_step(GRULayer* layer, Tensor* dh_next) {
    if (!layer || !layer->h_t || !layer->h_prev || !layer->x_t) {
        LOG_ERROR("GRU layer state not initialized for backward pass");
        return;
    }

    // Ensure gradient tensors are allocated
    size_t input_shape[] = {(size_t)layer->input_size, (size_t)layer->hidden_size};
    size_t hidden_shape[] = {(size_t)layer->hidden_size, (size_t)layer->hidden_size};
    size_t hidden_1d[] = {(size_t)layer->hidden_size};

    if (!layer->dW_ir) layer->dW_ir = tensor_zeros(DT_FLOAT32, 2, input_shape);
    if (!layer->dW_ih) layer->dW_ih = tensor_zeros(DT_FLOAT32, 2, hidden_shape);
    if (!layer->dW_zr) layer->dW_zr = tensor_zeros(DT_FLOAT32, 2, input_shape);
    if (!layer->dW_zh) layer->dW_zh = tensor_zeros(DT_FLOAT32, 2, hidden_shape);
    if (!layer->dW_hr) layer->dW_hr = tensor_zeros(DT_FLOAT32, 2, input_shape);
    if (!layer->dW_hh) layer->dW_hh = tensor_zeros(DT_FLOAT32, 2, hidden_shape);

    if (!layer->db_ir) layer->db_ir = tensor_zeros(DT_FLOAT32, 1, hidden_1d);
    if (!layer->db_ih) layer->db_ih = tensor_zeros(DT_FLOAT32, 1, hidden_1d);
    if (!layer->db_zr) layer->db_zr = tensor_zeros(DT_FLOAT32, 1, hidden_1d);
    if (!layer->db_zh) layer->db_zh = tensor_zeros(DT_FLOAT32, 1, hidden_1d);
    if (!layer->db_hr) layer->db_hr = tensor_zeros(DT_FLOAT32, 1, hidden_1d);
    if (!layer->db_hh) layer->db_hh = tensor_zeros(DT_FLOAT32, 1, hidden_1d);

    float* x_data  = (float*)layer->x_t->data;
    float* hp_data = (float*)layer->h_prev->data;
    float* r_data  = (float*)layer->r_t->data;
    float* z_data  = (float*)layer->z_t->data;
    float* ht_data = (float*)layer->h_tilde->data;
    int hidden = layer->hidden_size;
    int input  = layer->input_size;

    // === dh_t = dh_next (or zero if NULL) ===
    Tensor* dh_t;
    if (dh_next) {
        dh_t = tensor_clone(dh_next);
    } else {
        dh_t = tensor_zeros(DT_FLOAT32, 1, hidden_1d);
    }
    float* dh = (float*)dh_t->data;

    // === dz_partial: gradient through h_t w.r.t z_t ===
    // h_t = (1-z)*h_tilde + z*h_prev  =>  dh/dz = h_prev - h_tilde
    Tensor* dz_partial = tensor_create(DT_FLOAT32, 1, hidden_1d);
    float* dzp = (float*)dz_partial->data;
    for (int i = 0; i < hidden; i++) {
        dzp[i] = dh[i] * (hp_data[i] - ht_data[i]);
    }

    // === dh_tilde: gradient through h_t w.r.t h_tilde ===
    // dh/dh_tilde = 1 - z
    Tensor* dh_tilde = tensor_create(DT_FLOAT32, 1, hidden_1d);
    float* dht_data = (float*)dh_tilde->data;
    for (int i = 0; i < hidden; i++) {
        dht_data[i] = dh[i] * (1.0f - z_data[i]);
    }

    // === dh_prev_z: gradient through h_t w.r.t h_prev (via z) ===
    // dh/dh_prev = z
    Tensor* dh_prev_z = tensor_create(DT_FLOAT32, 1, hidden_1d);
    float* dhpz = (float*)dh_prev_z->data;
    for (int i = 0; i < hidden; i++) {
        dhpz[i] = dh[i] * z_data[i];
    }

    // === Candidate gate: tanh'(h_tilde_pre) * dh_tilde ===
    // tanh'(x) = 1 - tanh(x)^2 = 1 - h_tilde^2
    Tensor* dh_tilde_raw = tensor_create(DT_FLOAT32, 1, hidden_1d);
    float* dhtr = (float*)dh_tilde_raw->data;
    for (int i = 0; i < hidden; i++) {
        dhtr[i] = dht_data[i] * (1.0f - ht_data[i] * ht_data[i]);
    }

    // dW_hr += outer(x_t, dh_tilde_raw)   shape: (input, hidden)
    float* dw_hr = (float*)layer->dW_hr->data;
    for (int i = 0; i < input; i++) {
        for (int j = 0; j < hidden; j++) {
            dw_hr[i * hidden + j] += x_data[i] * dhtr[j];
        }
    }
    // db_hr += dh_tilde_raw
    float* db_hr = (float*)layer->db_hr->data;
    for (int j = 0; j < hidden; j++) db_hr[j] += dhtr[j];

    // dW_hh += outer(r_t * h_prev, dh_tilde_raw)
    float* dw_hh = (float*)layer->dW_hh->data;
    for (int i = 0; i < hidden; i++) {
        float r_hp = r_data[i] * hp_data[i];
        for (int j = 0; j < hidden; j++) {
            dw_hh[i * hidden + j] += r_hp * dhtr[j];
        }
    }
    // db_hh += dh_tilde_raw
    float* db_hh = (float*)layer->db_hh->data;
    for (int j = 0; j < hidden; j++) db_hh[j] += dhtr[j];

    // dr_t: gradient through candidate w.r.t r_t
    // dr_t = dh_tilde_raw * W_hh^T  (element-wise with h_prev later)
    // Simplified: dr_t_i = sum_j(dhtr[j] * W_hh[i][j]) * h_prev[i]
    //   then through r_t sigmoid: dr_raw_i = dr_t_i * r_i * (1 - r_i)
    Tensor* dr_t = tensor_create(DT_FLOAT32, 1, hidden_1d);
    float* dr = (float*)dr_t->data;
    float* w_hh = (float*)layer->W_hh->data;
    for (int i = 0; i < hidden; i++) {
        float sum = 0.0f;
        for (int j = 0; j < hidden; j++) {
            sum += dhtr[j] * w_hh[i * hidden + j];
        }
        dr[i] = sum * hp_data[i];
    }

    // dh_prev_h: gradient through candidate w.r.t h_prev (via r_t)
    // dh_prev_h_i = sum_j(dhtr[j] * W_hh[i][j]) * r_t[i]
    Tensor* dh_prev_h = tensor_create(DT_FLOAT32, 1, hidden_1d);
    float* dhph = (float*)dh_prev_h->data;
    for (int i = 0; i < hidden; i++) {
        float sum = 0.0f;
        for (int j = 0; j < hidden; j++) {
            sum += dhtr[j] * w_hh[i * hidden + j];
        }
        dhph[i] = sum * r_data[i];
    }

    // sigmoid'(r) = r * (1-r)
    Tensor* dr_raw = tensor_create(DT_FLOAT32, 1, hidden_1d);
    float* drr = (float*)dr_raw->data;
    for (int i = 0; i < hidden; i++) {
        drr[i] = dr[i] * r_data[i] * (1.0f - r_data[i]);
    }

    // dW_ir += outer(x_t, dr_raw)
    float* dw_ir = (float*)layer->dW_ir->data;
    for (int i = 0; i < input; i++) {
        for (int j = 0; j < hidden; j++) {
            dw_ir[i * hidden + j] += x_data[i] * drr[j];
        }
    }
    float* db_ir = (float*)layer->db_ir->data;
    for (int j = 0; j < hidden; j++) db_ir[j] += drr[j];

    // dW_ih += outer(h_prev, dr_raw)
    float* dw_ih = (float*)layer->dW_ih->data;
    for (int i = 0; i < hidden; i++) {
        for (int j = 0; j < hidden; j++) {
            dw_ih[i * hidden + j] += hp_data[i] * drr[j];
        }
    }
    float* db_ih = (float*)layer->db_ih->data;
    for (int j = 0; j < hidden; j++) db_ih[j] += drr[j];

    // dh_prev_r: gradient through h_prev via reset gate
    // dh_prev_r_i = sum_j(drr[j] * W_ih[i][j])
    Tensor* dh_prev_r = tensor_create(DT_FLOAT32, 1, hidden_1d);
    float* dhpr = (float*)dh_prev_r->data;
    float* w_ih = (float*)layer->W_ih->data;
    for (int i = 0; i < hidden; i++) {
        float sum = 0.0f;
        for (int j = 0; j < hidden; j++) {
            sum += drr[j] * w_ih[i * hidden + j];
        }
        dhpr[i] = sum;
    }

    // === Update gate: sigmoid'(z) * dz_partial ===
    // sigmoid'(z) = z * (1-z)
    Tensor* dz_raw = tensor_create(DT_FLOAT32, 1, hidden_1d);
    float* dzr = (float*)dz_raw->data;
    for (int i = 0; i < hidden; i++) {
        dzr[i] = dzp[i] * z_data[i] * (1.0f - z_data[i]);
    }

    // dW_zr += outer(x_t, dz_raw)
    float* dw_zr = (float*)layer->dW_zr->data;
    for (int i = 0; i < input; i++) {
        for (int j = 0; j < hidden; j++) {
            dw_zr[i * hidden + j] += x_data[i] * dzr[j];
        }
    }
    float* db_zr = (float*)layer->db_zr->data;
    for (int j = 0; j < hidden; j++) db_zr[j] += dzr[j];

    // dW_zh += outer(h_prev, dz_raw)
    float* dw_zh = (float*)layer->dW_zh->data;
    for (int i = 0; i < hidden; i++) {
        for (int j = 0; j < hidden; j++) {
            dw_zh[i * hidden + j] += hp_data[i] * dzr[j];
        }
    }
    float* db_zh = (float*)layer->db_zh->data;
    for (int j = 0; j < hidden; j++) db_zh[j] += dzr[j];

    // dh_prev_z2: gradient through h_prev via update gate
    Tensor* dh_prev_z2 = tensor_create(DT_FLOAT32, 1, hidden_1d);
    float* dhpz2 = (float*)dh_prev_z2->data;
    float* w_zh = (float*)layer->W_zh->data;
    for (int i = 0; i < hidden; i++) {
        float sum = 0.0f;
        for (int j = 0; j < hidden; j++) {
            sum += dzr[j] * w_zh[i * hidden + j];
        }
        dhpz2[i] = sum;
    }

    // === Accumulate dh_prev for BPTT ===
    // dh_prev = dh_prev_z + dh_prev_h + dh_prev_r + dh_prev_z2
    Tensor* dh_prev_final = tensor_create(DT_FLOAT32, 1, hidden_1d);
    float* dhpf = (float*)dh_prev_final->data;
    float* dhpz_d = (float*)dh_prev_z->data;
    float* dhph_d = (float*)dh_prev_h->data;
    float* dhpr_d = (float*)dh_prev_r->data;
    float* dhpz2_d = (float*)dh_prev_z2->data;
    for (int i = 0; i < hidden; i++) {
        dhpf[i] = dhpz_d[i] + dhph_d[i] + dhpr_d[i] + dhpz2_d[i];
    }
    if (layer->dh_prev_out) tensor_destroy(layer->dh_prev_out);
    layer->dh_prev_out = dh_prev_final;

    // Cleanup temporaries.
    tensor_destroy(dh_t);
    tensor_destroy(dz_partial);
    tensor_destroy(dh_tilde);
    tensor_destroy(dh_prev_z);
    tensor_destroy(dh_tilde_raw);
    tensor_destroy(dr_t);
    tensor_destroy(dh_prev_h);
    tensor_destroy(dr_raw);
    tensor_destroy(dh_prev_r);
    tensor_destroy(dz_raw);
    tensor_destroy(dh_prev_z2);
}

// GRU反向传播(序列)
void gru_backward_sequence(GRULayer* layer, Tensor* x_seq, Tensor* dh_seq) {
    if (!layer || !x_seq || !dh_seq) return;
    if (x_seq->ndim < 2 || dh_seq->ndim < 2) {
        LOG_ERROR("GRU sequence BP: x_seq and dh_seq must be 2D (seq_len x dim)");
        return;
    }

    size_t seq_len = x_seq->shape[0];
    if (dh_seq->shape[0] != seq_len) {
        LOG_ERROR("GRU sequence BP: x_seq and dh_seq seq_len mismatch");
        return;
    }

    // Process from last time step to first (BPTT)
    Tensor* dh_next = NULL;
    Tensor* saved_h_prev = NULL;  // h_t from time step t+1, used as h_prev for step t
    for (int t = (int)seq_len - 1; t >= 0; t--) {
        // Extract x_t and dh_t from sequences
        size_t t_size = (size_t)t;
        Tensor* x_t  = tensor_slice(x_seq, 0, t_size, t_size + 1);
        Tensor* dh_t = tensor_slice(dh_seq, 0, t_size, t_size + 1);

        // Squeeze batch dim: (1, dim) -> (dim,)
        size_t in_shape[]  = {(size_t)layer->input_size};
        size_t hid_shape[] = {(size_t)layer->hidden_size};
        Tensor* x_t_sq  = tensor_reshape(x_t, 1, in_shape);
        Tensor* dh_t_sq = tensor_reshape(dh_t, 1, hid_shape);

        // Re-run forward step with temporal chain: pass saved h_prev
        Tensor* h_out = gru_forward_step(layer, x_t_sq, saved_h_prev);

        // Combine dh_next (BPTT gradient from future) with current dh_t
        Tensor* dh_combined;
        if (dh_next) {
            dh_combined = tensor_add(dh_t_sq, dh_next);
            tensor_destroy(dh_next);
            dh_next = NULL;
        } else {
            dh_combined = tensor_clone(dh_t_sq);
        }

        // Run backward step (stores dh_prev_out in layer)
        gru_backward_step(layer, dh_combined);

        // Retrieve dh_prev for previous time step
        if (layer->dh_prev_out) {
            dh_next = tensor_clone(layer->dh_prev_out);
        }

        // Update saved_h_prev for next iteration (previous time step)
        // We need the h_prev that was actually used in the forward call.
        // Since forward_step saves h_prev internally, we use layer->h_prev.
        if (saved_h_prev) tensor_destroy(saved_h_prev);
        saved_h_prev = layer->h_prev ? tensor_clone(layer->h_prev) : NULL;

        // Cleanup
        tensor_destroy(x_t);
        tensor_destroy(dh_t);
        tensor_destroy(x_t_sq);
        tensor_destroy(dh_t_sq);
        tensor_destroy(dh_combined);
        if (h_out) tensor_destroy(h_out);
    }

    if (dh_next) tensor_destroy(dh_next);
    if (saved_h_prev) tensor_destroy(saved_h_prev);
}

// 销毁GRU层
void gru_layer_destroy(GRULayer* layer) {
    if (!layer) return;

    // 销毁权重
    tensor_destroy(layer->W_ir);
    tensor_destroy(layer->W_ih);
    tensor_destroy(layer->W_zr);
    tensor_destroy(layer->W_zh);
    tensor_destroy(layer->W_hr);
    tensor_destroy(layer->W_hh);

    // 销毁偏置
    tensor_destroy(layer->b_ir);
    tensor_destroy(layer->b_ih);
    tensor_destroy(layer->b_zr);
    tensor_destroy(layer->b_zh);
    tensor_destroy(layer->b_hr);
    tensor_destroy(layer->b_hh);

    // 销毁梯度
    tensor_destroy(layer->dW_ir);
    tensor_destroy(layer->dW_ih);
    tensor_destroy(layer->dW_zr);
    tensor_destroy(layer->dW_zh);
    tensor_destroy(layer->dW_hr);
    tensor_destroy(layer->dW_hh);

    // 销毁偏置梯度
    tensor_destroy(layer->db_ir);
    tensor_destroy(layer->db_ih);
    tensor_destroy(layer->db_zr);
    tensor_destroy(layer->db_zh);
    tensor_destroy(layer->db_hr);
    tensor_destroy(layer->db_hh);

    // 销毁缓存
    tensor_destroy(layer->x_t);
    tensor_destroy(layer->h_prev);
    tensor_destroy(layer->r_t);
    tensor_destroy(layer->z_t);
    tensor_destroy(layer->h_tilde);
    tensor_destroy(layer->h_t);

    tensor_destroy(layer->dh_prev_out);

    free(layer);
}
