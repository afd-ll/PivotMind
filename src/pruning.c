#include "../include/common.h"
#include "../include/pruning.h"
#include "../include/error.h"

// 剪枝张量(基于幅值)
Tensor* prune_tensor_magnitude(Tensor* tensor, float sparsity) {
    CHECK_NULL_RETURN(tensor, NULL);

    if (sparsity < 0 || sparsity > 1) {
        LOG_ERROR("Sparsity must be in [0, 1]");
        return NULL;
    }

    size_t num_zeros = (size_t)(tensor->size * sparsity);

    // 创建掩码
    Tensor* mask = tensor_clone(tensor);

    // 计算每个权重的绝对值
    float* mask_data = (float*)mask->data;
    float* tensor_data = (float*)tensor->data;

    for (size_t i = 0; i < mask->size; i++) {
        mask_data[i] = fabsf(tensor_data[i]);
    }

    // 找到阈值
    // 简化:使用冒泡排序(实际应该用快速选择算法)
    for (size_t i = 0; i < num_zeros; i++) {
        for (size_t j = i + 1; j < mask->size; j++) {
            if (mask_data[i] > mask_data[j]) {
                float temp = mask_data[i];
                mask_data[i] = mask_data[j];
                mask_data[j] = temp;
            }
        }
    }

    float threshold = mask_data[num_zeros - 1];

    // 应用掩码
    for (size_t i = 0; i < tensor->size; i++) {
        if (fabsf(tensor_data[i]) < threshold) {
            mask_data[i] = 0.0f;
        } else {
            mask_data[i] = 1.0f;
        }
    }

    return mask;
}

// 剪枝张量(随机)
Tensor* prune_tensor_random(Tensor* tensor, float sparsity) {
    CHECK_NULL_RETURN(tensor, NULL);

    size_t num_zeros = (size_t)(tensor->size * sparsity);

    Tensor* mask = tensor_ones(DT_FLOAT32, tensor->ndim, tensor->shape);
    float* mask_data = (float*)mask->data;

    // Initialize random seed
    init_random();

    // 随机选择要剪枝的位置
    size_t* indices = malloc(num_zeros * sizeof(size_t));
    for (size_t i = 0; i < num_zeros; i++) {
        indices[i] = rand() % tensor->size;
    }

    // 设置掩码
    for (size_t i = 0; i < num_zeros; i++) {
        mask_data[indices[i]] = 0.0f;
    }

    free(indices);
    return mask;
}

// 剪枝张量(基于梯度)
Tensor* prune_tensor_gradient(Tensor* tensor, Tensor* gradient, float sparsity) {
    CHECK_NULL_RETURN(tensor, NULL);
    CHECK_NULL_RETURN(gradient, NULL);

    size_t num_zeros = (size_t)(tensor->size * sparsity);

    Tensor* mask = tensor_clone(tensor);
    float* mask_data = (float*)mask->data;
    float* grad_data = (float*)gradient->data;

    // 计算梯度绝对值
    for (size_t i = 0; i < mask->size; i++) {
        mask_data[i] = fabsf(grad_data[i]);
    }

    // 找到阈值(剪枝梯度最小的权重)
    for (size_t i = 0; i < num_zeros; i++) {
        for (size_t j = i + 1; j < mask->size; j++) {
            if (mask_data[i] > mask_data[j]) {
                float temp = mask_data[i];
                mask_data[i] = mask_data[j];
                mask_data[j] = temp;
            }
        }
    }

    float threshold = mask_data[num_zeros - 1];

    // 应用掩码
    for (size_t i = 0; i < tensor->size; i++) {
        if (fabsf(grad_data[i]) < threshold) {
            mask_data[i] = 0.0f;
        } else {
            mask_data[i] = 1.0f;
        }
    }

    return mask;
}

