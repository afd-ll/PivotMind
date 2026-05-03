#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include "../include/quantization.h"
#include "../include/error.h"

// FP16量化(使用半精度浮点)
Tensor* quantize_fp16(Tensor* tensor) {
    CHECK_NULL_RETURN(tensor, NULL);

    size_t shape[] = {tensor->size};
    Tensor* fp16_tensor = tensor_create(DT_FLOAT32, 1, shape);

    float* input = (float*)tensor->data;
    float* output = (float*)fp16_tensor->data;

    // 简化:使用float作为FP16的代理
    // 实际应该使用半精度浮点格式
    for (size_t i = 0; i < tensor->size; i++) {
        output[i] = input[i];  // 在真实实现中这里应该转换为FP16
    }

    return fp16_tensor;
}

// FP16反量化
Tensor* dequantize_fp16(Tensor* tensor) {
    CHECK_NULL_RETURN(tensor, NULL);
    return tensor_clone(tensor);  // 简化实现
}

// INT8量化
Tensor* quantize_int8(Tensor* tensor, float scale, float zero_point) {
    CHECK_NULL_RETURN(tensor, NULL);

    size_t shape[] = {tensor->size};
    Tensor* int8_tensor = tensor_create(DT_INT32, 1, shape);  // 使用INT32存储INT8值

    float* input = (float*)tensor->data;
    int32_t* output = (int32_t*)int8_tensor->data;

    for (size_t i = 0; i < tensor->size; i++) {
        float quantized = roundf(input[i] / scale + zero_point);
        // 限制在INT8范围
        quantized = fmaxf(-128.0f, fminf(127.0f, quantized));
        output[i] = (int32_t)quantized;
    }

    return int8_tensor;
}

// INT8反量化
Tensor* dequantize_int8(Tensor* tensor, float scale, float zero_point) {
    CHECK_NULL_RETURN(tensor, NULL);

    size_t shape[] = {tensor->size};
    Tensor* float_tensor = tensor_create(DT_FLOAT32, 1, shape);

    int32_t* input = (int32_t*)tensor->data;
    float* output = (float*)float_tensor->data;

    for (size_t i = 0; i < tensor->size; i++) {
        output[i] = scale * ((float)input[i] - zero_point);
    }

    return float_tensor;
}

// 计算量化参数
void compute_quant_params(Tensor* tensor, QuantType type, float* scale, float* zero_point) {
    if (!tensor || !scale || !zero_point) return;

    float* data = (float*)tensor->data;

    // 找到最小值和最大值
    float min_val = data[0];
    float max_val = data[0];

    for (size_t i = 1; i < tensor->size; i++) {
        if (data[i] < min_val) min_val = data[i];
        if (data[i] > max_val) max_val = data[i];
    }

    // 根据量化类型计算参数
    if (type == QUANT_INT8) {
        // INT8范围: [-128, 127]
        *zero_point = 0.0f;  // 简化:使用对称量化
        *scale = fmaxf(fabsf(min_val), fabsf(max_val)) / 127.0f;
    } else if (type == QUANT_INT4) {
        // INT4范围: [-8, 7]
        *zero_point = 0.0f;
        *scale = fmaxf(fabsf(min_val), fabsf(max_val)) / 7.0f;
    } else if (type == QUANT_INT2) {
        // INT2范围: [-2, 1]
        *zero_point = 0.0f;
        *scale = fmaxf(fabsf(min_val), fabsf(max_val)) / 1.0f;
    }
}

// 伪量化(用于量化感知训练)
Tensor* fake_quantize(Tensor* tensor, QuantType type) {
    CHECK_NULL_RETURN(tensor, NULL);

    float scale, zero_point;
    compute_quant_params(tensor, type, &scale, &zero_point);

    // 量化
    Tensor* quantized = quantize_int8(tensor, scale, zero_point);

    // 反量化
    Tensor* dequantized = dequantize_int8(quantized, scale, zero_point);

    tensor_destroy(quantized);

    return dequantized;
}

// 模型量化
Model* quantize_model(Model* model, QuantConfig config) {
    CHECK_NULL_RETURN(model, NULL);

    LOG_INFO("Quantizing model: type=%d, weight_only=%d", config.type, config.weight_only);

    for (size_t i = 0; i < model->num_layers; i++) {
        quantize_layer(model->layers[i], config);
    }

    LOG_INFO("Model quantization completed");
    return model;
}

