#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "../include/gradient_ops.h"
#include "../include/error.h"

// 按值裁剪梯度
void clip_grad_by_value(Tensor* grad, float min_value, float max_value) {
    if (!grad) return;

    float* data = (float*)grad->data;
    for (size_t i = 0; i < grad->size; i++) {
        if (data[i] < min_value) {
            data[i] = min_value;
        } else if (data[i] > max_value) {
            data[i] = max_value;
        }
    }
}

// 按范数裁剪梯度
void clip_grad_by_norm(Tensor* grad, float max_norm) {
    if (!grad || max_norm <= 0) return;

    float norm = compute_grad_norm(grad);

    if (norm > max_norm) {
        float scale = max_norm / norm;
        float* data = (float*)grad->data;
        for (size_t i = 0; i < grad->size; i++) {
            data[i] *= scale;
        }
    }
}

// 按全局范数裁剪所有梯度
float clip_grad_global_norm(Tensor** grads, size_t num_grads, float max_norm) {
    if (!grads || num_grads == 0 || max_norm <= 0) return 0.0f;

    // 计算全局范数
    float global_norm = 0.0f;
    for (size_t i = 0; i < num_grads; i++) {
        if (grads[i]) {
            float norm = compute_grad_norm(grads[i]);
            global_norm += norm * norm;
        }
    }
    global_norm = sqrtf(global_norm);

    if (global_norm > max_norm) {
        float scale = max_norm / global_norm;

        // 缩放所有梯度
        for (size_t i = 0; i < num_grads; i++) {
            if (grads[i]) {
                float* data = (float*)grads[i]->data;
                for (size_t j = 0; j < grads[i]->size; j++) {
                    data[j] *= scale;
                }
            }
        }
    }

    return global_norm;
}

// 计算梯度范数
float compute_grad_norm(Tensor* grad) {
    if (!grad) return 0.0f;

    float norm = 0.0f;
    float* data = (float*)grad->data;

    for (size_t i = 0; i < grad->size; i++) {
        norm += data[i] * data[i];
    }

    return sqrtf(norm);
}

// 检查梯度是否有效(检测NaN/Inf)
bool check_grad_valid(Tensor* grad) {
    if (!grad) return false;

    float* data = (float*)grad->data;

    for (size_t i = 0; i < grad->size; i++) {
        if (isnan(data[i]) || isinf(data[i])) {
            LOG_WARNING("Invalid gradient detected at index %zu: %f", i, data[i]);
            return false;
        }
    }

    return true;
}

// 对模型所有梯度进行裁剪
void clip_model_grads(Model* model, float max_norm, ClipMode mode) {
    if (!model || max_norm <= 0) return;

    // 收集所有梯度
    Tensor** grads = NULL;
    size_t num_grads = 0;

    for (size_t i = 0; i < model->num_layers; i++) {
        Layer* layer = model->layers[i];
        if (layer->trainable) {
            // 收集权重和偏置的梯度
            if (layer->grad_weights || layer->grad_bias) {
                size_t new_size = num_grads + 2;
                grads = realloc(grads, new_size * sizeof(Tensor*));

                if (layer->grad_weights) {
                    grads[num_grads++] = layer->grad_weights;
                }
                if (layer->grad_bias) {
                    grads[num_grads++] = layer->grad_bias;
                }
            }
        }
    }

    if (num_grads == 0) {
        LOG_WARNING("No gradients to clip");
        return;
    }

    // 根据模式裁剪
    switch (mode) {
        case CLIP_BY_GLOBAL_NORM:
            clip_grad_global_norm(grads, num_grads, max_norm);
            break;

        case CLIP_BY_NORM:
            for (size_t i = 0; i < num_grads; i++) {
                clip_grad_by_norm(grads[i], max_norm);
            }
            break;

        case CLIP_BY_VALUE:
            for (size_t i = 0; i < num_grads; i++) {
                clip_grad_by_value(grads[i], -max_norm, max_norm);
            }
            break;

        default:
            LOG_ERROR("Unknown clip mode");
            break;
    }

    free(grads);
}

// 梯度累积
void accum_grad(Tensor* dest, Tensor* src, float scale) {
    if (!dest || !src) return;

    if (dest->size != src->size) {
        LOG_ERROR("Gradient size mismatch: %zu vs %zu", dest->size, src->size);
        return;
    }

    float* dest_data = (float*)dest->data;
    float* src_data = (float*)src->data;

    for (size_t i = 0; i < dest->size; i++) {
        dest_data[i] += src_data[i] * scale;
    }
}

// 清零模型梯度
void zero_model_grads(Model* model) {
    if (!model) return;

    for (size_t i = 0; i < model->num_layers; i++) {
        Layer* layer = model->layers[i];
        if (layer->trainable) {
            if (layer->grad_weights) {
                memset(layer->grad_weights->data, 0,
                       layer->grad_weights->size * sizeof(float));
            }
            if (layer->grad_bias) {
                memset(layer->grad_bias->data, 0,
                       layer->grad_bias->size * sizeof(float));
            }
        }
    }
}
