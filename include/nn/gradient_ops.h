#ifndef GRADIENT_OPS_H
#define GRADIENT_OPS_H

#include "tensor.h"
#include "model.h"

// 梯度裁剪模式
typedef enum {
    CLIP_BY_VALUE,          // 按值裁剪
    CLIP_BY_NORM,           // 按范数裁剪
    CLIP_BY_GLOBAL_NORM     // 按全局范数裁剪
} ClipMode;

// 按值裁剪梯度
void clip_grad_by_value(Tensor* grad, float min_value, float max_value);

// 按范数裁剪梯度
void clip_grad_by_norm(Tensor* grad, float max_norm);

// 按全局范数裁剪所有梯度
float clip_grad_global_norm(Tensor** grads, size_t num_grads, float max_norm);

// 计算梯度范数
float compute_grad_norm(Tensor* grad);

// 检查梯度是否有效(检测NaN/Inf)
bool check_grad_valid(Tensor* grad);

// 对模型所有梯度进行裁剪
void clip_model_grads(Model* model, float max_norm, ClipMode mode);

// 梯度累积
void accum_grad(Tensor* dest, Tensor* src, float scale);

// 清零模型梯度
void zero_model_grads(Model* model);

#endif // GRADIENT_OPS_H
