#include "model.h"
#include "common.h"
#include "error.h"

// 创建神经网络模型
Model* model_create() {
    Model* model = malloc(sizeof(Model));
    CHECK_NULL(model);

    model->layers = NULL;
    model->num_layers = 0;
    model->optimizer = NULL;
    model->mode = MODE_INFERENCE;  // 默认推理模式

    return model;
}

// 添加层到模型
void model_add_layer(Model* model, Layer* layer) {
    Layer** tmp = realloc(model->layers, (model->num_layers + 1) * sizeof(Layer*));
    if (!tmp) return;
    model->layers = tmp;
    model->layers[model->num_layers] = layer;
    model->num_layers++;
}

// 前向传播
Tensor* model_forward(Model* model, Tensor* input) {
    if (!model || !input) return NULL;
    if (model->num_layers == 0) return NULL;
    
    Tensor* current = input;

    for (size_t i = 0; i < model->num_layers; i++) {
        Layer* layer = model->layers[i];
        layer_forward(layer, current);

        if (layer->output) {
            current = layer->output;
        }
    }

    // 克隆输出，避免外部销毁影响层内部状态
    Tensor* result = tensor_clone(current);
    return result;
}

// 计算均方误差损失
Tensor* model_mse_loss(Tensor* pred, Tensor* target) {
    if (!pred || !target) return NULL;

    float loss = 0.0f;
    float* pred_data = (float*)pred->data;
    float* target_data = (float*)target->data;

    size_t min_size = (pred->size < target->size) ? pred->size : target->size;

    for (size_t i = 0; i < min_size; i++) {
        float diff = pred_data[i] - target_data[i];
        loss += diff * diff;
    }
    loss /= (float)min_size;

    size_t loss_shape[] = {1};
    Tensor* loss_tensor = tensor_create(DT_FLOAT32, 1, loss_shape);
    if (loss_tensor) {
        float* loss_data = (float*)loss_tensor->data;
        loss_data[0] = loss;
    }

    return loss_tensor;
}

// 训练步骤 — 支持单样本或批量的反向传播
void model_train_step(Model* model, Tensor* input, Tensor* target, float learning_rate) {
    // 前向传播
    Tensor* pred = model_forward(model, input);

    // 计算损失
    Tensor* loss = model_mse_loss(pred, target);

    // 正确的反向传播
    if (pred && loss) {
        float lr = learning_rate;

        /* 批次大小: input->shape[0] 表示样本数 */
        size_t batch_size = input->shape[0];

        /* 输出维度: 由最后一层权重 shape[1] 决定 */
        size_t output_dim = 1;
        {
            Layer* last_layer = model->layers[model->num_layers - 1];
            if (last_layer->type == LAYER_LINEAR && last_layer->weights)
                output_dim = last_layer->weights->shape[1];
        }

        // 计算输出层梯度: dL/dout = 2 * (pred - target) / batch_size
        size_t max_grad = (batch_size * output_dim > 128) ? batch_size * output_dim : 128;
        float* current_grad = (float*)malloc(max_grad * sizeof(float));
        size_t current_grad_size = batch_size * output_dim;

        float* pred_data = (float*)pred->data;
        float* target_data = (float*)target->data;
        for (size_t i = 0; i < current_grad_size; i++)
            current_grad[i] = 2.0f * (pred_data[i] - target_data[i]) / (float)batch_size;

        // 从最后一层向前遍历
        for (int i = (int)model->num_layers - 1; i >= 0; i--) {
            Layer* layer = model->layers[i];

            if (layer->type == LAYER_RELU) {
                // ReLU层的反向传播
                float* output_data = (float*)layer->output->data;
                for (size_t j = 0; j < current_grad_size; j++) {
                    if (output_data[j] <= 0.0f) {
                        current_grad[j] = 0.0f;
                    }
                }
            }
            else if (layer->type == LAYER_LINEAR && layer->trainable) {
                // 获取前一层输出（当前层的输入）
                Tensor* layer_input = NULL;
                if (i > 0) {
                    Layer* prev_layer = model->layers[i - 1];
                    layer_input = prev_layer->output;
                } else {
                    layer_input = input;
                }

                if (layer_input) {
                    float* input_data = (float*)layer_input->data;
                    /* 使用权重矩阵的实际维度 */
                    size_t input_dim  = layer->weights->shape[0];
                    size_t output_dim = layer->weights->shape[1];

                    float* w_data = (float*)layer->weights->data;
                    float* b_data = (float*)layer->bias->data;

                    /* 批量梯度累积: dW = X^T * dout */
                    for (size_t s = 0; s < batch_size; s++) {
                        for (size_t j = 0; j < input_dim; j++) {
                            for (size_t k = 0; k < output_dim; k++) {
                                w_data[j * output_dim + k] -=
                                    lr * current_grad[s * output_dim + k] *
                                    input_data[s * input_dim + j];
                            }
                        }
                    }

                    /* 偏置更新: db = sum(dout) / batch_size */
                    for (size_t k = 0; k < output_dim; k++) {
                        float db_sum = 0.0f;
                        for (size_t s = 0; s < batch_size; s++)
                            db_sum += current_grad[s * output_dim + k];
                        b_data[k] -= lr * db_sum;
                    }

                    /* 计算传播到前一层的梯度 */
                    if (i > 0) {
                        size_t prev_dim = input_dim;
                        float* new_grad = (float*)malloc(
                            (batch_size * prev_dim) * sizeof(float));

                        for (size_t s = 0; s < batch_size; s++) {
                            for (size_t j = 0; j < prev_dim; j++) {
                                float sum = 0.0f;
                                for (size_t k = 0; k < output_dim; k++) {
                                    sum += current_grad[s * output_dim + k] *
                                           w_data[j * output_dim + k];
                                }
                                new_grad[s * prev_dim + j] = sum;
                            }
                        }

                        free(current_grad);
                        current_grad = new_grad;
                        current_grad_size = batch_size * prev_dim;
                    }
                }
            }
        }

        free(current_grad);
    }

    // 清理损失
    if (loss) tensor_destroy(loss);
}

// 设置优化器
void model_set_optimizer(Model* model, Optimizer* optimizer) {
    model->optimizer = optimizer;
}

// 获取可训练参数
void model_get_trainable_params(Model* model, Tensor*** params, size_t* num_params) {
    *num_params = 0;

    // 首先计算参数数量
    for (size_t i = 0; i < model->num_layers; i++) {
        Layer* layer = model->layers[i];
        if (layer->trainable) {
            if (layer->weights) (*num_params)++;
            if (layer->bias) (*num_params)++;
        }
    }

    if (*num_params == 0) return;

    // 分配参数数组
    *params = malloc(*num_params * sizeof(Tensor*));
    size_t idx = 0;

    for (size_t i = 0; i < model->num_layers; i++) {
        Layer* layer = model->layers[i];
        if (layer->trainable) {
            if (layer->weights) (*params)[idx++] = layer->weights;
            if (layer->bias) (*params)[idx++] = layer->bias;
        }
    }
}

// 销毁模型
void model_destroy(Model* model) {
    if (!model) return;

    for (size_t i = 0; i < model->num_layers; i++) {
        layer_destroy(model->layers[i]);
    }

    if (model->layers) free(model->layers);
    if (model->optimizer) optimizer_destroy(model->optimizer);

    free(model);
}
