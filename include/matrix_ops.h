#ifndef MATRIX_OPS_H
#define MATRIX_OPS_H

#include "tensor.h"
#include <stdbool.h>

// 矩阵乘法策略
typedef enum {
    MATMUL_NAIVE,        // 简单三重循环
    MATMUL_BLOCKED,      // 分块矩阵乘法
    MATMUL_STRASSEN,     // Strassen算法
    MATMUL_AUTO          // 自动选择最优策略
} MatMulStrategy;

// 矩阵乘法配置
typedef struct {
    MatMulStrategy strategy;
    size_t block_size;    // 分块大小
    bool use_simd;        // 是否使用SIMD指令
    size_t num_threads;   // 线程数
} MatMulConfig;

// 设置矩阵乘法策略
void matrix_set_strategy(MatMulConfig config);

// 矩阵乘法(自动选择最优实现)
Tensor* matrix_multiply(Tensor* a, Tensor* b);

// 简单矩阵乘法
Tensor* matrix_multiply_naive(Tensor* a, Tensor* b);

// 分块矩阵乘法
Tensor* matrix_multiply_blocked(Tensor* a, Tensor* b, size_t block_size);

// 转置矩阵乘法优化: A^T * B
Tensor* matrix_multiply_transpose_a(Tensor* a, Tensor* b);

// 矩阵向量乘法
Tensor* matrix_vector_multiply(Tensor* matrix, Tensor* vector);

// 批量矩阵乘法
Tensor* batch_matrix_multiply(Tensor* a, Tensor* b);

// 获取当前配置
MatMulConfig matrix_get_config(void);

#endif // MATRIX_OPS_H
