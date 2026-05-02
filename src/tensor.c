#include "../include/tensor.h"
#include "../include/common.h"
#include "../include/error.h"
#include "../include/tensor_pool.h"

// 辅助函数声明
static size_t* tensor_get_broadcast_shape(Tensor* a, Tensor* b);

// 全局 tensor pool
static TensorPool* global_pool = NULL;
static size_t total_mallocs = 0;
static size_t total_frees = 0;

// 基本函数声明（如果标准库不可用）
#ifndef RAND_MAX
#define RAND_MAX 32767
#endif

// 张量创建和销毁
Tensor* tensor_create(DataType dtype, size_t ndim, const size_t* shape)
{
    if (ndim == 0 || !shape) {
        LOG_ERROR("Invalid tensor parameters: ndim=%zu, shape=%p", ndim, (void*)shape);
        return NULL;
    }

    // Try to allocate from global pool if enabled
    if (global_pool) {
        Tensor* tensor = tensor_pool_alloc(global_pool, dtype, ndim, shape);
        if (tensor) {
            total_mallocs++;
            LOG_DEBUG("Tensor allocated from pool: %zu bytes", tensor->size * tensor_element_size(dtype));
            return tensor;
        }
    }

    Tensor* tensor = malloc(sizeof(Tensor));
    CHECK_NULL(tensor);
    
    tensor->dtype = dtype;
    tensor->ndim = ndim;
    tensor->requires_grad = false;
    tensor->is_view = false;
    tensor->base = NULL;
    tensor->grad = NULL;
    
    // 分配形状和步长数组
    tensor->shape = malloc(ndim * sizeof(size_t));
    tensor->strides = malloc(ndim * sizeof(size_t));
    if (!tensor->shape || !tensor->strides) {
        free(tensor);
        return NULL;
    }
    
    // 计算总大小和步长
    size_t size = 1;
    for (size_t i = 0; i < ndim; i++) {
        tensor->shape[i] = shape[i];
        size *= shape[i];
    }
    tensor->size = size;
    
    // 计算步长（C风格，最后一个维度步长为1）
    size_t stride = 1;
    for (int i = ndim - 1; i >= 0; i--) {
        tensor->strides[i] = stride;
        stride *= tensor->shape[i];
    }
    
    // 分配数据内存
    tensor->data = tensor_alloc_data(size, dtype);
    if (!tensor->data) {
        free(tensor->shape);
        free(tensor->strides);
        free(tensor);
        return NULL;
    }

    total_mallocs++;
    return tensor;
}

Tensor* tensor_create_from_data(DataType dtype, size_t ndim, const size_t* shape, void* data)
{
    Tensor* tensor = tensor_create(dtype, ndim, shape);
    if (!tensor) return NULL;
    
    size_t element_size = tensor_element_size(dtype);
    memcpy(tensor->data, data, tensor->size * element_size);
    
    return tensor;
}

Tensor* tensor_zeros(DataType dtype, size_t ndim, const size_t* shape)
{
    Tensor* tensor = tensor_create(dtype, ndim, shape);
    if (!tensor) return NULL;
    
    size_t element_size = tensor_element_size(dtype);
    memset(tensor->data, 0, tensor->size * element_size);
    
    return tensor;
}

Tensor* tensor_ones(DataType dtype, size_t ndim, const size_t* shape)
{
    Tensor* tensor = tensor_create(dtype, ndim, shape);
    if (!tensor) return NULL;
    
    
    // 根据数据类型设置值为1
    if (dtype == DT_FLOAT32) {
        float* data = (float*)tensor->data;
        for (size_t i = 0; i < tensor->size; i++) {
            data[i] = 1.0f;
        }
    } else if (dtype == DT_FLOAT64) {
        double* data = (double*)tensor->data;
        for (size_t i = 0; i < tensor->size; i++) {
            data[i] = 1.0;
        }
    } else if (dtype == DT_INT32) {
        int32_t* data = (int32_t*)tensor->data;
        for (size_t i = 0; i < tensor->size; i++) {
            data[i] = 1;
        }
    } else if (dtype == DT_INT64) {
        int64_t* data = (int64_t*)tensor->data;
        for (size_t i = 0; i < tensor->size; i++) {
            data[i] = 1;
        }
    }
    
    return tensor;
}

