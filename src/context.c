#include "../include/context.h"
#include "../include/common.h"
#include "../include/error.h"
#include "../include/model.h"

// 全局上下文
static ContextMode g_current_mode = MODE_INFERENCE;

// 全局上下文管理函数
void context_set_mode(ContextMode mode) {
    g_current_mode = mode;
    LOG_INFO("Context mode set to: %s", context_mode_to_string(mode));
}

ContextMode context_get_mode(void) {
    return g_current_mode;
}

const char* context_mode_to_string(ContextMode mode) {
    return (mode == MODE_TRAINING) ? "TRAINING" : "INFERENCE";
}

bool context_is_training(void) {
    return g_current_mode == MODE_TRAINING;
}

bool context_is_inference(void) {
    return g_current_mode == MODE_INFERENCE;
}

// 模型级上下文管理函数
void model_set_mode(void* model, ContextMode mode) {
    if (!model) return;
    Model* m = (Model*)model;
    m->mode = mode;
    LOG_INFO("Model mode set to: %s", context_mode_to_string(mode));
}

ContextMode model_get_mode(void* model) {
    if (!model) return MODE_INFERENCE;
    return ((Model*)model)->mode;
}

bool model_is_training(void* model) {
    return model && ((Model*)model)->mode == MODE_TRAINING;
}

bool model_is_inference(void* model) {
    return !model || ((Model*)model)->mode == MODE_INFERENCE;
}
