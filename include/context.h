#ifndef CONTEXT_H
#define CONTEXT_H

#include <stdbool.h>

// 训练/推理模式枚举
typedef enum {
    MODE_INFERENCE = 0,  // 推理模式
    MODE_TRAINING = 1   // 训练模式
} ContextMode;

// 全局上下文管理
void context_set_mode(ContextMode mode);
ContextMode context_get_mode(void);
const char* context_mode_to_string(ContextMode mode);
bool context_is_training(void);
bool context_is_inference(void);

// 模型级上下文管理
void model_set_mode(void* model, ContextMode mode);
ContextMode model_get_mode(void* model);
bool model_is_training(void* model);
bool model_is_inference(void* model);

#endif // CONTEXT_H
