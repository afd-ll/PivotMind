#include "../include/common.h"
#include "../include/trainer.h"
#include "../include/error.h"
#include "../include/model_io.h"

// 训练器结构
struct Trainer {
    Model* model;
    TrainConfig config;
    TrainStats stats;
    TrainCallback callback;
    void* callback_user_data;
    Tensor* input_batch;
    Tensor* target_batch;
};

// 创建训练器
Trainer* trainer_create(Model* model, TrainConfig config) {
    CHECK_NULL(model);

    // 设置默认值
    if (config.learning_rate <= 0) config.learning_rate = 0.001f;
    if (config.batch_size == 0) config.batch_size = 32;
    if (config.epochs == 0) config.epochs = 10;
    if (config.log_interval == 0) config.log_interval = 100;

    Trainer* trainer = malloc(sizeof(Trainer));
    if (!trainer) {
        LOG_ERROR("Failed to allocate trainer");
        return NULL;
    }

    trainer->model = model;
    trainer->config = config;
    trainer->callback = NULL;
    trainer->callback_user_data = NULL;
    trainer->input_batch = NULL;
    trainer->target_batch = NULL;

    // 初始化统计信息
    memset(&trainer->stats, 0, sizeof(TrainStats));

    LOG_INFO("Trainer created: lr=%.4f, batch_size=%zu, epochs=%zu",
             config.learning_rate, config.batch_size, config.epochs);

    return trainer;
}

// Mini-batch训练(核心函数)
float trainer_train_minibatch(Trainer* trainer, Tensor** inputs, Tensor** targets, size_t num_samples) {
    CHECK_NULL_RETURN(trainer, 0.0f);
    CHECK_NULL_RETURN(inputs, 0.0f);
    CHECK_NULL_RETURN(targets, 0.0f);

    TrainConfig* config = &trainer->config;
    size_t batch_size = config->batch_size;
    size_t num_batches = (num_samples + batch_size - 1) / batch_size;

    float epoch_loss = 0.0f;

    // 如果需要,打乱数据
    if (config->shuffle) {
        // 创建索引数组
        size_t* indices = malloc(num_samples * sizeof(size_t));
        if (!indices) {
            LOG_ERROR("Failed to allocate indices");
            return 0.0f;
        }

        for (size_t i = 0; i < num_samples; i++) {
            indices[i] = i;
        }

        // Fisher-Yates洗牌算法
        init_random();
        for (size_t i = num_samples - 1; i > 0; i--) {
            size_t j = rand() % (i + 1);
            size_t temp = indices[i];
            indices[i] = indices[j];
            indices[j] = temp;
        }

        // 训练每个批次
        for (size_t batch_idx = 0; batch_idx < num_batches; batch_idx++) {
            size_t start = batch_idx * batch_size;
            size_t end = (start + batch_size < num_samples) ? start + batch_size : num_samples;
            size_t current_batch_size = end - start;

            // 收集批次数据
            Tensor** batch_inputs = malloc(current_batch_size * sizeof(Tensor*));
            Tensor** batch_targets = malloc(current_batch_size * sizeof(Tensor*));
            if (!batch_inputs || !batch_targets) {
                LOG_ERROR("Failed to allocate batch arrays");
                free(indices);
                free(batch_inputs);
                free(batch_targets);
                return epoch_loss;
            }

            for (size_t i = 0; i < current_batch_size; i++) {
                batch_inputs[i] = inputs[indices[start + i]];
                batch_targets[i] = targets[indices[start + i]];
            }

            // 训练批次
            float batch_loss = trainer_train_batch(trainer, batch_inputs[0], batch_targets[0]);
            epoch_loss += batch_loss;

            free(batch_inputs);
            free(batch_targets);
        }

        free(indices);
    } else {
        // 不打乱,顺序训练
        for (size_t batch_idx = 0; batch_idx < num_batches; batch_idx++) {
            size_t start = batch_idx * batch_size;

            // 训练第一个样本(简化处理)
            float batch_loss = trainer_train_batch(trainer, inputs[start], targets[start]);
            epoch_loss += batch_loss;
        }
    }

    return epoch_loss / num_batches;
}

// 训练单个批次
float trainer_train_batch(Trainer* trainer, Tensor* input_batch, Tensor* target_batch) {
    CHECK_NULL_RETURN(trainer, 0.0f);

    // 前向传播
    Tensor* pred = model_forward(trainer->model, input_batch);
    if (!pred) {
        LOG_ERROR("Forward pass failed");
        return 0.0f;
    }

    // 计算损失
    Tensor* loss = model_mse_loss(pred, target_batch);
    if (!loss) {
        tensor_destroy(pred);
        return 0.0f;
    }

    float loss_value = ((float*)loss->data)[0];

    // 反向传播并更新参数(简化:直接使用train_step)
    model_train_step(trainer->model, input_batch, target_batch, trainer->config.learning_rate);

    // 清理
    tensor_destroy(pred);
    tensor_destroy(loss);

    return loss_value;
}

// 训练一个epoch
float trainer_train_epoch(Trainer* trainer, Tensor** inputs, Tensor** targets, size_t num_samples) {
    return trainer_train_minibatch(trainer, inputs, targets, num_samples);
}