Tensor* tensor_rand(DataType dtype, size_t ndim, const size_t* shape)
{
    Tensor* tensor = tensor_create(dtype, ndim, shape);
    if (!tensor) return NULL;
    
    // 简单随机数生成（实际应该使用更好的随机数生成器）
    if (dtype == DT_FLOAT32) {
        float* data = (float*)tensor->data;
        for (size_t i = 0; i < tensor->size; i++) {
            data[i] = (float)rand() / RAND_MAX;
        }
    }
    
    return tensor;
}

void tensor_destroy(Tensor* tensor)
{
    if (!tensor) return;

    // Return to pool if enabled
    if (global_pool && !tensor->is_view) {
        tensor_pool_free(global_pool, tensor);
        total_frees++;
        LOG_DEBUG("Tensor freed to pool");
        return;
    }

    if (!tensor->is_view) {
        tensor_free_data(tensor->data);
    }
    
    if (tensor->grad) {
        tensor_destroy(tensor->grad);
    }

    free(tensor->shape);
    free(tensor->strides);
    total_frees++;
    free(tensor);
}

// 张量操作
Tensor* tensor_reshape(Tensor* tensor, size_t new_ndim, const size_t* new_shape)
{
    if (!tensor || new_ndim == 0 || !new_shape) return NULL;
    
    // 检查新形状是否与原始大小匹配
    size_t new_size = 1;
    for (size_t i = 0; i < new_ndim; i++) {
        new_size *= new_shape[i];
    }
    
    if (new_size != tensor->size) return NULL;
    
    // 创建视图（不复制数据）
    Tensor* view = malloc(sizeof(Tensor));
    if (!view) return NULL;
    
    memcpy(view, tensor, sizeof(Tensor));
    view->ndim = new_ndim;
    view->is_view = true;
    view->base = tensor;
    
    // 分配新的形状和步长数组
    view->shape = malloc(new_ndim * sizeof(size_t));
    view->strides = malloc(new_ndim * sizeof(size_t));
    if (!view->shape || !view->strides) {
        free(view);
        return NULL;
    }
    
    memcpy(view->shape, new_shape, new_ndim * sizeof(size_t));
    
    // 重新计算步长
    size_t stride = 1;
    for (int i = new_ndim - 1; i >= 0; i--) {
        view->strides[i] = stride;
        stride *= view->shape[i];
    }
    
    return view;
}

Tensor* tensor_transpose(Tensor* tensor, size_t dim1, size_t dim2)
{
    if (!tensor || dim1 >= tensor->ndim || dim2 >= tensor->ndim) return NULL;
    
    // 创建视图
    Tensor* view = malloc(sizeof(Tensor));
    if (!view) return NULL;
    
    memcpy(view, tensor, sizeof(Tensor));
    view->is_view = true;
    view->base = tensor;
    
    // 分配新的形状和步长数组
    view->shape = malloc(tensor->ndim * sizeof(size_t));
    view->strides = malloc(tensor->ndim * sizeof(size_t));
    if (!view->shape || !view->strides) {
        free(view);
        return NULL;
    }
    
    // 交换维度
    memcpy(view->shape, tensor->shape, tensor->ndim * sizeof(size_t));
    memcpy(view->strides, tensor->strides, tensor->ndim * sizeof(size_t));
    
    size_t temp_shape = view->shape[dim1];
    size_t temp_stride = view->strides[dim1];
    
    view->shape[dim1] = view->shape[dim2];
    view->strides[dim1] = view->strides[dim2];
    
    view->shape[dim2] = temp_shape;
    view->strides[dim2] = temp_stride;
    
    return view;
}