// 结构化剪枝
Tensor* prune_structured(Tensor* tensor, float sparsity, int axis) {
    CHECK_NULL_RETURN(tensor, NULL);

    if (tensor->ndim != 2) {
        LOG_ERROR("Structured pruning only supports 2D tensors");
        return NULL;
    }

    size_t rows = tensor->shape[0];
    size_t cols = tensor->shape[1];

    Tensor* mask = tensor_ones(DT_FLOAT32, tensor->ndim, tensor->shape);
    float* mask_data = (float*)mask->data;
    float* tensor_data = (float*)tensor->data;

    if (axis == 0) {
        // 剪枝整列
        size_t num_pruned_cols = (size_t)(cols * sparsity);

        // 计算每列的L2范数
        float* col_norms = malloc(cols * sizeof(float));
        size_t* col_indices = malloc(cols * sizeof(size_t));
        for (size_t j = 0; j < cols; j++) {
            float norm = 0.0f;
            for (size_t i = 0; i < rows; i++) {
                norm += tensor_data[i * cols + j] * tensor_data[i * cols + j];
            }
            col_norms[j] = sqrtf(norm);
            col_indices[j] = j;  // 保留原始索引
        }

        // 找到范数最小的列（同步排序 norms 和 indices）
        for (size_t i = 0; i < num_pruned_cols; i++) {
            for (size_t j = i + 1; j < cols; j++) {
                if (col_norms[i] > col_norms[j]) {
                    float temp_n = col_norms[i];
                    col_norms[i] = col_norms[j];
                    col_norms[j] = temp_n;
                    size_t temp_i = col_indices[i];
                    col_indices[i] = col_indices[j];
                    col_indices[j] = temp_i;
                }
            }
        }

        // 剪枝范数最小的 num_pruned_cols 列
        for (size_t i = 0; i < num_pruned_cols; i++) {
            size_t col_idx = col_indices[i];  // 映射回原始列下标
            for (size_t r = 0; r < rows; r++) {
                mask_data[r * cols + col_idx] = 0.0f;
            }
        }

        free(col_norms);
        free(col_indices);
    } else {
        // 剪枝整行
        size_t num_pruned_rows = (size_t)(rows * sparsity);

        // 计算每行的L2范数
        float* row_norms = malloc(rows * sizeof(float));
        size_t* row_indices = malloc(rows * sizeof(size_t));
        for (size_t i = 0; i < rows; i++) {
            float norm = 0.0f;
            for (size_t j = 0; j < cols; j++) {
                norm += tensor_data[i * cols + j] * tensor_data[i * cols + j];
            }
            row_norms[i] = sqrtf(norm);
            row_indices[i] = i;  // 保留原始索引
        }

        // 找到范数最小的行（同步排序 norms 和 indices）
        for (size_t i = 0; i < num_pruned_rows; i++) {
            for (size_t j = i + 1; j < rows; j++) {
                if (row_norms[i] > row_norms[j]) {
                    float temp_n = row_norms[i];
                    row_norms[i] = row_norms[j];
                    row_norms[j] = temp_n;
                    size_t temp_i = row_indices[i];
                    row_indices[i] = row_indices[j];
                    row_indices[j] = temp_i;
                }
            }
        }

        // 剪枝范数最小的 num_pruned_rows 行
        for (size_t i = 0; i < num_pruned_rows; i++) {
            size_t row_idx = row_indices[i];  // 映射回原始行下标
            for (size_t j = 0; j < cols; j++) {
                mask_data[row_idx * cols + j] = 0.0f;
            }
        }

        free(row_norms);
        free(row_indices);
    }

    return mask;
}

// 生成剪枝掩码
Tensor* generate_prune_mask(Tensor* tensor, float sparsity, PruneMethod method) {
    switch (method) {
        case PRUNE_MAGNITUDE:
            return prune_tensor_magnitude(tensor, sparsity);
        case PRUNE_RANDOM:
            return prune_tensor_random(tensor, sparsity);
        case PRUNE_GRADIENT:
            LOG_ERROR("Gradient pruning requires gradient tensor");
            return NULL;
        case PRUNE_STRUCTURED:
            return prune_structured(tensor, sparsity, 1);
        default:
            LOG_ERROR("Unknown prune method");
            return NULL;
    }
}

// 应用剪枝掩码
void apply_prune_mask(Tensor* tensor, Tensor* mask) {
    if (!tensor || !mask) return;

    float* tensor_data = (float*)tensor->data;
    float* mask_data = (float*)mask->data;

    for (size_t i = 0; i < tensor->size; i++) {
        tensor_data[i] *= mask_data[i];
    }
}

