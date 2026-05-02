#ifndef OPTIMIZER_H
#define OPTIMIZER_H

#include "tensor.h"

// 优化器类型枚举
typedef enum {
    OPTIMIZER_SGD,          // 随机梯度下降
    OPTIMIZER_MOMENTUM,     // 动量优化
    OPTIMIZER_ADAM          // Adam优化
} OptimizerType;

// SGD优化器
typedef struct {
    float learning_rate;     // 学习率
    float momentum;         // 动量系数
    Tensor** velocity;      // 速度（用于momentum）
    size_t num_params;     // 参数数量
} SGDOptimizer;

// Adam优化器
typedef struct {
    float learning_rate;    // 学习率
    float beta1;           // 一阶矩估计的指数衰减率
    float beta2;           // 二阶矩估计的指数衰减率
    float epsilon;          // 数值稳定常数
    int t;                // 时间步
    Tensor** m;            // 一阶矩估计
    Tensor** v;            // 二阶矩估计
    size_t num_params;     // 参数数量
} AdamOptimizer;

// 优化器通用接口
typedef struct {
    OptimizerType type;     // 优化器类型
    void* optimizer;       // 具体优化器实例
} Optimizer;

// 创建优化器
Optimizer* optimizer_create_sgd(float learning_rate, float momentum);
Optimizer* optimizer_create_adam(float learning_rate, float beta1, float beta2, float epsilon);

// 优化步骤
void optimizer_step(Optimizer* optimizer, Tensor** params, size_t num_params);

// 销毁优化器
void optimizer_destroy(Optimizer* optimizer);

#endif // OPTIMIZER_H