Tensor* tensor_slice(Tensor* tensor, size_t dim, size_t start, size_t end)
{
    if (!tensor || dim >= tensor->ndim || start >= tensor->shape[dim] || end > tensor->shape[dim] || start >= end) {
        return NULL;
    }
    
    // 创建视图
    Tensor* view = malloc(sizeof(Tensor));
    if (!view) return NULL;
    
    memcpy(view, tensor, sizeof(Tensor));
    view->is_view = true;
    view->base = tensor;
    
    // 分配新的形状和步长数组
    view->shape = malloc(tensor->ndim * sizeof(size_t));
    view->strides = malloc(tensor->ndim * sizeof(size_t));
    if (!view->shape || !view->strides) {
        free(view);
        return NULL;
    }
    
    memcpy(view->shape, tensor->shape, tensor->ndim * sizeof(size_t));
    memcpy(view->strides, tensor->strides, tensor->ndim * sizeof(size_t));
    
    // 调整切片维度的形状
    view->shape[dim] = end - start;
    
    // 调整数据指针（指向切片开始位置）
    size_t offset = start * tensor->strides[dim];
    size_t element_size = tensor_element_size(tensor->dtype);
    view->data = (char*)tensor->data + offset * element_size;
    
    return view;
}

// 元素级运算
Tensor* tensor_add(Tensor* a, Tensor* b)
{
    if (!a || !b) return NULL;
    if (!tensor_broadcastable(a, b)) return NULL;
    
    // 创建输出张量
    size_t* out_shape = tensor_get_broadcast_shape(a, b);
    size_t out_ndim = max_z(a->ndim, b->ndim);
    Tensor* result = tensor_create(a->dtype, out_ndim, out_shape);

    // 简单实现：逐元素加法
    if (a->size == b->size) {
        for (size_t i = 0; i < a->size; i++) {
            float* a_data = (float*)a->data;
            float* b_data = (float*)b->data;
            float* r_data = (float*)result->data;
            r_data[i] = a_data[i] + b_data[i];
        }
    }
    
    free(out_shape);
    return result;
}

Tensor* tensor_sub(Tensor* a, Tensor* b)
{
    if (!a || !b) return NULL;
    if (!tensor_broadcastable(a, b)) return NULL;
    
    // 创建输出张量
    size_t* out_shape = tensor_get_broadcast_shape(a, b);
    size_t out_ndim = max_z(a->ndim, b->ndim);
    Tensor* result = tensor_create(a->dtype, out_ndim, out_shape);

    // 简单实现：逐元素减法
    if (a->size == b->size) {
        for (size_t i = 0; i < a->size; i++) {
            float* a_data = (float*)a->data;
            float* b_data = (float*)b->data;
            float* r_data = (float*)result->data;
            r_data[i] = a_data[i] - b_data[i];
        }
    }
    
    free(out_shape);
    return result;
}

Tensor* tensor_mul(Tensor* a, Tensor* b)
{
    if (!a || !b) return NULL;
    if (!tensor_broadcastable(a, b)) return NULL;
    
    // 创建输出张量
    size_t* out_shape = tensor_get_broadcast_shape(a, b);
    size_t out_ndim = max_z(a->ndim, b->ndim);
    Tensor* result = tensor_create(a->dtype, out_ndim, out_shape);

    // 简单实现：逐元素乘法
    if (a->size == b->size) {
        for (size_t i = 0; i < a->size; i++) {
            float* a_data = (float*)a->data;
            float* b_data = (float*)b->data;
            float* r_data = (float*)result->data;
            r_data[i] = a_data[i] * b_data[i];
        }
    }
    
    free(out_shape);
    return result;
}

Tensor* tensor_div(Tensor* a, Tensor* b)
{
    if (!a || !b) return NULL;
    if (!tensor_broadcastable(a, b)) return NULL;
    
    // 创建输出张量
    size_t* out_shape = tensor_get_broadcast_shape(a, b);
    size_t out_ndim = max_z(a->ndim, b->ndim);
    Tensor* result = tensor_create(a->dtype, out_ndim, out_shape);

    // 简单实现：逐元素除法
    if (a->size == b->size) {
        for (size_t i = 0; i < a->size; i++) {
            float* a_data = (float*)a->data;
            float* b_data = (float*)b->data;
            float* r_data = (float*)result->data;
            r_data[i] = a_data[i] / b_data[i];
        }
    }
    
    free(out_shape);
    return result;
}

