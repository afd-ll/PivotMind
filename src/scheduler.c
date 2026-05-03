#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <float.h>
#include "../include/scheduler.h"
#include "../include/error.h"

// 学习率调度器结构
struct LRScheduler {
    SchedulerType type;
    SchedulerConfig config;
    float current_lr;
    size_t step;
    float best_loss;
    size_t num_bad_epochs;
    bool in_warmup;
};

// 创建调度器
LRScheduler* scheduler_create(SchedulerType type, SchedulerConfig config) {
    // 设置默认值
    if (config.initial_lr <= 0) config.initial_lr = 0.001f;
    if (config.min_lr <= 0) config.min_lr = 0.0f;
    if (config.max_lr <= 0) config.max_lr = config.initial_lr;
    if (config.gamma <= 0) config.gamma = 0.1f;
    if (config.step_size <= 0) config.step_size = 10.0f;
    if (config.patience <= 0) config.patience = 10.0f;
    if (config.threshold <= 0) config.threshold = 1e-4f;

    LRScheduler* scheduler = malloc(sizeof(LRScheduler));
    if (!scheduler) {
        LOG_ERROR("Failed to allocate scheduler");
        return NULL;
    }

    scheduler->type = type;
    scheduler->config = config;
    scheduler->current_lr = config.initial_lr;
    scheduler->step = 0;
    scheduler->best_loss = INFINITY;
    scheduler->num_bad_epochs = 0;
    scheduler->in_warmup = (type == SCHEDULER_WARMUP_COSINE) && (config.warmup_steps > 0);

    LOG_INFO("Scheduler created: type=%d, initial_lr=%.6f, min_lr=%.6f",
             type, config.initial_lr, config.min_lr);

    return scheduler;
}

// 获取当前学习率
float scheduler_get_lr(LRScheduler* scheduler) {
    if (!scheduler) return 0.001f;
    return scheduler->current_lr;
}

// 固定学习率
static float lr_constant(LRScheduler* s) {
    return s->config.initial_lr;
}

// 阶梯式衰减
static float lr_step(LRScheduler* s) {
    float decay_steps = s->step / (size_t)s->config.step_size;
    float factor = powf(s->config.gamma, decay_steps);
    float lr = s->config.initial_lr * factor;
    return fmaxf(lr, s->config.min_lr);
}

// 指数衰减
static float lr_exponential(LRScheduler* s) {
    float factor = powf(s->config.gamma, s->step / s->config.step_size);
    float lr = s->config.initial_lr * factor;
    return fmaxf(lr, s->config.min_lr);
}

// 余弦退火
static float lr_cosine(LRScheduler* s) {
    float progress = s->step / s->config.step_size;
    float cosine = (1.0f + cosf(M_PI * progress)) / 2.0f;
    float lr = s->config.min_lr + (s->config.initial_lr - s->config.min_lr) * cosine;
    return fmaxf(lr, s->config.min_lr);
}

// 带预热余弦退火
static float lr_warmup_cosine(LRScheduler* s) {
    if (s->in_warmup) {
        // 预热阶段:线性增长
        float warmup_progress = s->step / s->config.warmup_steps;
        s->current_lr = s->config.initial_lr + (s->config.max_lr - s->config.initial_lr) * warmup_progress;

        if (s->step >= (size_t)s->config.warmup_steps) {
            s->in_warmup = false;
            s->step = 0; // 重新开始余弦退火
        }

        return s->current_lr;
    } else {
        // 余弦退火阶段
        return lr_cosine(s);
    }
}

// ReduceOnPlateau:基于验证损失动态调整
static void lr_reduce_on_plateau(LRScheduler* s, float val_loss) {
    if (val_loss < s->best_loss - s->config.threshold) {
        s->best_loss = val_loss;
        s->num_bad_epochs = 0;
    } else {
        s->num_bad_epochs++;
    }

    if (s->num_bad_epochs >= (size_t)s->config.patience) {
        float new_lr = s->current_lr * s->config.gamma;
        if (new_lr >= s->config.min_lr) {
            s->current_lr = new_lr;
            LOG_WARNING("Reducing learning rate to %.6f (epoch %zu, patience %zu)",
                       new_lr, s->step, s->num_bad_epochs);
            s->num_bad_epochs = 0;
        }
    }
}

// 更新调度器步数
void scheduler_step(LRScheduler* scheduler) {
    if (!scheduler) return;

    scheduler->step++;

    switch (scheduler->type) {
        case SCHEDULER_CONSTANT:
            scheduler->current_lr = lr_constant(scheduler);
            break;

        case SCHEDULER_STEP:
            scheduler->current_lr = lr_step(scheduler);
            break;

        case SCHEDULER_EXPONENTIAL:
            scheduler->current_lr = lr_exponential(scheduler);
            break;

        case SCHEDULER_COSINE:
            scheduler->current_lr = lr_cosine(scheduler);
            break;

        case SCHEDULER_WARMUP_COSINE:
            scheduler->current_lr = lr_warmup_cosine(scheduler);
            break;

        case SCHEDULER_REDUCE_ON_PLATEAU:
            // 需要调用scheduler_step_loss
            break;

        default:
            LOG_ERROR("Unknown scheduler type");
            break;
    }
}

// 基于验证损失更新
void scheduler_step_loss(LRScheduler* scheduler, float val_loss) {
    if (!scheduler) return;

    scheduler->step++;

    if (scheduler->type == SCHEDULER_REDUCE_ON_PLATEAU) {
        lr_reduce_on_plateau(scheduler, val_loss);
    } else {
        // 其他类型使用普通步进
        scheduler_step(scheduler);
    }
}

// 重置调度器
void scheduler_reset(LRScheduler* scheduler) {
    if (!scheduler) return;

    scheduler->step = 0;
    scheduler->current_lr = scheduler->config.initial_lr;
    scheduler->best_loss = INFINITY;
    scheduler->num_bad_epochs = 0;
    scheduler->in_warmup = (scheduler->type == SCHEDULER_WARMUP_COSINE) &&
                          (scheduler->config.warmup_steps > 0);

    LOG_INFO("Scheduler reset");
}

// 销毁调度器
void scheduler_destroy(LRScheduler* scheduler) {
    if (!scheduler) return;
    free(scheduler);
}

// 获取调度器类型
SchedulerType scheduler_get_type(LRScheduler* scheduler) {
    return scheduler ? scheduler->type : SCHEDULER_CONSTANT;
}
