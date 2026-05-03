#include "../include/common.h"
#include "../include/layer_lstm.h"
#include "../include/error.h"

// 创建LSTM层
LSTMLayer* lstm_layer_create(LSTMConfig config) {
    LSTMLayer* layer = malloc(sizeof(LSTMLayer));
    if (!layer) {
        LOG_ERROR("Failed to allocate LSTM layer");
        return NULL;
    }

    layer->input_size = config.input_size;
    layer->hidden_size = config.hidden_size;
    layer->use_bias = config.use_bias;
    layer->bidirectional = config.bidirectional;

    // Initialize random seed
    init_random();

    // 创建权重矩阵 (输入大小 x 隐藏大小)
    size_t weight_shape[] = {(size_t)config.input_size, (size_t)config.hidden_size};
    size_t hidden_shape[] = {(size_t)config.hidden_size, (size_t)config.hidden_size};
    size_t bias_shape[] = {(size_t)config.hidden_size};

    layer->W_if = tensor_create(DT_FLOAT32, 2, weight_shape);
    layer->W_ig = tensor_create(DT_FLOAT32, 2, weight_shape);
    layer->W_io = tensor_create(DT_FLOAT32, 2, weight_shape);
    layer->W_ii = tensor_create(DT_FLOAT32, 2, weight_shape);

    layer->R_if = tensor_create(DT_FLOAT32, 2, hidden_shape);
    layer->R_ig = tensor_create(DT_FLOAT32, 2, hidden_shape);
    layer->R_io = tensor_create(DT_FLOAT32, 2, hidden_shape);
    layer->R_ii = tensor_create(DT_FLOAT32, 2, hidden_shape);

    if (config.use_bias) {
        layer->b_if = tensor_zeros(DT_FLOAT32, 1, bias_shape);
        layer->b_ig = tensor_zeros(DT_FLOAT32, 1, bias_shape);
        layer->b_io = tensor_zeros(DT_FLOAT32, 1, bias_shape);
        layer->b_ii = tensor_zeros(DT_FLOAT32, 1, bias_shape);
    } else {
        layer->b_if = NULL;
        layer->b_ig = NULL;
        layer->b_io = NULL;
        layer->b_ii = NULL;
    }

    // 初始化缓存为NULL
    layer->x_t = NULL;
    layer->h_prev = NULL;
    layer->c_prev = NULL;
    layer->f_t = NULL;
    layer->i_t = NULL;
    layer->o_t = NULL;
    layer->g_t = NULL;
    layer->c_t = NULL;
    layer->h_t = NULL;

    // 梯度(暂时为NULL,训练时分配)
    layer->dW_if = NULL;
    layer->dW_ig = NULL;
    layer->dW_io = NULL;
    layer->dW_ii = NULL;
    layer->dR_if = NULL;
    layer->dR_ig = NULL;
    layer->dR_io = NULL;
    layer->dR_ii = NULL;

    // 初始化权重
    lstm_init_weights(layer);

    LOG_INFO("LSTM layer created: input=%d, hidden=%d, bidirectional=%d",
             config.input_size, config.hidden_size, config.bidirectional);

    return layer;
}

// LSTM权重初始化
void lstm_init_weights(LSTMLayer* layer) {
    int fan_in = layer->input_size;
    int fan_out = layer->hidden_size;

    // 初始化输入权重
    float* w_if_data = (float*)layer->W_if->data;
    float* w_ig_data = (float*)layer->W_ig->data;
    float* w_io_data = (float*)layer->W_io->data;
    float* w_ii_data = (float*)layer->W_ii->data;

    for (size_t i = 0; i < layer->W_if->size; i++) {
        w_if_data[i] = xavier_init(fan_in, fan_out);
        w_ig_data[i] = xavier_init(fan_in, fan_out);
        w_io_data[i] = xavier_init(fan_in, fan_out);
        w_ii_data[i] = xavier_init(fan_in, fan_out);
    }

    // 初始化隐藏权重
    fan_in = layer->hidden_size;
    float* r_if_data = (float*)layer->R_if->data;
    float* r_ig_data = (float*)layer->R_ig->data;
    float* r_io_data = (float*)layer->R_io->data;
    float* r_ii_data = (float*)layer->R_ii->data;

    for (size_t i = 0; i < layer->R_if->size; i++) {
        r_if_data[i] = xavier_init(fan_in, fan_out);
        r_ig_data[i] = xavier_init(fan_in, fan_out);
        r_io_data[i] = xavier_init(fan_in, fan_out);
        r_ii_data[i] = xavier_init(fan_in, fan_out);
    }

    // 遗忘门偏置初始化为1(帮助长期记忆)
    if (layer->use_bias && layer->b_ig) {
        float* b_ig_data = (float*)layer->b_ig->data;
        for (size_t i = 0; i < layer->b_ig->size; i++) {
            b_ig_data[i] = 1.0f;
        }
    }
}

