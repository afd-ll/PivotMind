#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../include/tensor_pool.h"
#include "../include/error.h"

// 默认配置
#define DEFAULT_POOL_SIZE 100
#define DEFAULT_MAX_TENSOR_SIZE (1024 * 1024) // 1MB

// 创建张量池
TensorPool* tensor_pool_create(TensorPoolConfig config) {
    if (config.pool_size == 0) {
        config.pool_size = DEFAULT_POOL_SIZE;
    }
    if (config.max_tensor_size == 0) {
        config.max_tensor_size = DEFAULT_MAX_TENSOR_SIZE;
    }

    TensorPool* pool = malloc(sizeof(TensorPool));
    if (!pool) {
        LOG_ERROR("Failed to allocate tensor pool");
        return NULL;
    }

    pool->tensors = calloc(config.pool_size, sizeof(Tensor*));
    pool->used = calloc(config.pool_size, sizeof(bool));
    if (!pool->tensors || !pool->used) {
        LOG_ERROR("Failed to allocate pool arrays");
        free(pool->tensors);
        free(pool->used);
        free(pool);
        return NULL;
    }

    pool->capacity = config.pool_size;
    pool->size = 0;
    pool->total_allocated = 0;
    pool->peak_usage = 0;

    LOG_INFO("Tensor pool created with capacity %zu", pool->capacity);
    return pool;
}

// 从池中分配张量
Tensor* tensor_pool_alloc(TensorPool* pool, DataType dtype, size_t ndim, const size_t* shape) {
    CHECK_NULL(pool);
    CHECK_NULL(shape);

    // 计算张量大小
    size_t size = 1;
    for (size_t i = 0; i < ndim; i++) {
        size *= shape[i];
    }
    size_t tensor_bytes = size * tensor_element_size(dtype);

    // 检查大小限制
    if (tensor_bytes > DEFAULT_MAX_TENSOR_SIZE) {
        LOG_WARNING("Tensor size %zu bytes exceeds limit, using direct allocation", tensor_bytes);
        return tensor_create(dtype, ndim, shape);
    }

    // 尝试从池中复用张量
    for (size_t i = 0; i < pool->capacity; i++) {
        if (pool->used[i] && pool->tensors[i]) {
            Tensor* tensor = pool->tensors[i];

            // 检查类型、维度和形状是否匹配
            if (tensor->dtype == dtype && tensor->ndim == ndim &&
                tensor->size == size) {
                bool shape_match = true;
                for (size_t j = 0; j < ndim; j++) {
                    if (tensor->shape[j] != shape[j]) {
                        shape_match = false;
                        break;
                    }
                }

                if (shape_match) {
                    // 清零数据并复用
                    memset(tensor->data, 0, tensor_bytes);
                    if (tensor->grad) {
                        tensor_destroy(tensor->grad);
                        tensor->grad = NULL;
                    }
                    tensor->requires_grad = false;
                    return tensor;
                }
            }
        }
    }

    // 查找空闲槽位
    for (size_t i = 0; i < pool->capacity; i++) {
        if (!pool->used[i]) {
            // 创建新张量
            Tensor* tensor = tensor_create(dtype, ndim, shape);
            if (!tensor) {
                LOG_ERROR("Failed to create tensor in pool");
                return NULL;
            }

            pool->tensors[i] = tensor;
            pool->used[i] = true;
            pool->size++;
            pool->total_allocated += tensor_bytes;

            if (pool->size > pool->peak_usage) {
                pool->peak_usage = pool->size;
            }

            return tensor;
        }
    }

    // 池已满,直接创建
    LOG_WARNING("Tensor pool full, using direct allocation");
    return tensor_create(dtype, ndim, shape);
}

// 释放张量到池中
void tensor_pool_free(TensorPool* pool, Tensor* tensor) {
    if (!pool || !tensor) return;

    // 查找张量是否在池中
    for (size_t i = 0; i < pool->capacity; i++) {
        if (pool->tensors[i] == tensor) {
            pool->used[i] = false;
            pool->size--;
            return;
        }
    }

    // 不在池中,直接销毁
    tensor_destroy(tensor);
}

// 销毁张量池
void tensor_pool_destroy(TensorPool* pool) {
    if (!pool) return;

    for (size_t i = 0; i < pool->capacity; i++) {
        if (pool->tensors[i]) {
            tensor_destroy(pool->tensors[i]);
        }
    }

    LOG_INFO("Tensor pool destroyed. Peak usage: %zu/%zu, Total allocated: %zu bytes",
             pool->peak_usage, pool->capacity, pool->total_allocated);

    free(pool->tensors);
    free(pool->used);
    free(pool);
}

// 获取池统计信息
void tensor_pool_print_stats(TensorPool* pool) {
    if (!pool) {
        LOG_ERROR("Null pool");
        return;
    }

    printf("=== Tensor Pool Statistics ===\n");
    printf("Capacity:     %zu\n", pool->capacity);
    printf("Current size: %zu\n", pool->size);
    printf("Peak usage:   %zu (%.1f%%)\n",
           pool->peak_usage,
           100.0 * pool->peak_usage / pool->capacity);
    printf("Total allocated: %zu bytes (%.2f MB)\n",
           pool->total_allocated,
           pool->total_allocated / (1024.0 * 1024.0));
    printf("==============================\n");
}

// 清理未使用的张量
void tensor_pool_cleanup(TensorPool* pool) {
    if (!pool) return;

    size_t freed = 0;
    for (size_t i = 0; i < pool->capacity; i++) {
        if (!pool->used[i] && pool->tensors[i]) {
            tensor_destroy(pool->tensors[i]);
            pool->tensors[i] = NULL;
            freed++;
        }
    }

    if (freed > 0) {
        LOG_INFO("Cleaned up %zu unused tensors from pool", freed);
    }
}
