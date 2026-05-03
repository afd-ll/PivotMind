#include "../include/layer.h"
#include "../include/common.h"
#include "../include/error.h"

// 创建全连接层
Layer* layer_create_linear(size_t input_size, size_t output_size, bool trainable) {
    init_random();

    Layer* layer = malloc(sizeof(Layer));
    CHECK_NULL(layer);

    layer->type = LAYER_LINEAR;
    layer->trainable = trainable;
    layer->output = NULL;
    layer->grad_weights = NULL;
    layer->grad_bias = NULL;
    layer->private_data = NULL;

    // 创建权重矩阵 (input_size x output_size)
    size_t weight_shape[] = {input_size, output_size};
    layer->weights = tensor_create(DT_FLOAT32, 2, weight_shape);
    layer->bias = tensor_zeros(DT_FLOAT32, 1, (size_t[]){output_size});

    if (!layer->weights || !layer->bias) {
        if (layer->weights) tensor_destroy(layer->weights);
        if (layer->bias) tensor_destroy(layer->bias);
        free(layer);
        return NULL;
    }

    // Xavier初始化 - 乘以一个缩放因子来增加输出范围
    float scale = sqrtf(2.0f / (input_size + output_size)) * 2.0f;
    float* weight_data = (float*)layer->weights->data;
    for (size_t i = 0; i < layer->weights->size; i++) {
        weight_data[i] = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * scale;
    }

    if (trainable) {
        layer->weights->requires_grad = true;
        layer->bias->requires_grad = true;
    }

    return layer;
}

// 全连接层前向传播
void layer_linear_forward(Layer* layer, Tensor* input) {
    if (layer->output) {
        tensor_destroy(layer->output);
    }

    Tensor* output = tensor_matmul(input, layer->weights);
    if (!output) return;

    // 添加偏置（广播）
    for (size_t i = 0; i < output->size; i++) {
        size_t row = i / output->shape[1];
        float* out_data = (float*)output->data;
        float* bias_data = (float*)layer->bias->data;
        out_data[i] += bias_data[row % layer->bias->size];
    }

    layer->output = output;
}

// 嵌入层和RNN层的声明
extern Layer* layer_create_embedding(int vocab_size, int embedding_dim);
extern void layer_embedding_forward(Layer* layer, Tensor* input);
extern void layer_embedding_backward(Layer* layer, Tensor* grad_output);
extern Layer* layer_create_simple_rnn(int input_size, int hidden_size);
extern void layer_rnn_forward(Layer* layer, Tensor* input);
extern void layer_rnn_backward(Layer* layer, Tensor* grad_output);

// 创建ReLU层
Layer* layer_create_relu() {
    Layer* layer = malloc(sizeof(Layer));
    CHECK_NULL(layer);

    layer->type = LAYER_RELU;
    layer->trainable = false;
    layer->weights = NULL;
    layer->bias = NULL;
    layer->output = NULL;
    layer->grad_weights = NULL;
    layer->grad_bias = NULL;
    layer->private_data = NULL;

    return layer;
}

// ReLU层前向传播
void layer_relu_forward(Layer* layer, Tensor* input) {
    if (layer->output) {
        tensor_destroy(layer->output);
    }
    layer->output = tensor_relu(input);
}

// 创建Sigmoid层
Layer* layer_create_sigmoid() {
    Layer* layer = malloc(sizeof(Layer));
    CHECK_NULL(layer);

    layer->type = LAYER_SIGMOID;
    layer->trainable = false;
    layer->weights = NULL;
    layer->bias = NULL;
    layer->output = NULL;
    layer->grad_weights = NULL;
    layer->grad_bias = NULL;
    layer->private_data = NULL;

    return layer;
}

// Sigmoid层前向传播
void layer_sigmoid_forward(Layer* layer, Tensor* input) {
    if (layer->output) {
        tensor_destroy(layer->output);
    }
    layer->output = tensor_sigmoid(input);
}

// 创建Tanh层
Layer* layer_create_tanh() {
    Layer* layer = malloc(sizeof(Layer));
    CHECK_NULL(layer);

    layer->type = LAYER_TANH;
    layer->trainable = false;
    layer->weights = NULL;
    layer->bias = NULL;
    layer->output = NULL;
    layer->grad_weights = NULL;
    layer->grad_bias = NULL;
    layer->private_data = NULL;

    return layer;
}

// Tanh层前向传播
void layer_tanh_forward(Layer* layer, Tensor* input) {
    if (layer->output) {
        tensor_destroy(layer->output);
    }
    layer->output = tensor_tanh(input);
}