// 初始化梯度张量（训练前调用）
void lstm_init_gradients(LSTMLayer* layer) {
    if (!layer) return;

    size_t weight_shape[] = {(size_t)layer->input_size, (size_t)layer->hidden_size};
    size_t hidden_shape[] = {(size_t)layer->hidden_size, (size_t)layer->hidden_size};

    // 输入权重梯度
    layer->dW_if = tensor_zeros(DT_FLOAT32, 2, weight_shape);
    layer->dW_ig = tensor_zeros(DT_FLOAT32, 2, weight_shape);
    layer->dW_io = tensor_zeros(DT_FLOAT32, 2, weight_shape);
    layer->dW_ii = tensor_zeros(DT_FLOAT32, 2, weight_shape);

    // 隐藏权重梯度
    layer->dR_if = tensor_zeros(DT_FLOAT32, 2, hidden_shape);
    layer->dR_ig = tensor_zeros(DT_FLOAT32, 2, hidden_shape);
    layer->dR_io = tensor_zeros(DT_FLOAT32, 2, hidden_shape);
    layer->dR_ii = tensor_zeros(DT_FLOAT32, 2, hidden_shape);

    // 缓存 x_t（用于反向传播）
    size_t x_shape[] = {(size_t)layer->input_size};
    layer->x_t = tensor_zeros(DT_FLOAT32, 1, x_shape);

    LOG_INFO("LSTM gradients initialized");
}

// 清零梯度（每个batch开始时调用）
void lstm_zero_gradients(LSTMLayer* layer) {
    if (!layer) return;

    // 使用 tensor_zeros 创建新的零张量替换
    if (layer->dW_if) {
        float* data = (float*)layer->dW_if->data;
        memset(data, 0, layer->dW_if->size * sizeof(float));
    }
    if (layer->dW_ig) {
        float* data = (float*)layer->dW_ig->data;
        memset(data, 0, layer->dW_ig->size * sizeof(float));
    }
    if (layer->dW_io) {
        float* data = (float*)layer->dW_io->data;
        memset(data, 0, layer->dW_io->size * sizeof(float));
    }
    if (layer->dW_ii) {
        float* data = (float*)layer->dW_ii->data;
        memset(data, 0, layer->dW_ii->size * sizeof(float));
    }
    if (layer->dR_if) {
        float* data = (float*)layer->dR_if->data;
        memset(data, 0, layer->dR_if->size * sizeof(float));
    }
    if (layer->dR_ig) {
        float* data = (float*)layer->dR_ig->data;
        memset(data, 0, layer->dR_ig->size * sizeof(float));
    }
    if (layer->dR_io) {
        float* data = (float*)layer->dR_io->data;
        memset(data, 0, layer->dR_io->size * sizeof(float));
    }
    if (layer->dR_ii) {
        float* data = (float*)layer->dR_ii->data;
        memset(data, 0, layer->dR_ii->size * sizeof(float));
    }
}

