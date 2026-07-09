#ifndef TRAINER_H
#define TRAINER_H

#include "model.h"
#include "tensor.h"
#include <stdbool.h>

// 训练配置
typedef struct {
    float learning_rate;       // 学习率
    size_t batch_size;         // 批次大小
    size_t epochs;             // 训练轮数
    float weight_decay;        // 权重衰减(L2正则化)
    float grad_clip;           // 梯度裁剪阈值(0表示不裁剪)
    bool shuffle;              // 是否打乱数据
    size_t log_interval;       // 日志输出间隔
    char* checkpoint_dir;     // 检查点保存目录
} TrainConfig;

// 训练统计
typedef struct {
    size_t current_epoch;
    size_t total_samples;
    float train_loss;
    float val_loss;
    float train_accuracy;
    float val_accuracy;
    size_t samples_per_second;
} TrainStats;

// 训练回调函数类型
typedef void (*TrainCallback)(TrainStats* stats, void* user_data);

// 创建训练器
typedef struct Trainer Trainer;

Trainer* trainer_create(Model* model, TrainConfig config);

// 训练一个epoch
float trainer_train_epoch(Trainer* trainer, Tensor** inputs, Tensor** targets, size_t num_samples);

// 验证模型
float trainer_validate(Trainer* trainer, Tensor** inputs, Tensor** targets, size_t num_samples);

// 训练模型(完整训练)
void trainer_train(Trainer* trainer, Tensor** inputs, Tensor** targets, size_t num_samples);

// 训练单个批次
float trainer_train_batch(Trainer* trainer, Tensor* input_batch, Tensor* target_batch);

// Mini-batch训练(核心函数)
float trainer_train_minibatch(Trainer* trainer, Tensor** inputs, Tensor** targets, size_t num_samples);

// 设置回调函数
void trainer_set_callback(Trainer* trainer, TrainCallback callback, void* user_data);

// 保存检查点
void trainer_save_checkpoint(Trainer* trainer, const char* filepath);

// 加载检查点
bool trainer_load_checkpoint(Trainer* trainer, const char* filepath);

// 获取训练统计
TrainStats trainer_get_stats(Trainer* trainer);

// 销毁训练器
void trainer_destroy(Trainer* trainer);

// 工具函数: 打乱数据
void shuffle_data(void** data, size_t size, size_t count);

// 工具函数: 计算准确率
float compute_accuracy(Tensor* predictions, Tensor* targets);

#endif // TRAINER_H