// 模型反量化
Model* dequantize_model(Model* model, QuantConfig /*config*/) {
    CHECK_NULL_RETURN(model, NULL);

    LOG_INFO("Dequantizing model");

    // 简化实现:实际需要存储原始值
    for (size_t i = 0; i < model->num_layers; i++) {
        // TODO: 实现反量化
    }

    return model;
}

// 量化感知训练准备
Model* prepare_quantized_aware_training(Model* model, QuantConfig /*config*/) {
    CHECK_NULL_RETURN(model, NULL);

    LOG_INFO("Preparing quantization aware training");

    // 添加伪量化操作到每一层
    for (size_t i = 0; i < model->num_layers; i++) {
        Layer* layer = model->layers[i];
        if (layer->trainable && layer->weights) {
            // 在前向传播时应用伪量化
            // TODO: 集成到层的实现中
        }
    }

    return model;
}

// 计算量化误差
float compute_quantization_error(Tensor* original, Tensor* quantized) {
    CHECK_NULL_RETURN(original, 0.0f);
    CHECK_NULL_RETURN(quantized, 0.0f);

    if (original->size != quantized->size) {
        LOG_ERROR("Tensor size mismatch for error computation");
        return 0.0f;
    }

    float* orig_data = (float*)original->data;
    float* quant_data = (float*)quantized->data;

    float mse = 0.0f;
    for (size_t i = 0; i < original->size; i++) {
        float diff = orig_data[i] - quant_data[i];
        mse += diff * diff;
    }

    mse /= original->size;

    return sqrtf(mse);  // 返回RMSE
}

// 后训练量化
Model* post_training_quantization(Model* model, Tensor** /*calib_data*/, size_t /*num_samples*/) {
    CHECK_NULL_RETURN(model, NULL);

    LOG_INFO("Post-training quantization with calibration samples");

    // 使用校准数据计算量化参数
    for (size_t i = 0; i < model->num_layers; i++) {
        Layer* layer = model->layers[i];
        if (layer->trainable && layer->weights) {
            // 简化:使用权重范围计算量化参数
            float scale, zero_point;
            compute_quant_params(layer->weights, QUANT_INT8, &scale, &zero_point);

            // 量化权重
            Tensor* quantized = quantize_int8(layer->weights, scale, zero_point);

            // 保存scale和zero_point到私有数据
            // TODO: 实现参数存储

            tensor_destroy(quantized);
        }
    }

    return model;
}

// 量化特定层
void quantize_layer(Layer* layer, QuantConfig config) {
    if (!layer || !layer->trainable) return;

    if (layer->weights && !config.weight_only) {
        // 量化权重
        float scale, zero_point;
        compute_quant_params(layer->weights, config.type, &scale, &zero_point);

        Tensor* quantized = quantize_int8(layer->weights, scale, zero_point);

        // 计算误差
        float error = compute_quantization_error(layer->weights, quantized);
        LOG_INFO("Layer weight quantization error: %.6f", error);

        // 保存量化参数
        // TODO: 实现参数存储机制

        tensor_destroy(quantized);
    }

    if (layer->bias && !config.weight_only) {
        // 量化偏置
        float scale, zero_point;
        compute_quant_params(layer->bias, config.type, &scale, &zero_point);

        Tensor* quantized = quantize_int8(layer->bias, scale, zero_point);

        float error = compute_quantization_error(layer->bias, quantized);
        LOG_INFO("Layer bias quantization error: %.6f", error);

        tensor_destroy(quantized);
    }
}

// 打印量化统计信息
void print_quantization_stats(Model* model) {
    if (!model) return;

    printf("=== Model Quantization Statistics ===\n");
    printf("Number of layers: %zu\n", model->num_layers);

    size_t total_params = 0;
    size_t quantized_params = 0;

    for (size_t i = 0; i < model->num_layers; i++) {
        Layer* layer = model->layers[i];
        if (layer->trainable) {
            if (layer->weights) {
                total_params += layer->weights->size;
                // TODO: 检查是否已量化
            }
            if (layer->bias) {
                total_params += layer->bias->size;
            }
        }
    }

    printf("Total parameters: %zu\n", total_params);
    printf("Quantized parameters: %zu\n", quantized_params);
    printf("Compression ratio: %.2fx\n", quantized_params > 0 ? (float)total_params / quantized_params : 1.0f);
    printf("====================================\n");
}