// LSTM前向传播(单步)
Tensor* lstm_forward_step(LSTMLayer* layer, Tensor* x_t, Tensor* h_prev, Tensor* c_prev) {
    CHECK_NULL(layer);
    CHECK_NULL(x_t);

    // 使用零初始化如果未提供前一状态
    if (!h_prev) {
        size_t shape[] = {(size_t)layer->hidden_size};
        h_prev = tensor_zeros(DT_FLOAT32, 1, shape);
    }
    if (!c_prev) {
        size_t shape[] = {(size_t)layer->hidden_size};
        c_prev = tensor_zeros(DT_FLOAT32, 1, shape);
    }

    // 计算门控和候选状态
    // f_t = sigmoid(W_if * x_t + R_if * h_prev + b_if)
    // i_t = sigmoid(W_ig * x_t + R_ig * h_prev + b_ig)
    // o_t = sigmoid(W_io * x_t + R_io * h_prev + b_io)
    // g_t = tanh(W_ii * x_t + R_ii * h_prev + b_ii)

    // 计算输入部分的加权和
    Tensor* W_if_x = tensor_matmul(x_t, layer->W_if);
    Tensor* W_ig_x = tensor_matmul(x_t, layer->W_ig);
    Tensor* W_io_x = tensor_matmul(x_t, layer->W_io);
    Tensor* W_ii_x = tensor_matmul(x_t, layer->W_ii);

    // 计算隐藏部分的加权和
    Tensor* R_if_h = tensor_matmul(h_prev, layer->R_if);
    Tensor* R_ig_h = tensor_matmul(h_prev, layer->R_ig);
    Tensor* R_io_h = tensor_matmul(h_prev, layer->R_io);
    Tensor* R_ii_h = tensor_matmul(h_prev, layer->R_ii);

    // 相加
    Tensor* f_pre = tensor_add(W_if_x, R_if_h);
    Tensor* i_pre = tensor_add(W_ig_x, R_ig_h);
    Tensor* o_pre = tensor_add(W_io_x, R_io_h);
    Tensor* g_pre = tensor_add(W_ii_x, R_ii_h);

    // 加偏置
    if (layer->use_bias) {
        Tensor* f_b = tensor_add(f_pre, layer->b_if);
        Tensor* i_b = tensor_add(i_pre, layer->b_ig);
        Tensor* o_b = tensor_add(o_pre, layer->b_io);
        Tensor* g_b = tensor_add(g_pre, layer->b_ii);

        tensor_destroy(f_pre);
        tensor_destroy(i_pre);
        tensor_destroy(o_pre);
        tensor_destroy(g_pre);

        f_pre = f_b;
        i_pre = i_b;
        o_pre = o_b;
        g_pre = g_b;
    }

    // 应用激活函数
    layer->f_t = tensor_sigmoid(f_pre);  // 遗忘门
    layer->i_t = tensor_sigmoid(i_pre);  // 输入门
    layer->o_t = tensor_sigmoid(o_pre);  // 输出门
    layer->g_t = tensor_tanh(g_pre);     // 候选状态

    // 清理临时张量
    tensor_destroy(W_if_x);
    tensor_destroy(W_ig_x);
    tensor_destroy(W_io_x);
    tensor_destroy(W_ii_x);
    tensor_destroy(R_if_h);
    tensor_destroy(R_ig_h);
    tensor_destroy(R_io_h);
    tensor_destroy(R_ii_h);
    tensor_destroy(f_pre);
    tensor_destroy(i_pre);
    tensor_destroy(o_pre);
    tensor_destroy(g_pre);

    // 更新细胞状态: c_t = f_t ⊙ c_prev + i_t ⊙ g_t
    Tensor* fc = tensor_mul(layer->f_t, c_prev);
    Tensor* ig = tensor_mul(layer->i_t, layer->g_t);
    layer->c_t = tensor_add(fc, ig);

    tensor_destroy(fc);
    tensor_destroy(ig);

    // 更新隐藏状态: h_t = o_t ⊙ tanh(c_t)
    Tensor* tanh_c = tensor_tanh(layer->c_t);
    layer->h_t = tensor_mul(layer->o_t, tanh_c);

    tensor_destroy(tanh_c);

    // 缓存前一状态和输入（用于反向传播）
    layer->h_prev = tensor_clone(h_prev);
    layer->c_prev = tensor_clone(c_prev);
    if (layer->x_t) tensor_destroy(layer->x_t);
    layer->x_t = tensor_clone(x_t);

    return tensor_clone(layer->h_t);
}

