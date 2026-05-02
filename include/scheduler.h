#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdbool.h>

// 学习率调度器类型
typedef enum {
    SCHEDULER_CONSTANT,          // 固定学习率
    SCHEDULER_STEP,              // 阶梯式衰减
    SCHEDULER_EXPONENTIAL,       // 指数衰减
    SCHEDULER_COSINE,            // 余弦退火
    SCHEDULER_WARMUP_COSINE,     // 带预热余弦退火
    SCHEDULER_REDUCE_ON_PLATEAU  // 基于验证损失的动态调整
} SchedulerType;

// 学习率调度器
typedef struct LRScheduler LRScheduler;

// 调度器配置
typedef struct {
    float initial_lr;           // 初始学习率
    float min_lr;              // 最小学习率
    float max_lr;              // 最大学习率(用于预热)
    float warmup_steps;        // 预热步数
    float gamma;               // 衰减因子
    float step_size;           // 衰减步长
    float patience;            // 容忍步数(用于ReduceOnPlateau)
    float threshold;           // 改善阈值
} SchedulerConfig;

// 创建调度器
LRScheduler* scheduler_create(SchedulerType type, SchedulerConfig config);

// 获取当前学习率
float scheduler_get_lr(LRScheduler* scheduler);

// 更新调度器步数
void scheduler_step(LRScheduler* scheduler);

// 基于验证损失更新(用于ReduceOnPlateau)
void scheduler_step_loss(LRScheduler* scheduler, float val_loss);

// 重置调度器
void scheduler_reset(LRScheduler* scheduler);

// 销毁调度器
void scheduler_destroy(LRScheduler* scheduler);

// 获取调度器类型
SchedulerType scheduler_get_type(LRScheduler* scheduler);

#endif // SCHEDULER_H