Tensor* tensor_matmul(Tensor* a, Tensor* b)
{
    if (!a || !b) return NULL;
    if (a->ndim != 2 || b->ndim != 2) return NULL;
    if (a->shape[1] != b->shape[0]) return NULL;
    
    // 创建输出张量
    size_t shape[] = {a->shape[0], b->shape[1]};
    Tensor* result = tensor_create(a->dtype, 2, shape);
    
    // 简单矩阵乘法实现
    float* a_data = (float*)a->data;
    float* b_data = (float*)b->data;
    float* r_data = (float*)result->data;
    
    for (size_t i = 0; i < a->shape[0]; i++) {
        for (size_t j = 0; j < b->shape[1]; j++) {
            float sum = 0.0f;
            for (size_t k = 0; k < a->shape[1]; k++) {
                sum += a_data[i * a->shape[1] + k] * b_data[k * b->shape[1] + j];
            }
            r_data[i * b->shape[1] + j] = sum;
        }
    }
    
    return result;
}

// 激活函数
Tensor* tensor_relu(Tensor* tensor)
{
    if (!tensor) return NULL;
    
    Tensor* result = tensor_create(tensor->dtype, tensor->ndim, tensor->shape);
    if (!result) return NULL;
    
    // ReLU激活：max(0, x)
    float* input = (float*)tensor->data;
    float* output = (float*)result->data;
    
    for (size_t i = 0; i < tensor->size; i++) {
        output[i] = (input[i] > 0) ? input[i] : 0;
    }
    
    return result;
}

Tensor* tensor_sigmoid(Tensor* tensor)
{
    if (!tensor) return NULL;
    
    Tensor* result = tensor_create(tensor->dtype, tensor->ndim, tensor->shape);
    if (!result) return NULL;
    
    // Sigmoid激活：1 / (1 + exp(-x))
    float* input = (float*)tensor->data;
    float* output = (float*)result->data;
    
    for (size_t i = 0; i < tensor->size; i++) {
        output[i] = 1.0f / (1.0f + expf(-input[i]));
    }
    
    return result;
}

Tensor* tensor_tanh(Tensor* tensor)
{
    if (!tensor) return NULL;
    
    Tensor* result = tensor_create(tensor->dtype, tensor->ndim, tensor->shape);
    if (!result) return NULL;
    
    // Tanh激活
    float* input = (float*)tensor->data;
    float* output = (float*)result->data;
    
    for (size_t i = 0; i < tensor->size; i++) {
        output[i] = tanhf(input[i]);
    }
    
    return result;
}

Tensor* tensor_softmax(Tensor* tensor, size_t dim)
{
    if (!tensor || dim >= tensor->ndim) return NULL;
    
    Tensor* result = tensor_create(tensor->dtype, tensor->ndim, tensor->shape);
    if (!result) return NULL;
    
    // Softmax实现（沿指定维度归一化为概率分布）
    float* input = (float*)tensor->data;
    float* output = (float*)result->data;
    
    // 计算最大值（数值稳定性）
    float max_val = input[0];
    for (size_t i = 1; i < tensor->size; i++) {
        if (input[i] > max_val) max_val = input[i];
    }
    
    // 计算指数和
    float sum = 0.0f;
    for (size_t i = 0; i < tensor->size; i++) {
        output[i] = expf(input[i] - max_val);
        sum += output[i];
    }
    
    // 归一化
    for (size_t i = 0; i < tensor->size; i++) {
        output[i] /= sum;
    }
    
    return result;
}

// 归约操作
Tensor* tensor_sum(Tensor* tensor, size_t dim, bool keepdim)
{
    if (!tensor || dim >= tensor->ndim) return NULL;
    
    // 计算输出形状
    size_t out_ndim = keepdim ? tensor->ndim : tensor->ndim - 1;
    size_t* out_shape = malloc(out_ndim * sizeof(size_t));
    
    size_t j = 0;
    for (size_t i = 0; i < tensor->ndim; i++) {
        if (i != dim || keepdim) {
            out_shape[j++] = (i == dim && keepdim) ? 1 : tensor->shape[i];
        }
    }
    
    Tensor* result = tensor_create(tensor->dtype, out_ndim, out_shape);
    free(out_shape);
    
    if (!result) return NULL;
    
    // 求和实现（简化）
    float* input = (float*)tensor->data;
    float* output = (float*)result->data;
    
    // 简单实现：对整个张量求和
    float sum = 0.0f;
    for (size_t i = 0; i < tensor->size; i++) {
        sum += input[i];
    }
    output[0] = sum;
    
    return result;
}

