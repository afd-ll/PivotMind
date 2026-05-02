#ifndef PRUNING_H
#define PRUNING_H

#include "tensor.h"
#include "model.h"

// 剪枝方法
typedef enum {
    PRUNE_MAGNITUDE,    // 基于权重大小
    PRUNE_RANDOM,       // 随机剪枝
    PRUNE_GRADIENT,     // 基于梯度
    PRUNE_STRUCTURED    // 结构化剪枝(剪枝整个神经元)
} PruneMethod;

// 剪枝配置
typedef struct {
    PruneMethod method;    // 剪枝方法
    float sparsity;        // 目标稀疏度
    bool global;           // 是否全局剪枝
    bool iterative;        // 是否迭代剪枝
    int num_iterations;     // 迭代次数
    float schedule;        // 剪枝调度器
} PruneConfig;

// 剪枝统计
typedef struct {
    size_t total_params;
    size_t zero_params;
    float current_sparsity;
    size_t num_pruned_layers;
} PruneStats;

// 剪枝模型
Model* prune_model(Model* model, PruneConfig config);

// 剪枝张量(基于幅值)
Tensor* prune_tensor_magnitude(Tensor* tensor, float sparsity);

// 剪枝张量(随机)
Tensor* prune_tensor_random(Tensor* tensor, float sparsity);

// 剪枝张量(基于梯度)
Tensor* prune_tensor_gradient(Tensor* tensor, Tensor* gradient, float sparsity);

// 结构化剪枝(剪枝整行/列)
Tensor* prune_structured(Tensor* tensor, float sparsity, int axis);

// 迭代剪枝
Model* iterative_prune_model(Model* model, PruneConfig config);

// 获取剪枝统计
PruneStats get_prune_stats(Model* model);

// 打印剪枝统计
void print_prune_stats(Model* model);

// 应用剪枝掩码
void apply_prune_mask(Tensor* tensor, Tensor* mask);

// 生成剪枝掩码
Tensor* generate_prune_mask(Tensor* tensor, float sparsity, PruneMethod method);

// 恢复被剪枝的权重
void restore_pruned_weights(Model* model, Tensor** backups, size_t num_backups);

// 检查模型稀疏度
float compute_model_sparsity(Model* model);

#endif // PRUNING_H
