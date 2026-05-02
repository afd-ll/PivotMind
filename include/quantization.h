#ifndef QUANTIZATION_H
#define QUANTIZATION_H

#include "tensor.h"
#include "model.h"

// 量化类型
typedef enum {
    QUANT_FP16,       // 半精度浮点
    QUANT_INT8,       // 8位整数
    QUANT_INT4,       // 4位整数
    QUANT_INT2        // 2位整数
} QuantType;

// 量化参数
typedef struct {
    float scale;      // 缩放因子
    float zero_point; // 零点
    QuantType type;   // 量化类型
} QuantParams;

// 量化配置
typedef struct {
    QuantType type;           // 量化类型
    bool per_channel;         // 是否按通道量化
    bool symmetric;           // 是否对称量化
    bool weight_only;         // 是否仅量化权重
    float clip_min;          // 最小截断值
    float clip_max;          // 最大截断值
} QuantConfig;

// FP16量化
Tensor* quantize_fp16(Tensor* tensor);

Tensor* dequantize_fp16(Tensor* tensor);

// INT8量化
Tensor* quantize_int8(Tensor* tensor, float scale, float zero_point);

Tensor* dequantize_int8(Tensor* tensor, float scale, float zero_point);

// 计算量化参数(scale和zero_point)
void compute_quant_params(Tensor* tensor, QuantType type, float* scale, float* zero_point);

// 伪量化(用于训练量化感知)
Tensor* fake_quantize(Tensor* tensor, QuantType type);

// 模型量化
Model* quantize_model(Model* model, QuantConfig config);

// 模型反量化
Model* dequantize_model(Model* model, QuantConfig config);

// 量化感知训练
Model* prepare_quantized_aware_training(Model* model, QuantConfig config);

// 评估量化误差
float compute_quantization_error(Tensor* original, Tensor* quantized);

// 后训练量化
Model* post_training_quantization(Model* model, Tensor** calib_data, size_t num_samples);

// 量化特定层
void quantize_layer(Layer* layer, QuantConfig config);

// 获取量化统计信息
void print_quantization_stats(Model* model);

#endif // QUANTIZATION_H