// LSTM前向传播(序列)
Tensor* lstm_forward_sequence(LSTMLayer* layer, Tensor* x_seq, Tensor* h_0, Tensor* c_0) {
    CHECK_NULL(layer);
    CHECK_NULL(x_seq);

    // x_seq形状应该是 (seq_len, batch_size, input_size) 或 (seq_len, input_size)

    size_t seq_len = x_seq->shape[0];
    size_t hidden_size = (size_t)layer->hidden_size;

    // 输出张量形状: (seq_len, hidden_size)
    size_t output_shape[] = {seq_len, hidden_size};
    Tensor* output_seq = tensor_create(DT_FLOAT32, 2, output_shape);

    Tensor* h_prev = h_0 ? tensor_clone(h_0) : NULL;
    Tensor* c_prev = c_0 ? tensor_clone(c_0) : NULL;

    // 逐时间步前向传播
    for (size_t t = 0; t < seq_len; t++) {
        // 提取当前时间步的输入
        Tensor* x_t = tensor_slice(x_seq, 0, t, t + 1);

        // 前向传播
        Tensor* h_t = lstm_forward_step(layer, x_t, h_prev, c_prev);

        // 将输出写入output_seq
        if (h_t) {
            size_t element_size = tensor_element_size(h_t->dtype);
            float* out_data = (float*)output_seq->data;
            float* h_data = (float*)h_t->data;
            memcpy(out_data + t * hidden_size, h_data, hidden_size * element_size);

            // 更新前一状态
            if (h_prev) tensor_destroy(h_prev);
            if (c_prev) tensor_destroy(c_prev);
            h_prev = layer->h_t ? tensor_clone(layer->h_t) : NULL;
            c_prev = layer->c_t ? tensor_clone(layer->c_t) : NULL;
        }

        tensor_destroy(x_t);
        tensor_destroy(h_t);
    }

    if (h_prev) tensor_destroy(h_prev);
    if (c_prev) tensor_destroy(c_prev);

    return output_seq;
}

// 初始化隐藏状态和细胞状态
void lstm_init_state(LSTMLayer* layer, Tensor** h_0, Tensor** c_0) {
    if (!layer) return;

    size_t shape[] = {(size_t)layer->hidden_size};

    if (h_0) {
        *h_0 = tensor_zeros(DT_FLOAT32, 1, shape);
    }
    if (c_0) {
        *c_0 = tensor_zeros(DT_FLOAT32, 1, shape);
    }
}

// 获取隐藏状态
Tensor* lstm_get_hidden_state(LSTMLayer* layer) {
    return layer ? tensor_clone(layer->h_t) : NULL;
}

// 获取细胞状态
Tensor* lstm_get_cell_state(LSTMLayer* layer) {
    return layer ? tensor_clone(layer->c_t) : NULL;
}

