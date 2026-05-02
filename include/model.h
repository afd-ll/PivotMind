#ifndef MODEL_H
#define MODEL_H

#include "layer.h"
#include "optimizer.h"
#include "context.h"

// 神经网络模型
typedef struct {
    Layer** layers;        // 层数组
    size_t num_layers;    // 层数量
    Optimizer* optimizer;  // 优化器
    ContextMode mode;     // 训练/推理模式
} Model;

// 创建神经网络模型
Model* model_create();

// 添加层到模型
void model_add_layer(Model* model, Layer* layer);

// 前向传播
Tensor* model_forward(Model* model, Tensor* input);

// 训练步骤
void model_train_step(Model* model, Tensor* input, Tensor* target, float learning_rate);

// 计算均方误差损失
Tensor* model_mse_loss(Tensor* pred, Tensor* target);

// 设置优化器
void model_set_optimizer(Model* model, Optimizer* optimizer);

// 获取可训练参数
void model_get_trainable_params(Model* model, Tensor*** params, size_t* num_params);

// 销毁模型
void model_destroy(Model* model);

#endif // MODEL_H
