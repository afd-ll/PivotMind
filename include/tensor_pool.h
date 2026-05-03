#ifndef TENSOR_POOL_H
#define TENSOR_POOL_H

#include "tensor.h"

// 张量池配置
typedef struct {
    size_t pool_size;         // 池中最大张量数量
    size_t max_tensor_size;   // 单个张量最大大小(字节)
} TensorPoolConfig;

// 张量内存池
typedef struct TensorPool {
    Tensor** tensors;         // 张量指针数组
    bool* used;              // 使用标志
    size_t capacity;         // 池容量
    size_t size;             // 当前使用数量
    size_t total_allocated;  // 总分配字节数
    size_t peak_usage;       // 峰值使用量
} TensorPool;

// 创建张量池
TensorPool* tensor_pool_create(TensorPoolConfig config);

// 从池中分配张量
Tensor* tensor_pool_alloc(TensorPool* pool, DataType dtype, size_t ndim, const size_t* shape);

// 释放张量到池中
void tensor_pool_free(TensorPool* pool, Tensor* tensor);

// 销毁张量池
void tensor_pool_destroy(TensorPool* pool);

// 获取池统计信息
void tensor_pool_print_stats(TensorPool* pool);

// 清理未使用的张量
void tensor_pool_cleanup(TensorPool* pool);

#endif // TENSOR_POOL_H
