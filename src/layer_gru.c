#include "../include/common.h"
#include "../include/layer_gru.h"
#include "../include/error.h"

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
    layer->h_prev = NULL;
    layer->r_t = NULL;
    layer->z_t = NULL;
    layer->h_tilde = NULL;
    layer->h_t = NULL;

    // 梯度(暂时为NULL)
    layer->dW_ir = NULL;
    layer->dW_ih = NULL;
    layer->dW_zr = NULL;
    layer->dW_zh = NULL;
    layer->dW_hr = NULL;
    layer->dW_hh = NULL;

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
    layer->h_prev = tensor_clone(h_prev);

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
void gru_backward_step(GRULayer* layer, Tensor* /*dh_next*/) {
    if (!layer || !layer->h_t) {
        LOG_ERROR("GRU layer state not initialized for backward pass");
        return;
    }

    // TODO: 实现完整的GRU反向传播
    LOG_WARNING("GRU backward pass not fully implemented");
}

// GRU反向传播(序列)
void gru_backward_sequence(GRULayer* layer, Tensor* dh_seq) {
    if (!layer || !dh_seq) return;

    // TODO: 实现完整的GRU序列反向传播
    LOG_WARNING("GRU sequence backward pass not fully implemented");
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

    // 销毁缓存
    tensor_destroy(layer->h_prev);
    tensor_destroy(layer->r_t);
    tensor_destroy(layer->z_t);
    tensor_destroy(layer->h_tilde);
    tensor_destroy(layer->h_t);

    free(layer);
}
