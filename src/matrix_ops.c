#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../include/matrix_ops.h"
#include "../include/error.h"

// 默认配置
static MatMulConfig g_config = {
    .strategy = MATMUL_BLOCKED,
    .block_size = 32,
    .use_simd = false,
    .num_threads = 1
};

// 设置矩阵乘法策略
void matrix_set_strategy(MatMulConfig config) {
    g_config = config;
    LOG_INFO("Matrix multiplication strategy set: strategy=%d, block_size=%zu, simd=%d, threads=%zu",
             config.strategy, config.block_size, config.use_simd, config.num_threads);
}

// 获取当前配置
MatMulConfig matrix_get_config(void) {
    return g_config;
}

// 简单矩阵乘法
Tensor* matrix_multiply_naive(Tensor* a, Tensor* b) {
    CHECK_NULL(a);
    CHECK_NULL(b);

    if (a->ndim != 2 || b->ndim != 2) {
        LOG_ERROR("Matrices must be 2D");
        return NULL;
    }
    if (a->shape[1] != b->shape[0]) {
        LOG_ERROR("Matrix dimensions incompatible for multiplication: %zux%zu and %zux%zu",
                  a->shape[0], a->shape[1], b->shape[0], b->shape[1]);
        return NULL;
    }

    size_t M = a->shape[0];
    size_t K = a->shape[1];
    size_t N = b->shape[1];

    size_t result_shape[] = {M, N};
    Tensor* result = tensor_create(a->dtype, 2, result_shape);
    if (!result) return NULL;

    float* a_data = (float*)a->data;
    float* b_data = (float*)b->data;
    float* c_data = (float*)result->data;

    // 优化循环顺序: i-k-j (缓存友好)
    for (size_t i = 0; i < M; i++) {
        for (size_t k = 0; k < K; k++) {
            float a_ik = a_data[i * K + k];
            for (size_t j = 0; j < N; j++) {
                c_data[i * N + j] += a_ik * b_data[k * N + j];
            }
        }
    }

    return result;
}

// 分块矩阵乘法
Tensor* matrix_multiply_blocked(Tensor* a, Tensor* b, size_t block_size) {
    CHECK_NULL(a);
    CHECK_NULL(b);

    if (a->ndim != 2 || b->ndim != 2) {
        LOG_ERROR("Matrices must be 2D");
        return NULL;
    }
    if (a->shape[1] != b->shape[0]) {
        LOG_ERROR("Matrix dimensions incompatible");
        return NULL;
    }

    size_t M = a->shape[0];
    size_t K = a->shape[1];
    size_t N = b->shape[1];

    if (block_size == 0) {
        block_size = 32; // 默认分块大小
    }

    size_t result_shape[] = {M, N};
    Tensor* result = tensor_create(a->dtype, 2, result_shape);
    if (!result) return NULL;

    float* a_data = (float*)a->data;
    float* b_data = (float*)b->data;
    float* c_data = (float*)result->data;

    // 初始化结果为0
    memset(c_data, 0, M * N * sizeof(float));

    // 分块矩阵乘法
    for (size_t i = 0; i < M; i += block_size) {
        for (size_t j = 0; j < N; j += block_size) {
            for (size_t k = 0; k < K; k += block_size) {
                // 处理当前块
                size_t i_end = (i + block_size < M) ? i + block_size : M;
                size_t j_end = (j + block_size < N) ? j + block_size : N;
                size_t k_end = (k + block_size < K) ? k + block_size : K;

                for (size_t ii = i; ii < i_end; ii++) {
                    for (size_t kk = k; kk < k_end; kk++) {
                        float a_ik = a_data[ii * K + kk];
                        for (size_t jj = j; jj < j_end; jj++) {
                            c_data[ii * N + jj] += a_ik * b_data[kk * N + jj];
                        }
                    }
                }
            }
        }
    }

    return result;
}

// 矩阵乘法(自动选择)
Tensor* matrix_multiply(Tensor* a, Tensor* b) {
    CHECK_NULL(a);
    CHECK_NULL(b);

    // 根据矩阵大小自动选择策略
    size_t total_elements = a->size + b->size;

    if (total_elements < 10000) {
        // 小矩阵使用简单实现
        return matrix_multiply_naive(a, b);
    } else {
        // 大矩阵使用分块实现
        size_t block_size = (total_elements < 1000000) ? 32 : 64;
        return matrix_multiply_blocked(a, b, block_size);
    }
}