Tensor* tensor_mean(Tensor* tensor, size_t dim, bool keepdim)
{
    Tensor* sum = tensor_sum(tensor, dim, keepdim);
    if (!sum) return NULL;
    
    // 计算平均值
    float* data = (float*)sum->data;
    data[0] /= tensor->size;
    
    return sum;
}

Tensor* tensor_max(Tensor* tensor, size_t dim, bool keepdim)
{
    if (!tensor) return NULL;
    
    // 计算输出形状
    size_t out_ndim = keepdim ? tensor->ndim : tensor->ndim - 1;
    size_t* out_shape = malloc(out_ndim * sizeof(size_t));
    
    size_t j = 0;
    for (size_t i = 0; i < tensor->ndim; i++) {
        if (i != dim || keepdim) {
            out_shape[j++] = (i == dim && keepdim) ? 1 : tensor->shape[i];
        }
    }
    
    Tensor* result = tensor_create(tensor->dtype, out_ndim, out_shape);
    free(out_shape);
    
    if (!result) return NULL;
    
    // 最大值实现（简化）
    float* input = (float*)tensor->data;
    float* output = (float*)result->data;
    
    float max_val = input[0];
    for (size_t i = 1; i < tensor->size; i++) {
        if (input[i] > max_val) max_val = input[i];
    }
    output[0] = max_val;
    
    return result;
}

Tensor* tensor_min(Tensor* tensor, size_t dim, bool keepdim)
{
    if (!tensor) return NULL;
    
    // 计算输出形状
    size_t out_ndim = keepdim ? tensor->ndim : tensor->ndim - 1;
    size_t* out_shape = malloc(out_ndim * sizeof(size_t));
    
    size_t j = 0;
    for (size_t i = 0; i < tensor->ndim; i++) {
        if (i != dim || keepdim) {
            out_shape[j++] = (i == dim && keepdim) ? 1 : tensor->shape[i];
        }
    }
    
    Tensor* result = tensor_create(tensor->dtype, out_ndim, out_shape);
    free(out_shape);
    
    if (!result) return NULL;
    
    // 最小值实现（简化）
    float* input = (float*)tensor->data;
    float* output = (float*)result->data;
    
    float min_val = input[0];
    for (size_t i = 1; i < tensor->size; i++) {
        if (input[i] < min_val) min_val = input[i];
    }
    output[0] = min_val;
    
    return result;
}

// 工具函数
void tensor_print(Tensor* tensor)
{
    if (!tensor) {
        printf("NULL tensor\n");
        return;
    }
    
    printf("Tensor(dtype=%d, shape=[", tensor->dtype);
    for (size_t i = 0; i < tensor->ndim; i++) {
        printf("%zu", tensor->shape[i]);
        if (i < tensor->ndim - 1) printf(", ");
    }
    printf("], size=%zu)\n", tensor->size);
}

bool tensor_shape_equal(Tensor* a, Tensor* b)
{
    if (!a || !b || a->ndim != b->ndim) return false;
    
    for (size_t i = 0; i < a->ndim; i++) {
        if (a->shape[i] != b->shape[i]) return false;
    }
    
    return true;
}

bool tensor_broadcastable(Tensor* a, Tensor* b)
{
    if (!a || !b) return false;

    // 获取两个张量的维度
    size_t a_ndim = a->ndim;
    size_t b_ndim = b->ndim;

    // 从右向左比较维度，支持广播
    // 例如: (3,4,5) 和 (4,5) 可以广播
    //       (3,1,5) 和 (4,5) 可以广播
    size_t max_ndim = (a_ndim > b_ndim) ? a_ndim : b_ndim;

    for (size_t i = 0; i < max_ndim; i++) {
        // 计算从右向左的索引
        size_t a_idx = (a_ndim > i) ? a_ndim - 1 - i : 0;
        size_t b_idx = (b_ndim > i) ? b_ndim - 1 - i : 0;

        size_t a_dim = (a_ndim > i) ? a->shape[a_idx] : 1;
        size_t b_dim = (b_ndim > i) ? b->shape[b_idx] : 1;

        // 广播规则: 维度必须相等，或其中一个为1
        if (a_dim != b_dim && a_dim != 1 && b_dim != 1) {
            return false;
        }
    }

    return true;
}

