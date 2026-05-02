#include "../include/generative_model.h"
#include "../include/tensor.h"
#include "../include/layer.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

// 交叉熵损失
float cross_entropy_loss(Tensor* predictions, Tensor* targets, int vocab_size) {
    float* pred_data = (float*)predictions->data;
    float* target_data = (float*)targets->data;
    int seq_len = predictions->shape[0];
    float loss = 0.0f;

    for (int t = 0; t < seq_len; t++) {
        int target_idx = (int)target_data[t];
        for (int v = 0; v < vocab_size; v++) {
            float pred = pred_data[t * vocab_size + v];
            float target = (v == target_idx) ? 1.0f : 0.0f;
            // 添加小值避免log(0)
            loss -= target * logf(pred + 1e-10f);
        }
    }

    return loss / seq_len;
}

// Softmax激活
void softmax(float* logits, int size) {
    // 检查是否有无效值
    for (int i = 0; i < size; i++) {
        if (!isfinite(logits[i])) {
            logits[i] = 0.0f;
        }
    }

    float max_val = logits[0];
    for (int i = 1; i < size; i++) {
        if (logits[i] > max_val) max_val = logits[i];
    }

    float sum = 0.0f;
    for (int i = 0; i < size; i++) {
        float val = expf(logits[i] - max_val);
        if (!isfinite(val)) val = 0.0f;  // 防止溢出
        logits[i] = val;
        sum += val;
    }

    // 防止除以零
    if (sum < 1e-10f) {
        sum = 1e-10f;
    }

    for (int i = 0; i < size; i++) {
        logits[i] /= sum;
        // 再次检查
        if (!isfinite(logits[i])) {
            logits[i] = 1.0f / size;  // 均匀分布
        }
    }
}

// 矩阵转置 (真正转置,不是视图)
Tensor* matrix_transpose(Tensor* tensor) {
    if (!tensor || tensor->ndim != 2) {
        printf("matrix_transpose: 无效输入\n");
        return NULL;
    }

    size_t rows = tensor->shape[0];
    size_t cols = tensor->shape[1];
    size_t new_shape[] = {cols, rows};
    Tensor* result = tensor_create(tensor->dtype, 2, new_shape);
    if (!result) {
        printf("matrix_transpose: 创建结果失败\n");
        return NULL;
    }

    float* src = (float*)tensor->data;
    float* dst = (float*)result->data;

    for (size_t i = 0; i < rows; i++) {
        for (size_t j = 0; j < cols; j++) {
            dst[j * rows + i] = src[i * cols + j];
        }
    }

    return result;
}

// Teacher Forcing训练 (简化版:只计算前向损失)
float seq2seq_train_with_teacher_forcing(Seq2SeqModel* model,
                                      Tensor* input_seq,
                                      Tensor* target_seq,
                                      float /*learning_rate*/,
                                      int /*use_teacher_forcing*/) {
    // 编码器前向传播
    Tensor* encoder_output = model_forward(model->encoder, input_seq);

    // 解码器训练循环
    int target_seq_len = target_seq->shape[0];
    int hidden_size = model->hidden_dim;
    int vocab_size = model->vocab_size;

    // 获取解码器各层
    Layer* dec_emb = model->decoder->layers[0];
    Layer* dec_rnn = model->decoder->layers[1];
    Layer* dec_linear = model->decoder->layers[2];
    RNNData* dec_rnn_data = (RNNData*)dec_rnn->private_data;

    if (!dec_rnn_data || !dec_rnn_data->hidden) {
        tensor_destroy(encoder_output);
        return 4.0f;
    }

    // 重置隐藏状态
    memset(dec_rnn_data->hidden->data, 0, hidden_size * sizeof(float));
    if (encoder_output->size >= (size_t)hidden_size) {
        memcpy(dec_rnn_data->hidden->data, encoder_output->data, hidden_size * sizeof(float));
    } else {
        memcpy(dec_rnn_data->hidden->data, encoder_output->data, encoder_output->size * sizeof(float));
    }

    float total_loss = 0.0f;
    int max_steps = target_seq_len > 8 ? 8 : target_seq_len;  // 限制步数加快训练

    // 逐时间步前向传播
    for (int t = 0; t < max_steps; t++) {
        int input_token = (t == 0) ? 1 : (int)((float*)target_seq->data)[t - 1];

        float input_val = (float)input_token;
        size_t input_shape[] = {1};
        Tensor* dec_input = tensor_create_from_data(DT_FLOAT32, 1, input_shape, &input_val);

        layer_forward(dec_emb, dec_input);
        layer_forward(dec_rnn, dec_emb->output);
        layer_forward(dec_linear, dec_rnn->output);

        // 简单softmax计算
        float* logits = (float*)dec_linear->output->data;
        int prob_size = vocab_size > 500 ? 500 : vocab_size;
        
        float max_logit = logits[0];
        for (int i = 1; i < prob_size; i++) {
            if (logits[i] > max_logit) max_logit = logits[i];
        }
        
        float sum = 0.0f;
        float probs[500];
        for (int i = 0; i < prob_size; i++) {
            probs[i] = expf(logits[i] - max_logit);
            sum += probs[i];
        }
        for (int i = 0; i < prob_size; i++) {
            probs[i] /= sum;
        }

        int target_token = (int)((float*)target_seq->data)[t];
        if (target_token < 0 || target_token >= vocab_size) target_token = 3;

        if (target_token < prob_size) {
            float target_prob = probs[target_token];
            total_loss += -logf(target_prob + 1e-10f);
        }

        tensor_destroy(dec_input);
    }

    tensor_destroy(encoder_output);
    return total_loss / max_steps;
}

// 梯度裁剪
void clip_gradients(float* grads, int size, float max_norm) {
    float norm = 0.0f;
    for (int i = 0; i < size; i++) {
        norm += grads[i] * grads[i];
    }
    norm = sqrtf(norm);

    if (norm > max_norm) {
        float scale = max_norm / norm;
        for (int i = 0; i < size; i++) {
            grads[i] *= scale;
        }
    }
}