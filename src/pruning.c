#include "common.h"
#include "pruning.h"
#include "error.h"

// 快速选择：在 arr[left..right] 中找到第 k 小的元素（k=0 最小）
// 原地分区，O(n) 平均时间
static void quickselect_float(float* arr, size_t k, size_t left, size_t right) {
    while (left < right) {
        float pivot = arr[right];
        size_t i = left;
        for (size_t j = left; j < right; j++) {
            if (arr[j] <= pivot) {
                float tmp = arr[i];
                arr[i] = arr[j];
                arr[j] = tmp;
                i++;
            }
        }
        float tmp = arr[i];
        arr[i] = arr[right];
        arr[right] = tmp;
        if (i == k) return;
        if (i < k) left = i + 1;
        else right = i - 1;
    }
}

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

    // 找到阈值: 快速选择第 num_zeros 小的绝对值 → 它就是 threshold
    if (num_zeros > 0 && num_zeros <= mask->size) {
        quickselect_float(mask_data, num_zeros - 1, 0, mask->size - 1);
    }
    float threshold = (num_zeros > 0 && num_zeros <= mask->size) ? mask_data[num_zeros - 1] : 0.0f;

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

    // 找到阈值(剪枝梯度最小的权重): 快速选择
    if (num_zeros > 0 && num_zeros <= mask->size) {
        quickselect_float(mask_data, num_zeros - 1, 0, mask->size - 1);
    }
    float threshold = (num_zeros > 0 && num_zeros <= mask->size) ? mask_data[num_zeros - 1] : 0.0f;

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

        // 找到阈值：复制 norms 做 quickselect，找到第 num_pruned_cols 小的范数
        if (num_pruned_cols > 0 && num_pruned_cols <= cols) {
            float* norms_copy = (float*)malloc(cols * sizeof(float));
            if (norms_copy) {
                memcpy(norms_copy, col_norms, cols * sizeof(float));
                quickselect_float(norms_copy, num_pruned_cols - 1, 0, cols - 1);
                float threshold = norms_copy[num_pruned_cols - 1];
                free(norms_copy);
                // 剪枝范数 <= threshold 的列
                for (size_t c = 0; c < cols; c++) {
                    if (col_norms[c] <= threshold) {
                        for (size_t r = 0; r < rows; r++) {
                            mask_data[r * cols + c] = 0.0f;
                        }
                    }
                }
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

                // 找到阈值：复制 norms 做 quickselect，找到第 num_pruned_rows 小的范数
        if (num_pruned_rows > 0 && num_pruned_rows <= rows) {
            float* norms_copy = (float*)malloc(rows * sizeof(float));
            if (norms_copy) {
                memcpy(norms_copy, row_norms, rows * sizeof(float));
                quickselect_float(norms_copy, num_pruned_rows - 1, 0, rows - 1);
                float threshold = norms_copy[num_pruned_rows - 1];
                free(norms_copy);
                // 剪枝范数 <= threshold 的行
                for (size_t r = 0; r < rows; r++) {
                    if (row_norms[r] <= threshold) {
                        for (size_t c = 0; c < cols; c++) {
                            mask_data[r * cols + c] = 0.0f;
                        }
                    }
                }
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