// LSTM反向传播(单步)
void lstm_backward_step(LSTMLayer* layer, Tensor* dh_next, Tensor* dc_next) {
    if (!layer || !layer->h_t || !layer->c_t) {
        LOG_ERROR("LSTM layer state not initialized for backward pass");
        return;
    }

    // 初始化梯度
    Tensor* dh_t = dh_next ? tensor_clone(dh_next) : NULL;
    Tensor* dc_t = dc_next ? tensor_clone(dc_next) : NULL;

    if (!dh_t) {
        size_t shape[] = {(size_t)layer->hidden_size};
        dh_t = tensor_zeros(DT_FLOAT32, 1, shape);
    }
    if (!dc_t) {
        size_t shape[] = {(size_t)layer->hidden_size};
        dc_t = tensor_zeros(DT_FLOAT32, 1, shape);
    }

    // 计算dc_t: dc_t = dh_t ⊙ o_t ⊙ tanh'(c_t) + dc_next
    // tanh'(c_t) = 1 - tanh(c_t)^2
    Tensor* tanh_c = tensor_tanh(layer->c_t);
    Tensor* tanh_c_sq = tensor_mul(tanh_c, tanh_c);
    Tensor* one_minus = tensor_create(DT_FLOAT32, tanh_c_sq->ndim, tanh_c_sq->shape);
    float* one_minus_data = (float*)one_minus->data;
    float* tanh_sq_data = (float*)tanh_c_sq->data;
    for (size_t i = 0; i < one_minus->size; i++) {
        one_minus_data[i] = 1.0f - tanh_sq_data[i];
    }

    Tensor* dh_o = tensor_mul(dh_t, layer->o_t);
    Tensor* dh_final = tensor_mul(dh_o, one_minus);
    Tensor* dc_new = tensor_add(dh_final, dc_t);

    // 计算门控梯度
    // df_t = dc_t ⊙ c_prev
    Tensor* df = tensor_mul(dc_new, layer->c_prev);

    // di_t = dc_t ⊙ g_t
    Tensor* di = tensor_mul(dc_new, layer->g_t);

    // dg_t = dc_t ⊙ i_t
    Tensor* dg = tensor_mul(dc_new, layer->i_t);

    // do_t = dh_t ⊙ tanh(c_t)
    Tensor* do_ = tensor_mul(dh_t, tanh_c);

    // 计算门控输入的梯度
    // d(前向网络输出) = sigmoid'(x) ⊙ d(门控输出)
    // sigmoid'(x) = sigmoid(x) * (1 - sigmoid(x))

    // tanh' = 1 - tanh^2(x)
    Tensor* tanh_sq = tensor_mul(layer->g_t, layer->g_t);
    Tensor* one_minus_g = tensor_create(DT_FLOAT32, tanh_sq->ndim, tanh_sq->shape);
    float* one_g_data = (float*)one_minus_g->data;
    float* g_sq_data = (float*)tanh_sq->data;
    for (size_t i = 0; i < one_minus_g->size; i++) {
        one_g_data[i] = 1.0f - g_sq_data[i];
    }
    Tensor* dg_pre = tensor_mul(dg, one_minus_g);

    // sigmoid' = sigmoid(x) * (1 - sigmoid(x))
    Tensor* one_minus_f = tensor_create(DT_FLOAT32, layer->f_t->ndim, layer->f_t->shape);
    float* one_f_data = (float*)one_minus_f->data;
    float* f_data = (float*)layer->f_t->data;
    for (size_t i = 0; i < one_minus_f->size; i++) {
        one_f_data[i] = 1.0f - f_data[i];
    }
    Tensor* f_deriv = tensor_mul(layer->f_t, one_minus_f);
    Tensor* df_pre = tensor_mul(df, f_deriv);

    Tensor* one_minus_i = tensor_create(DT_FLOAT32, layer->i_t->ndim, layer->i_t->shape);
    float* one_i_data = (float*)one_minus_i->data;
    float* i_data = (float*)layer->i_t->data;
    for (size_t i = 0; i < one_minus_i->size; i++) {
        one_i_data[i] = 1.0f - i_data[i];
    }
    Tensor* i_deriv = tensor_mul(layer->i_t, one_minus_i);
    Tensor* di_pre = tensor_mul(di, i_deriv);

    Tensor* one_minus_o = tensor_create(DT_FLOAT32, layer->o_t->ndim, layer->o_t->shape);
    float* one_o_data = (float*)one_minus_o->data;
    float* o_data = (float*)layer->o_t->data;
    for (size_t i = 0; i < one_minus_o->size; i++) {
        one_o_data[i] = 1.0f - o_data[i];
    }
    Tensor* o_deriv = tensor_mul(layer->o_t, one_minus_o);
    Tensor* do_pre = tensor_mul(do_, o_deriv);

    // 累加到dc_prev: dc_prev = df_t ⊙ c_prev
    Tensor* dc_prev = tensor_mul(df, layer->c_prev);

    // ========== 计算权重梯度 ==========
    // dW_if = x_t^T @ df_pre
    // dR_if = h_prev^T @ df_pre
    if (layer->dW_if && layer->dR_if && layer->x_t && layer->h_prev) {
        // dW_if += x_t^T @ df_pre (梯度累积)
        Tensor* x_t_trans = tensor_transpose(layer->x_t, 0, 1);  // 转置为 (hidden, input)
        Tensor* dW_if_grad = tensor_matmul(x_t_trans, df_pre);    // (hidden, input) @ (hidden,) -> (input, hidden)
        Tensor* new_dW_if = tensor_add(layer->dW_if, dW_if_grad);
        tensor_destroy(layer->dW_if);
        layer->dW_if = new_dW_if;

        // dR_if += h_prev^T @ df_pre
        Tensor* h_prev_trans = tensor_transpose(layer->h_prev, 0, 1);
        Tensor* dR_if_grad = tensor_matmul(h_prev_trans, df_pre);
        Tensor* new_dR_if = tensor_add(layer->dR_if, dR_if_grad);
        tensor_destroy(layer->dR_if);
        layer->dR_if = new_dR_if;

        tensor_destroy(x_t_trans);
        tensor_destroy(dW_if_grad);
        tensor_destroy(h_prev_trans);
        tensor_destroy(dR_if_grad);
    }

    // dW_ig = x_t^T @ di_pre, dR_ig = h_prev^T @ di_pre
    if (layer->dW_ig && layer->dR_ig && layer->x_t && layer->h_prev) {
        Tensor* x_t_trans = tensor_transpose(layer->x_t, 0, 1);
        Tensor* dW_ig_grad = tensor_matmul(x_t_trans, di_pre);
        Tensor* new_dW_ig = tensor_add(layer->dW_ig, dW_ig_grad);
        tensor_destroy(layer->dW_ig);
        layer->dW_ig = new_dW_ig;

        Tensor* h_prev_trans = tensor_transpose(layer->h_prev, 0, 1);
        Tensor* dR_ig_grad = tensor_matmul(h_prev_trans, di_pre);
        Tensor* new_dR_ig = tensor_add(layer->dR_ig, dR_ig_grad);
        tensor_destroy(layer->dR_ig);
        layer->dR_ig = new_dR_ig;

        tensor_destroy(x_t_trans);
        tensor_destroy(dW_ig_grad);
        tensor_destroy(h_prev_trans);
        tensor_destroy(dR_ig_grad);
    }

    // dW_io = x_t^T @ do_pre, dR_io = h_prev^T @ do_pre
    if (layer->dW_io && layer->dR_io && layer->x_t && layer->h_prev) {
        Tensor* x_t_trans = tensor_transpose(layer->x_t, 0, 1);
        Tensor* dW_io_grad = tensor_matmul(x_t_trans, do_pre);
        Tensor* new_dW_io = tensor_add(layer->dW_io, dW_io_grad);
        tensor_destroy(layer->dW_io);
        layer->dW_io = new_dW_io;

        Tensor* h_prev_trans = tensor_transpose(layer->h_prev, 0, 1);
        Tensor* dR_io_grad = tensor_matmul(h_prev_trans, do_pre);
        Tensor* new_dR_io = tensor_add(layer->dR_io, dR_io_grad);
        tensor_destroy(layer->dR_io);
        layer->dR_io = new_dR_io;

        tensor_destroy(x_t_trans);
        tensor_destroy(dW_io_grad);
        tensor_destroy(h_prev_trans);
        tensor_destroy(dR_io_grad);
    }

    // dW_ii = x_t^T @ dg_pre, dR_ii = h_prev^T @ dg_pre
    if (layer->dW_ii && layer->dR_ii && layer->x_t && layer->h_prev) {
        Tensor* x_t_trans = tensor_transpose(layer->x_t, 0, 1);
        Tensor* dW_ii_grad = tensor_matmul(x_t_trans, dg_pre);
        Tensor* new_dW_ii = tensor_add(layer->dW_ii, dW_ii_grad);
        tensor_destroy(layer->dW_ii);
        layer->dW_ii = new_dW_ii;

        Tensor* h_prev_trans = tensor_transpose(layer->h_prev, 0, 1);
        Tensor* dR_ii_grad = tensor_matmul(h_prev_trans, dg_pre);
        Tensor* new_dR_ii = tensor_add(layer->dR_ii, dR_ii_grad);
        tensor_destroy(layer->dR_ii);
        layer->dR_ii = new_dR_ii;

        tensor_destroy(x_t_trans);
        tensor_destroy(dW_ii_grad);
        tensor_destroy(h_prev_trans);
        tensor_destroy(dR_ii_grad);
    }

    // ========== 计算 dh_prev ==========
    // dh_prev = df_pre @ R_if^T + di_pre @ R_ig^T + dg_pre @ R_ii^T + do_pre @ R_io^T
    Tensor* dh_prev = tensor_matmul(df_pre, layer->R_if);
    Tensor* dh_prev2 = tensor_matmul(di_pre, layer->R_ig);
    Tensor* dh_prev3 = tensor_matmul(dg_pre, layer->R_ii);
    Tensor* dh_prev4 = tensor_matmul(do_pre, layer->R_io);

    Tensor* dh_prev_sum1 = tensor_add(dh_prev, dh_prev2);
    Tensor* dh_prev_sum2 = tensor_add(dh_prev3, dh_prev4);
    Tensor* dh_prev_final = tensor_add(dh_prev_sum1, dh_prev_sum2);

    // 清理临时张量
    tensor_destroy(dh_t);
    tensor_destroy(dc_t);
    tensor_destroy(tanh_c);
    tensor_destroy(tanh_sq);
    tensor_destroy(one_minus_g);
    tensor_destroy(one_minus_f);
    tensor_destroy(one_minus_i);
    tensor_destroy(one_minus_o);
    tensor_destroy(f_deriv);
    tensor_destroy(i_deriv);
    tensor_destroy(o_deriv);
    tensor_destroy(dh_o);
    tensor_destroy(dh_final);
    tensor_destroy(dc_new);
    tensor_destroy(df);
    tensor_destroy(di);
    tensor_destroy(dg);
    tensor_destroy(do_);
    tensor_destroy(dg_pre);
    tensor_destroy(df_pre);
    tensor_destroy(di_pre);
    tensor_destroy(do_pre);
    tensor_destroy(dc_prev);
    tensor_destroy(dh_prev);
    tensor_destroy(dh_prev2);
    tensor_destroy(dh_prev3);
    tensor_destroy(dh_prev4);
    tensor_destroy(dh_prev_sum1);
    tensor_destroy(dh_prev_sum2);
    tensor_destroy(dh_prev_final);
}

