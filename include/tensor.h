#ifndef TENSOR_H
#define TENSOR_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// 张量数据类型
typedef enum {
    DT_FLOAT32,
    DT_FLOAT64,
    DT_INT32,
    DT_INT64
} DataType;

// 张量结构体前向声明
typedef struct Tensor Tensor;

// 张量结构体定义
struct Tensor {
    DataType dtype;         // 数据类型
    size_t ndim;           // 维度数量
    size_t* shape;         // 形状数组
    size_t* strides;       // 步长数组
    size_t size;           // 元素总数
    void* data;            // 数据指针
    bool requires_grad;    // 是否需要梯度
    Tensor* grad;          // 梯度数据
    bool is_view;          // 是否是视图
    struct Tensor* base;   // 基础张量（如果是视图）
};

// 张量创建和销毁
Tensor* tensor_create(DataType dtype, size_t ndim, const size_t* shape);
Tensor* tensor_create_from_data(DataType dtype, size_t ndim, const size_t* shape, void* data);
Tensor* tensor_zeros(DataType dtype, size_t ndim, const size_t* shape);
Tensor* tensor_ones(DataType dtype, size_t ndim, const size_t* shape);
Tensor* tensor_rand(DataType dtype, size_t ndim, const size_t* shape);
Tensor* tensor_clone(Tensor* tensor);
void tensor_destroy(Tensor* tensor);

// 张量操作
Tensor* tensor_reshape(Tensor* tensor, size_t new_ndim, const size_t* new_shape);
Tensor* tensor_transpose(Tensor* tensor, size_t dim1, size_t dim2);
Tensor* tensor_slice(Tensor* tensor, size_t dim, size_t start, size_t end);

// 元素级运算
Tensor* tensor_add(Tensor* a, Tensor* b);
Tensor* tensor_sub(Tensor* a, Tensor* b);
Tensor* tensor_mul(Tensor* a, Tensor* b);
Tensor* tensor_div(Tensor* a, Tensor* b);
Tensor* tensor_matmul(Tensor* a, Tensor* b);

// 激活函数
Tensor* tensor_relu(Tensor* tensor);
Tensor* tensor_sigmoid(Tensor* tensor);
Tensor* tensor_tanh(Tensor* tensor);
Tensor* tensor_softmax(Tensor* tensor, size_t dim);

// 归约操作
Tensor* tensor_sum(Tensor* tensor, size_t dim, bool keepdim);
Tensor* tensor_mean(Tensor* tensor, size_t dim, bool keepdim);
Tensor* tensor_max(Tensor* tensor, size_t dim, bool keepdim);
Tensor* tensor_min(Tensor* tensor, size_t dim, bool keepdim);

// 工具函数
void tensor_print(Tensor* tensor);
bool tensor_shape_equal(Tensor* a, Tensor* b);
bool tensor_broadcastable(Tensor* a, Tensor* b);
Tensor* tensor_broadcast_to(Tensor* tensor, size_t ndim, const size_t* shape);
void tensor_zero_grad(Tensor* tensor);
void tensor_backward(Tensor* tensor, Tensor* grad);

// 内存管理
size_t tensor_element_size(DataType dtype);
void* tensor_alloc_data(size_t size, DataType dtype);
void tensor_free_data(void* data);

// 内存池管理
void tensor_enable_pool(bool enable);
void tensor_get_pool_stats(size_t* total_allocated, size_t* total_freed);
void tensor_print_pool_stats();

#endif // TENSOR_H