Tensor* tensor_broadcast_to(Tensor* tensor, size_t /*ndim*/, const size_t* shape)
{
    if (!tensor || !shape) return NULL;
    
    // 简化实现：直接返回原始张量
    return tensor;
}

void tensor_zero_grad(Tensor* tensor)
{
    if (!tensor || !tensor->grad) return;
    
    // 清零梯度
    Tensor* grad_tensor = (Tensor*)tensor->grad;
    size_t element_size = tensor_element_size(tensor->dtype);
    memset(grad_tensor->data, 0, grad_tensor->size * element_size);
}

void tensor_backward(Tensor* tensor, Tensor* grad)
{
    if (!tensor || !grad) return;
    
    // 简单的梯度累加实现
    if (tensor->grad == NULL) {
        // 如果还没有梯度，直接赋值
        tensor->grad = tensor_clone(grad);
    } else {
        // 梯度累加
        Tensor* temp = tensor_add(tensor->grad, grad);
        tensor_destroy(tensor->grad);
        tensor->grad = temp;
    }
}

// 内存管理
size_t tensor_element_size(DataType dtype)
{
    switch (dtype) {
        case DT_FLOAT32: return sizeof(float);
        case DT_FLOAT64: return sizeof(double);
        case DT_INT32: return sizeof(int32_t);
        case DT_INT64: return sizeof(int64_t);
        default: return 0;
    }
}

void* tensor_alloc_data(size_t size, DataType dtype)
{
    size_t element_size = tensor_element_size(dtype);
    return malloc(size * element_size);
}

void tensor_free_data(void* data)
{
    free(data);
}

// 辅助函数实现
static size_t* tensor_get_broadcast_shape(Tensor* a, Tensor* b)
{
    if (!a || !b) return NULL;
    
    size_t max_ndim = (a->ndim > b->ndim) ? a->ndim : b->ndim;
    size_t* shape = malloc(max_ndim * sizeof(size_t));
    if (!shape) return NULL;
    
    for (size_t i = 0; i < max_ndim; i++) {
        size_t a_dim = (i < a->ndim) ? a->shape[a->ndim - 1 - i] : 1;
        size_t b_dim = (i < b->ndim) ? b->shape[b->ndim - 1 - i] : 1;
        shape[max_ndim - 1 - i] = (a_dim > b_dim) ? a_dim : b_dim;
    }
    
    return shape;
}

Tensor* tensor_clone(Tensor* tensor)
{
    if (!tensor) return NULL;

    Tensor* clone = tensor_create(tensor->dtype, tensor->ndim, tensor->shape);
    if (!clone) return NULL;

    size_t element_size = tensor_element_size(tensor->dtype);
    memcpy(clone->data, tensor->data, tensor->size * element_size);
    return clone;
}

// ========== Global Tensor Pool Functions ==========

/**
 * Enable or disable global tensor pool
 *
 * @param enable true to enable pool, false to disable and destroy pool
 */
void tensor_enable_pool(bool enable) {
    if (enable && !global_pool) {
        TensorPoolConfig config = {
            .pool_size = 100,
            .max_tensor_size = 1024 * 1024  // 1MB
        };
        global_pool = tensor_pool_create(config);
        total_mallocs = 0;
        total_frees = 0;
        LOG_INFO("Tensor pool enabled");
    } else if (!enable && global_pool) {
        LOG_INFO("Tensor pool stats: allocated=%zu, freed=%zu",
                  total_mallocs, total_frees);
        tensor_pool_destroy(global_pool);
        global_pool = NULL;
    }
}

/**
 * Get pool allocation statistics
 *
 * @param total_allocated Output: number of tensors allocated
 * @param total_freed Output: number of tensors freed
 */
void tensor_get_pool_stats(size_t* allocated, size_t* freed) {
    if (allocated) *allocated = total_mallocs;
    if (freed) *freed = total_frees;
}

/**
 * Print pool statistics to stdout
 */
void tensor_print_pool_stats() {
    printf("=== Tensor Pool Statistics ===\n");
    printf("Allocated: %zu tensors\n", total_mallocs);
    printf("Freed: %zu tensors\n", total_frees);
    if (global_pool) {
        tensor_pool_print_stats(global_pool);
    }
}