// 创建Softmax层
Layer* layer_create_softmax() {
    Layer* layer = malloc(sizeof(Layer));
    CHECK_NULL(layer);

    layer->type = LAYER_SOFTMAX;
    layer->trainable = false;
    layer->weights = NULL;
    layer->bias = NULL;
    layer->output = NULL;
    layer->grad_weights = NULL;
    layer->grad_bias = NULL;
    layer->private_data = NULL;

    return layer;
}

// Softmax层前向传播
void layer_softmax_forward(Layer* layer, Tensor* input) {
    if (layer->output) {
        tensor_destroy(layer->output);
    }
    layer->output = tensor_softmax(input, 0);
}

// 统一的前向传播接口
void layer_forward(Layer* layer, Tensor* input) {
    switch (layer->type) {
        case LAYER_LINEAR:
            layer_linear_forward(layer, input);
            break;
        case LAYER_RELU:
            layer_relu_forward(layer, input);
            break;
        case LAYER_SIGMOID:
            layer_sigmoid_forward(layer, input);
            break;
        case LAYER_TANH:
            layer_tanh_forward(layer, input);
            break;
        case LAYER_SOFTMAX:
            layer_softmax_forward(layer, input);
            break;
        case LAYER_EMBEDDING:
            layer_embedding_forward(layer, input);
            break;
        case LAYER_SIMPLE_RNN:
            layer_rnn_forward(layer, input);
            break;
        default:
            break;
    }
}

// ReLU层反向传播
void layer_relu_backward(Layer* layer, Tensor* grad_output) {
    if (layer->output && grad_output) {
        float* output_data = (float*)layer->output->data;
        float* grad_data = (float*)grad_output->data;
        for (size_t i = 0; i < layer->output->size; i++) {
            grad_data[i] *= (output_data[i] > 0.0f) ? 1.0f : 0.0f;
        }
    }
}

// 全连接层反向传播
void layer_linear_backward(Layer* layer, Tensor* grad_output) {
    if (!layer->trainable || !layer->output || !grad_output) return;

    size_t weight_shape[] = {layer->weights->shape[0], layer->weights->shape[1]};
    if (!layer->grad_weights) {
        layer->grad_weights = tensor_zeros(DT_FLOAT32, 2, weight_shape);
    }
    if (!layer->grad_bias) {
        layer->grad_bias = tensor_zeros(DT_FLOAT32, 1, (size_t[]){layer->bias->shape[0]});
    }

    float* grad_b_data = (float*)layer->grad_bias->data;
    float* grad_out_data = (float*)grad_output->data;

    // 保存输入用于梯度计算(需要在前向传播时保存)
    // 这里简化处理，假设layer->private_data保存了输入

    // 计算偏置梯度: grad_bias = sum(grad_output, axis=0)
    // 对于每个输出维度，累加所有样本的梯度
    size_t batch_size = grad_output->shape[0];
    size_t output_size = grad_output->shape[1];

    for (size_t o = 0; o < output_size; o++) {
        float sum = 0.0f;
        for (size_t b = 0; b < batch_size; b++) {
            sum += grad_out_data[b * output_size + o];
        }
        grad_b_data[o] += sum;  // 累加而不是覆盖
    }

    // 注意:完整的权重梯度需要输入数据
    // grad_weights = input^T @ grad_output
    // 由于没有保存输入，这里返回NULL调用者可以自行计算
    // 在seq2seq_train.c中已经通过矩阵乘法计算了

    layer->weights->grad = layer->grad_weights;
    layer->bias->grad = layer->grad_bias;
}

// 统一的反向传播接口
void layer_backward(Layer* layer, Tensor* grad_output) {
    switch (layer->type) {
        case LAYER_LINEAR:
            layer_linear_backward(layer, grad_output);
            break;
        case LAYER_RELU:
            layer_relu_backward(layer, grad_output);
            break;
        case LAYER_EMBEDDING:
            layer_embedding_backward(layer, grad_output);
            break;
        case LAYER_SIMPLE_RNN:
            layer_rnn_backward(layer, grad_output);
            break;
        default:
            break;
    }
}

// 销毁层
void layer_destroy(Layer* layer) {
    if (!layer) return;

    if (layer->weights) tensor_destroy(layer->weights);
    if (layer->bias) tensor_destroy(layer->bias);
    if (layer->output) tensor_destroy(layer->output);
    if (layer->grad_weights) tensor_destroy(layer->grad_weights);
    if (layer->grad_bias) tensor_destroy(layer->grad_bias);
    if (layer->private_data) free(layer->private_data);

    free(layer);
}