// 剪枝模型
Model* prune_model(Model* model, PruneConfig config) {
    CHECK_NULL_RETURN(model, NULL);

    LOG_INFO("Pruning model: method=%d, sparsity=%.2f, global=%d",
             config.method, config.sparsity, config.global);

    for (size_t i = 0; i < model->num_layers; i++) {
        Layer* layer = model->layers[i];
        if (layer->trainable && layer->weights) {
            Tensor* mask = generate_prune_mask(layer->weights, config.sparsity, config.method);
            if (mask) {
                apply_prune_mask(layer->weights, mask);
                tensor_destroy(mask);
            }
        }
    }

    LOG_INFO("Model pruning completed");
    return model;
}

// 迭代剪枝
Model* iterative_prune_model(Model* model, PruneConfig config) {
    CHECK_NULL_RETURN(model, NULL);

    if (!config.iterative) {
        return prune_model(model, config);
    }

    LOG_INFO("Iterative pruning: %d iterations", config.num_iterations);

    float base_sparsity = config.sparsity;
    float increment = base_sparsity / config.num_iterations;

    for (int i = 0; i < config.num_iterations; i++) {
        float current_sparsity = (i + 1) * increment;
        config.sparsity = current_sparsity;

        LOG_INFO("Iteration %d: sparsity=%.4f", i + 1, current_sparsity);

        Model* pruned = prune_model(model, config);

        // TODO: 训练模型以恢复精度
        // train_model(pruned, ...);

        print_prune_stats(pruned);
    }

    return model;
}

// 计算模型稀疏度
float compute_model_sparsity(Model* model) {
    if (!model) return 0.0f;

    size_t total_params = 0;
    size_t zero_params = 0;

    for (size_t i = 0; i < model->num_layers; i++) {
        Layer* layer = model->layers[i];
        if (layer->trainable) {
            if (layer->weights) {
                float* data = (float*)layer->weights->data;
                for (size_t j = 0; j < layer->weights->size; j++) {
                    total_params++;
                    if (fabsf(data[j]) < 1e-6f) {
                        zero_params++;
                    }
                }
            }
            if (layer->bias) {
                float* data = (float*)layer->bias->data;
                for (size_t j = 0; j < layer->bias->size; j++) {
                    total_params++;
                    if (fabsf(data[j]) < 1e-6f) {
                        zero_params++;
                    }
                }
            }
        }
    }

    return total_params > 0 ? (float)zero_params / total_params : 0.0f;
}

// 获取剪枝统计
PruneStats get_prune_stats(Model* model) {
    PruneStats stats = {0};

    if (!model) return stats;

    stats.num_pruned_layers = 0;

    for (size_t i = 0; i < model->num_layers; i++) {
        Layer* layer = model->layers[i];
        if (layer->trainable) {
            if (layer->weights) {
                float* data = (float*)layer->weights->data;
                for (size_t j = 0; j < layer->weights->size; j++) {
                    stats.total_params++;
                    if (fabsf(data[j]) < 1e-6f) {
                        stats.zero_params++;
                    }
                }
            }
            if (layer->bias) {
                float* data = (float*)layer->bias->data;
                for (size_t j = 0; j < layer->bias->size; j++) {
                    stats.total_params++;
                    if (fabsf(data[j]) < 1e-6f) {
                        stats.zero_params++;
                    }
                }
            }
        }
    }

    stats.current_sparsity = stats.total_params > 0 ?
                              (float)stats.zero_params / stats.total_params : 0.0f;

    return stats;
}

// 打印剪枝统计
void print_prune_stats(Model* model) {
    if (!model) return;

    PruneStats stats = get_prune_stats(model);

    printf("=== Pruning Statistics ===\n");
    printf("Total parameters: %zu\n", stats.total_params);
    printf("Zero parameters: %zu\n", stats.zero_params);
    printf("Current sparsity: %.2f%%\n", stats.current_sparsity * 100);
    printf("Non-zero parameters: %zu\n", stats.total_params - stats.zero_params);
    printf("=========================\n");
}

// 恢复被剪枝的权重
void restore_pruned_weights(Model* model, Tensor** backups, size_t num_backups) {
    if (!model || !backups || num_backups == 0) return;

    LOG_INFO("Restoring pruned weights");

    size_t backup_idx = 0;
    for (size_t i = 0; i < model->num_layers && backup_idx < num_backups; i++) {
        Layer* layer = model->layers[i];
        if (layer->trainable && layer->weights) {
            if (backups[backup_idx]) {
                size_t element_size = tensor_element_size(layer->weights->dtype);
                memcpy(layer->weights->data, backups[backup_idx]->data,
                       layer->weights->size * element_size);
                backup_idx++;
            }
        }
    }
}