// LSTM反向传播(序列)
void lstm_backward_sequence(LSTMLayer* layer, Tensor* dh_seq) {
    if (!layer || !dh_seq) return;

    // TODO: 实现完整的LSTM序列反向传播
    LOG_WARNING("LSTM sequence backward pass not fully implemented");
}

// 销毁LSTM层
void lstm_layer_destroy(LSTMLayer* layer) {
    if (!layer) return;

    // 销毁权重
    tensor_destroy(layer->W_if);
    tensor_destroy(layer->W_ig);
    tensor_destroy(layer->W_io);
    tensor_destroy(layer->W_ii);
    tensor_destroy(layer->R_if);
    tensor_destroy(layer->R_ig);
    tensor_destroy(layer->R_io);
    tensor_destroy(layer->R_ii);

    // 销毁偏置
    tensor_destroy(layer->b_if);
    tensor_destroy(layer->b_ig);
    tensor_destroy(layer->b_io);
    tensor_destroy(layer->b_ii);

    // 销毁梯度
    tensor_destroy(layer->dW_if);
    tensor_destroy(layer->dW_ig);
    tensor_destroy(layer->dW_io);
    tensor_destroy(layer->dW_ii);
    tensor_destroy(layer->dR_if);
    tensor_destroy(layer->dR_ig);
    tensor_destroy(layer->dR_io);
    tensor_destroy(layer->dR_ii);

    // 销毁缓存
    tensor_destroy(layer->x_t);
    tensor_destroy(layer->h_prev);
    tensor_destroy(layer->c_prev);
    tensor_destroy(layer->f_t);
    tensor_destroy(layer->i_t);
    tensor_destroy(layer->o_t);
    tensor_destroy(layer->g_t);
    tensor_destroy(layer->c_t);
    tensor_destroy(layer->h_t);

    free(layer);
}