// 验证模型
float trainer_validate(Trainer* trainer, Tensor** inputs, Tensor** targets, size_t num_samples) {
    CHECK_NULL_RETURN(trainer, 0.0f);

    float total_loss = 0.0f;

    for (size_t i = 0; i < num_samples; i++) {
        Tensor* pred = model_forward(trainer->model, inputs[i]);
        if (pred) {
            Tensor* loss = model_mse_loss(pred, targets[i]);
            if (loss) {
                total_loss += ((float*)loss->data)[0];
                tensor_destroy(loss);
            }
            tensor_destroy(pred);
        }
    }

    return num_samples > 0 ? total_loss / num_samples : 0.0f;
}

// 训练模型(完整训练)
void trainer_train(Trainer* trainer, Tensor** inputs, Tensor** targets, size_t num_samples) {
    if (!trainer || !inputs || !targets) return;

    TrainConfig* config = &trainer->config;
    clock_t start_time = clock();

    LOG_INFO("Starting training for %zu epochs with %zu samples", config->epochs, num_samples);

    for (size_t epoch = 0; epoch < config->epochs; epoch++) {
        trainer->stats.current_epoch = epoch;

        // 训练一个epoch
        float train_loss = trainer_train_epoch(trainer, inputs, targets, num_samples);
        trainer->stats.train_loss = train_loss;

        // 验证(这里简化,使用训练数据)
        float val_loss = train_loss; // 应该使用验证集
        trainer->stats.val_loss = val_loss;

        // 计算统计信息
        clock_t elapsed = clock() - start_time;
        trainer->stats.samples_per_second = num_samples * (epoch + 1) / ((double)elapsed / CLOCKS_PER_SEC);

        // 输出日志
        if (epoch % config->log_interval == 0 || epoch == config->epochs - 1) {
            LOG_INFO("Epoch %zu/%zu: Loss=%.4f, Samples/sec=%.2f",
                     epoch + 1, config->epochs, train_loss, trainer->stats.samples_per_second);
        }

        // 调用回调
        if (trainer->callback) {
            trainer->callback(&trainer->stats, trainer->callback_user_data);
        }

        // 保存检查点
        if (config->checkpoint_dir && (epoch + 1) % 10 == 0) {
            char filepath[256];
            snprintf(filepath, sizeof(filepath), "%s/checkpoint_epoch_%zu.bin",
                     config->checkpoint_dir, epoch + 1);
            trainer_save_checkpoint(trainer, filepath);
        }
    }

    LOG_INFO("Training completed");
}

// 设置回调函数
void trainer_set_callback(Trainer* trainer, TrainCallback callback, void* user_data) {
    if (trainer) {
        trainer->callback = callback;
        trainer->callback_user_data = user_data;
    }
}

// 保存检查点
void trainer_save_checkpoint(Trainer* trainer, const char* filepath) {
    if (!trainer || !filepath) {
        LOG_ERROR("Invalid parameters for checkpoint save");
        return;
    }

    LOG_INFO("Saving checkpoint to %s", filepath);

    // 保存模型检查点，包含当前epoch和损失值
    int epoch = (int)trainer->stats.current_epoch;
    float loss = trainer->stats.train_loss;

    if (!model_save_checkpoint(trainer->model, filepath, epoch, loss)) {
        LOG_ERROR("Failed to save model checkpoint");
    } else {
        LOG_INFO("Checkpoint saved successfully: epoch=%d, loss=%.6f", epoch, loss);
    }
}

// 加载检查点
bool trainer_load_checkpoint(Trainer* trainer, const char* filepath) {
    if (!trainer || !filepath) {
        LOG_ERROR("Invalid parameters for checkpoint load");
        return false;
    }

    LOG_INFO("Loading checkpoint from %s", filepath);

    // 加载模型检查点
    int epoch = 0;
    float loss = 0.0f;

    if (!model_load_checkpoint(trainer->model, filepath, &epoch, &loss)) {
        LOG_ERROR("Failed to load model checkpoint");
        return false;
    }

    // 更新训练统计
    trainer->stats.current_epoch = (size_t)epoch;
    trainer->stats.train_loss = loss;

    LOG_INFO("Checkpoint loaded successfully: epoch=%d, loss=%.6f", epoch, loss);
    return true;
}

// 获取训练统计
TrainStats trainer_get_stats(Trainer* trainer) {
    return trainer ? trainer->stats : (TrainStats){0};
}

// 销毁训练器
void trainer_destroy(Trainer* trainer) {
    if (!trainer) return;

    if (trainer->input_batch) tensor_destroy(trainer->input_batch);
    if (trainer->target_batch) tensor_destroy(trainer->target_batch);

    free(trainer);
}

// 工具函数: 打乱数据
void shuffle_data(void** data, size_t /*size*/, size_t count) {
    // Initialize random seed
    init_random();

    for (size_t i = count - 1; i > 0; i--) {
        size_t j = rand() % (i + 1);
        void* temp = data[i];
        data[i] = data[j];
        data[j] = temp;
    }
}

// 工具函数: 计算准确率 (已移至 metrics.c)