// 转置矩阵乘法: A^T * B
Tensor* matrix_multiply_transpose_a(Tensor* a, Tensor* b) {
    CHECK_NULL(a);
    CHECK_NULL(b);

    if (a->ndim != 2 || b->ndim != 2) {
        LOG_ERROR("Matrices must be 2D");
        return NULL;
    }
    if (a->shape[0] != b->shape[0]) {
        LOG_ERROR("Matrix dimensions incompatible for A^T * B");
        return NULL;
    }

    size_t K = a->shape[0];
    size_t M = a->shape[1];
    size_t N = b->shape[1];

    size_t result_shape[] = {M, N};
    Tensor* result = tensor_create(a->dtype, 2, result_shape);
    if (!result) return NULL;

    float* a_data = (float*)a->data;
    float* b_data = (float*)b->data;
    float* c_data = (float*)result->data;

    // A^T * B: 直接访问原始A的转置元素
    for (size_t i = 0; i < M; i++) {
        for (size_t j = 0; j < N; j++) {
            float sum = 0.0f;
            for (size_t k = 0; k < K; k++) {
                sum += a_data[k * M + i] * b_data[k * N + j];
            }
            c_data[i * N + j] = sum;
        }
    }

    return result;
}

// 矩阵向量乘法
Tensor* matrix_vector_multiply(Tensor* matrix, Tensor* vector) {
    CHECK_NULL(matrix);
    CHECK_NULL(vector);

    if (matrix->ndim != 2 || vector->ndim != 1) {
        LOG_ERROR("Matrix must be 2D and vector must be 1D");
        return NULL;
    }
    if (matrix->shape[1] != vector->size) {
        LOG_ERROR("Matrix-vector dimensions incompatible");
        return NULL;
    }

    size_t M = matrix->shape[0];
    size_t N = matrix->shape[1];

    size_t result_shape[] = {M};
    Tensor* result = tensor_create(matrix->dtype, 1, result_shape);
    if (!result) return NULL;

    float* m_data = (float*)matrix->data;
    float* v_data = (float*)vector->data;
    float* r_data = (float*)result->data;

    for (size_t i = 0; i < M; i++) {
        float sum = 0.0f;
        for (size_t j = 0; j < N; j++) {
            sum += m_data[i * N + j] * v_data[j];
        }
        r_data[i] = sum;
    }

    return result;
}

// 批量矩阵乘法
Tensor* batch_matrix_multiply(Tensor* a, Tensor* b) {
    CHECK_NULL(a);
    CHECK_NULL(b);

    if (a->ndim != 3 || b->ndim != 3) {
        LOG_ERROR("Batch matrices must be 3D");
        return NULL;
    }
    if (a->shape[0] != b->shape[0] || a->shape[2] != b->shape[1]) {
        LOG_ERROR("Batch matrix dimensions incompatible");
        return NULL;
    }

    size_t batch_size = a->shape[0];
    size_t M = a->shape[1];
    size_t K = a->shape[2];
    size_t N = b->shape[2];

    size_t result_shape[] = {batch_size, M, N};
    Tensor* result = tensor_create(a->dtype, 3, result_shape);
    if (!result) return NULL;

    float* a_data = (float*)a->data;
    float* b_data = (float*)b->data;
    float* c_data = (float*)result->data;

    // 对每个批次进行矩阵乘法
    for (size_t batch = 0; batch < batch_size; batch++) {
        size_t a_offset = batch * M * K;
        size_t b_offset = batch * K * N;
        size_t c_offset = batch * M * N;

        for (size_t i = 0; i < M; i++) {
            for (size_t k = 0; k < K; k++) {
                float a_ik = a_data[a_offset + i * K + k];
                for (size_t j = 0; j < N; j++) {
                    c_data[c_offset + i * N + j] +=
                        a_ik * b_data[b_offset + k * N + j];
                }
            }
        }
    }

    return result;
}
