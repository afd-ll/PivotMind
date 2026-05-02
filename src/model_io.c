#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include "../include/model_io.h"
#include "../include/error.h"
#include "../include/layer.h"

// 魔数常量
#define MODEL_MAGIC 0x4D4F444C // 'MODL'

// 写入魔数
static bool write_magic(FILE* fp) {
    uint32_t magic = MODEL_MAGIC;
    return fwrite(&magic, sizeof(uint32_t), 1, fp) == 1;
}

// 读取魔数
static bool read_magic(FILE* fp) {
    uint32_t magic;
    if (fread(&magic, sizeof(uint32_t), 1, fp) != 1) {
        return false;
    }
    return magic == MODEL_MAGIC;
}

// 获取当前时间字符串
static void get_timestamp(char* buffer, size_t size) {
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    if (tm_info) {
        strftime(buffer, size, "%Y-%m-%d %H:%M:%S", tm_info);
    } else {
        strncpy(buffer, "Unknown Time", size - 1);
        buffer[size - 1] = '\0';
    }
}

// 计算模型参数数量
static size_t count_model_params(Model* model) {
    size_t count = 0;
    for (size_t i = 0; i < model->num_layers; i++) {
        Layer* layer = model->layers[i];
        if (layer->trainable) {
            if (layer->weights) count += layer->weights->size;
            if (layer->bias) count += layer->bias->size;
        }
    }
    return count;
}

// 计算模型参数总字节数
static size_t count_model_param_bytes(Model* model) {
    size_t bytes = 0;
    for (size_t i = 0; i < model->num_layers; i++) {
        Layer* layer = model->layers[i];
        if (layer->trainable) {
            if (layer->weights) bytes += layer->weights->size * sizeof(float);
            if (layer->bias) bytes += layer->bias->size * sizeof(float);
        }
    }
    return bytes;
}

// 保存模型到文件
bool model_save(Model* model, const char* filepath, ModelMetadata* metadata) {
    FILE* fp = NULL;
    bool success = false;

    CHECK_NULL_RETURN(model, false);
    CHECK_NULL_RETURN(filepath, false);

    fp = fopen(filepath, "wb");
    if (!fp) {
        LOG_ERROR("Failed to open file for writing: %s", filepath);
        return false;
    }

    // 写入魔数和版本
    if (!write_magic(fp)) {
        LOG_ERROR("Failed to write magic number");
        goto cleanup;
    }

    int32_t version = MODEL_FORMAT_VERSION;
    if (fwrite(&version, sizeof(int32_t), 1, fp) != 1) {
        LOG_ERROR("Failed to write version");
        goto cleanup;
    }

    // 写入元数据
    ModelMetadata meta;
    if (metadata) {
        meta = *metadata;
    } else {
        memset(&meta, 0, sizeof(meta));
        strncpy(meta.name, "Unnamed Model", sizeof(meta.name) - 1);
        meta.name[sizeof(meta.name) - 1] = '\0';
        strncpy(meta.description, "No description", sizeof(meta.description) - 1);
        meta.description[sizeof(meta.description) - 1] = '\0';
        meta.version = 1;
    }

    get_timestamp(meta.created_at, sizeof(meta.created_at));
    meta.num_params = count_model_params(model);
    meta.param_size_bytes = count_model_param_bytes(model);

    if (fwrite(&meta, sizeof(ModelMetadata), 1, fp) != 1) {
        LOG_ERROR("Failed to write metadata");
        goto cleanup;
    }

    // 写入层数
    int32_t num_layers = (int32_t)model->num_layers;
    if (fwrite(&num_layers, sizeof(int32_t), 1, fp) != 1) {
        LOG_ERROR("Failed to write num_layers");
        goto cleanup;
    }

    // 写入每层的参数
    for (size_t i = 0; i < model->num_layers; i++) {
        Layer* layer = model->layers[i];

        // 写入层类型
        int32_t layer_type = (int32_t)layer->type;
        if (fwrite(&layer_type, sizeof(int32_t), 1, fp) != 1) {
            LOG_ERROR("Failed to write layer type");
            goto cleanup;
        }

        // 写入是否可训练
        int32_t trainable = layer->trainable ? 1 : 0;
        if (fwrite(&trainable, sizeof(int32_t), 1, fp) != 1) {
            LOG_ERROR("Failed to write trainable flag");
            goto cleanup;
        }

        // 写入权重
        int32_t has_weights = layer->weights ? 1 : 0;
        if (fwrite(&has_weights, sizeof(int32_t), 1, fp) != 1) {
            LOG_ERROR("Failed to write has_weights");
            goto cleanup;
        }

        if (has_weights) {
            // 写入维度
            int32_t ndim = (int32_t)layer->weights->ndim;
            if (fwrite(&ndim, sizeof(int32_t), 1, fp) != 1) {
                LOG_ERROR("Failed to write weights ndim");
                goto cleanup;
            }
            if (fwrite(layer->weights->shape, sizeof(size_t), ndim, fp) != (size_t)ndim) {
                LOG_ERROR("Failed to write weights shape");
                goto cleanup;
            }

            // 写入数据
            size_t elem_size = tensor_element_size(layer->weights->dtype);
            size_t bytes_to_write = elem_size * layer->weights->size;
            if (fwrite(layer->weights->data, 1, bytes_to_write, fp) != bytes_to_write) {
                LOG_ERROR("Failed to write complete weight data");
                goto cleanup;
            }
        }

        // 写入偏置
        int32_t has_bias = layer->bias ? 1 : 0;
        if (fwrite(&has_bias, sizeof(int32_t), 1, fp) != 1) {
            LOG_ERROR("Failed to write has_bias");
            goto cleanup;
        }

        if (has_bias) {
            // 写入维度
            int32_t ndim = (int32_t)layer->bias->ndim;
            if (fwrite(&ndim, sizeof(int32_t), 1, fp) != 1) {
                LOG_ERROR("Failed to write bias ndim");
                goto cleanup;
            }
            if (fwrite(layer->bias->shape, sizeof(size_t), ndim, fp) != (size_t)ndim) {
                LOG_ERROR("Failed to write bias shape");
                goto cleanup;
            }

            // 写入数据
            size_t elem_size = tensor_element_size(layer->bias->dtype);
            size_t bytes_to_write = elem_size * layer->bias->size;
            if (fwrite(layer->bias->data, 1, bytes_to_write, fp) != bytes_to_write) {
                LOG_ERROR("Failed to write complete bias data");
                goto cleanup;
            }
        }
    }

    success = true;
    LOG_INFO("Model saved to %s (%zu params, %.2f MB)",
             filepath, meta.num_params, meta.param_size_bytes / (1024.0 * 1024.0));

cleanup:
    if (fp) fclose(fp);
    return success;
}

// 从文件加载模型
Model* model_load(const char* filepath) {
    FILE* fp = NULL;
    Model* model = NULL;
    size_t* shape_weights = NULL;
    size_t* shape_bias = NULL;
    float* weight_data = NULL;
    float* bias_data = NULL;

    CHECK_NULL_RETURN(filepath, NULL);

    fp = fopen(filepath, "rb");
    if (!fp) {
        LOG_ERROR("Failed to open file for reading: %s", filepath);
        return NULL;
    }

    // 读取魔数
    if (!read_magic(fp)) {
        LOG_ERROR("Invalid model file format");
        goto cleanup;
    }

    // 读取版本
    int32_t version;
    if (fread(&version, sizeof(int32_t), 1, fp) != 1) {
        LOG_ERROR("Failed to read version");
        goto cleanup;
    }

    if (version != MODEL_FORMAT_VERSION) {
        LOG_WARNING("Model version mismatch: expected %d, got %d",
                    MODEL_FORMAT_VERSION, version);
    }

    // 读取元数据
    ModelMetadata meta;
    if (fread(&meta, sizeof(ModelMetadata), 1, fp) != 1) {
        LOG_ERROR("Failed to read metadata");
        goto cleanup;
    }

    LOG_INFO("Loading model: %s v%d", meta.name, meta.version);

    // 读取层数
    int32_t num_layers;
    if (fread(&num_layers, sizeof(int32_t), 1, fp) != 1) {
        LOG_ERROR("Failed to read num_layers");
        goto cleanup;
    }

    if (num_layers < 0 || num_layers > 1000) {
        LOG_ERROR("Invalid num_layers: %d", num_layers);
        goto cleanup;
    }

    // 创建模型
    model = model_create();
    if (!model) {
        LOG_ERROR("Failed to create model");
        goto cleanup;
    }

    // 读取每层参数
    for (int32_t i = 0; i < num_layers; i++) {
        int32_t layer_type, trainable;

        if (fread(&layer_type, sizeof(int32_t), 1, fp) != 1) {
            LOG_ERROR("Failed to read layer_type for layer %d", i);
            goto cleanup;
        }

        if (fread(&trainable, sizeof(int32_t), 1, fp) != 1) {
            LOG_ERROR("Failed to read trainable flag for layer %d", i);
            goto cleanup;
        }

        Layer* layer = NULL;

        // 根据层类型创建相应的层
        if (layer_type == LAYER_LINEAR) {
            // 读取权重维度
            int32_t has_weights;
            if (fread(&has_weights, sizeof(int32_t), 1, fp) != 1) {
                LOG_ERROR("Failed to read has_weights for layer %d", i);
                goto cleanup;
            }

            size_t input_size = 0;
            size_t output_size = 0;

            if (has_weights) {
                int32_t ndim;
                if (fread(&ndim, sizeof(int32_t), 1, fp) != 1) {
                    LOG_ERROR("Failed to read weights ndim for layer %d", i);
                    goto cleanup;
                }

                if (ndim < 2) {
                    LOG_ERROR("Invalid weight dimensions: %d for layer %d", ndim, i);
                    goto cleanup;
                }

                shape_weights = malloc(ndim * sizeof(size_t));
                if (!shape_weights) {
                    LOG_ERROR("Failed to allocate memory for weights shape");
                    goto cleanup;
                }

                if (fread(shape_weights, sizeof(size_t), ndim, fp) != (size_t)ndim) {
                    LOG_ERROR("Failed to read weights shape for layer %d", i);
                    goto cleanup;
                }

                input_size = shape_weights[0];
                output_size = shape_weights[1];
                free(shape_weights);
                shape_weights = NULL;
            }

            // 读取偏置维度（用于验证）
            int32_t has_bias;
            if (fread(&has_bias, sizeof(int32_t), 1, fp) != 1) {
                LOG_ERROR("Failed to read has_bias for layer %d", i);
                goto cleanup;
            }

            if (has_bias) {
                int32_t ndim;
                if (fread(&ndim, sizeof(int32_t), 1, fp) != 1) {
                    LOG_ERROR("Failed to read bias ndim for layer %d", i);
                    goto cleanup;
                }

                shape_bias = malloc(ndim * sizeof(size_t));
                if (!shape_bias) {
                    LOG_ERROR("Failed to allocate memory for bias shape");
                    goto cleanup;
                }

                if (fread(shape_bias, sizeof(size_t), ndim, fp) != (size_t)ndim) {
                    LOG_ERROR("Failed to read bias shape for layer %d", i);
                    goto cleanup;
                }

                free(shape_bias);
                shape_bias = NULL;
            }

            // 创建线性层
            layer = layer_create_linear(input_size, output_size, trainable != 0);
            if (!layer) {
                LOG_ERROR("Failed to create linear layer %d", i);
                goto cleanup;
            }

            // 读取并填充权重数据
            if (has_weights && layer && layer->weights) {
                size_t weight_size = input_size * output_size;
                weight_data = malloc(weight_size * sizeof(float));
                if (!weight_data) {
                    LOG_ERROR("Failed to allocate memory for weight data");
                    goto cleanup;
                }

                if (fread(weight_data, sizeof(float), weight_size, fp) != weight_size) {
                    LOG_ERROR("Failed to read complete weight data for layer %d", i);
                    goto cleanup;
                }

                // 复制到层的权重张量
                float* layer_weight_data = (float*)layer->weights->data;
                for (size_t j = 0; j < weight_size; j++) {
                    layer_weight_data[j] = weight_data[j];
                }
                free(weight_data);
                weight_data = NULL;
            }

            // 读取并填充偏置数据
            if (has_bias && layer && layer->bias) {
                size_t bias_size = output_size;
                bias_data = malloc(bias_size * sizeof(float));
                if (!bias_data) {
                    LOG_ERROR("Failed to allocate memory for bias data");
                    goto cleanup;
                }

                if (fread(bias_data, sizeof(float), bias_size, fp) != bias_size) {
                    LOG_ERROR("Failed to read complete bias data for layer %d", i);
                    goto cleanup;
                }

                // 复制到层的偏置张量
                float* layer_bias_data = (float*)layer->bias->data;
                for (size_t j = 0; j < bias_size; j++) {
                    layer_bias_data[j] = bias_data[j];
                }
                free(bias_data);
                bias_data = NULL;
            }
        } else if (layer_type == LAYER_RELU) {
            layer = layer_create_relu();
            if (!layer) {
                LOG_ERROR("Failed to create ReLU layer %d", i);
                goto cleanup;
            }
        } else if (layer_type == LAYER_SIGMOID) {
            layer = layer_create_sigmoid();
            if (!layer) {
                LOG_ERROR("Failed to create Sigmoid layer %d", i);
                goto cleanup;
            }
        } else if (layer_type == LAYER_TANH) {
            layer = layer_create_tanh();
            if (!layer) {
                LOG_ERROR("Failed to create Tanh layer %d", i);
                goto cleanup;
            }
        } else if (layer_type == LAYER_SOFTMAX) {
            layer = layer_create_softmax();
            if (!layer) {
                LOG_ERROR("Failed to create Softmax layer %d", i);
                goto cleanup;
            }
        } else {
            LOG_ERROR("Unsupported layer type: %d for layer %d", layer_type, i);
            goto cleanup;
        }

        // 将层添加到模型
        if (layer) {
            model_add_layer(model, layer);
        }
    }

    LOG_INFO("Model loaded from %s", filepath);

    fclose(fp);
    fp = NULL;
    return model;

cleanup:
    if (shape_weights) free(shape_weights);
    if (shape_bias) free(shape_bias);
    if (weight_data) free(weight_data);
    if (bias_data) free(bias_data);
    if (fp) fclose(fp);
    if (model) model_destroy(model);
    return NULL;
}

// 保存模型权重(简化版)
bool model_save_weights(Model* model, const char* filepath) {
    FILE* fp = NULL;
    bool success = false;

    CHECK_NULL_RETURN(model, false);
    CHECK_NULL_RETURN(filepath, false);

    fp = fopen(filepath, "wb");
    if (!fp) {
        LOG_ERROR("Failed to open file for writing: %s", filepath);
        return false;
    }

    // 简单格式:每层参数依次写入
    for (size_t i = 0; i < model->num_layers; i++) {
        Layer* layer = model->layers[i];

        if (layer->trainable && layer->weights) {
            size_t bytes_to_write = layer->weights->size * sizeof(float);
            if (fwrite(layer->weights->data, 1, bytes_to_write, fp) != bytes_to_write) {
                LOG_ERROR("Failed to write weights for layer %zu", i);
                goto cleanup;
            }
        }

        if (layer->trainable && layer->bias) {
            size_t bytes_to_write = layer->bias->size * sizeof(float);
            if (fwrite(layer->bias->data, 1, bytes_to_write, fp) != bytes_to_write) {
                LOG_ERROR("Failed to write bias for layer %zu", i);
                goto cleanup;
            }
        }
    }

    success = true;
    LOG_INFO("Model weights saved to %s", filepath);

cleanup:
    if (fp) fclose(fp);
    return success;
}

// 加载模型权重
bool model_load_weights(Model* model, const char* filepath) {
    FILE* fp = NULL;
    bool success = false;

    CHECK_NULL_RETURN(model, false);
    CHECK_NULL_RETURN(filepath, false);

    fp = fopen(filepath, "rb");
    if (!fp) {
        LOG_ERROR("Failed to open file for reading: %s", filepath);
        return false;
    }

    // 简单格式:每层参数依次读取
    for (size_t i = 0; i < model->num_layers; i++) {
        Layer* layer = model->layers[i];

        if (layer->trainable && layer->weights) {
            size_t bytes_to_read = layer->weights->size * sizeof(float);
            if (fread(layer->weights->data, 1, bytes_to_read, fp) != bytes_to_read) {
                LOG_ERROR("Failed to read weights for layer %zu", i);
                goto cleanup;
            }
        }

        if (layer->trainable && layer->bias) {
            size_t bytes_to_read = layer->bias->size * sizeof(float);
            if (fread(layer->bias->data, 1, bytes_to_read, fp) != bytes_to_read) {
                LOG_ERROR("Failed to read bias for layer %zu", i);
                goto cleanup;
            }
        }
    }

    success = true;
    LOG_INFO("Model weights loaded from %s", filepath);

cleanup:
    if (fp) fclose(fp);
    return success;
}

// 保存训练检查点
bool model_save_checkpoint(Model* model, const char* filepath, int epoch, float loss) {
    FILE* fp = NULL;
    bool success = false;

    CHECK_NULL_RETURN(model, false);
    CHECK_NULL_RETURN(filepath, false);

    fp = fopen(filepath, "wb");
    if (!fp) {
        LOG_ERROR("Failed to open checkpoint file: %s", filepath);
        return false;
    }

    // 写入检查点信息
    int32_t epoch_int32 = (int32_t)epoch;
    if (fwrite(&epoch_int32, sizeof(int32_t), 1, fp) != 1) {
        LOG_ERROR("Failed to write epoch");
        goto cleanup;
    }
    if (fwrite(&loss, sizeof(float), 1, fp) != 1) {
        LOG_ERROR("Failed to write loss");
        goto cleanup;
    }

    // 写入模型权重
    for (size_t i = 0; i < model->num_layers; i++) {
        Layer* layer = model->layers[i];

        if (layer->trainable && layer->weights) {
            size_t bytes_to_write = layer->weights->size * sizeof(float);
            if (fwrite(layer->weights->data, 1, bytes_to_write, fp) != bytes_to_write) {
                LOG_ERROR("Failed to write weights for layer %zu in checkpoint", i);
                goto cleanup;
            }
        }

        if (layer->trainable && layer->bias) {
            size_t bytes_to_write = layer->bias->size * sizeof(float);
            if (fwrite(layer->bias->data, 1, bytes_to_write, fp) != bytes_to_write) {
                LOG_ERROR("Failed to write bias for layer %zu in checkpoint", i);
                goto cleanup;
            }
        }
    }

    success = true;
    LOG_INFO("Checkpoint saved: epoch=%d, loss=%.4f", epoch, loss);

cleanup:
    if (fp) fclose(fp);
    return success;
}

// 加载训练检查点
bool model_load_checkpoint(Model* model, const char* filepath, int* epoch, float* loss) {
    FILE* fp = NULL;
    bool success = false;

    CHECK_NULL_RETURN(model, false);
    CHECK_NULL_RETURN(filepath, false);

    fp = fopen(filepath, "rb");
    if (!fp) {
        LOG_ERROR("Failed to open checkpoint file: %s", filepath);
        return false;
    }

    // 读取检查点信息
    int32_t epoch_int32;
    float loss_value;

    if (fread(&epoch_int32, sizeof(int32_t), 1, fp) != 1) {
        LOG_ERROR("Failed to read epoch from checkpoint");
        goto cleanup;
    }
    if (fread(&loss_value, sizeof(float), 1, fp) != 1) {
        LOG_ERROR("Failed to read loss from checkpoint");
        goto cleanup;
    }

    if (epoch) *epoch = (int)epoch_int32;
    if (loss) *loss = loss_value;

    // 读取模型权重
    for (size_t i = 0; i < model->num_layers; i++) {
        Layer* layer = model->layers[i];

        if (layer->trainable && layer->weights) {
            size_t bytes_to_read = layer->weights->size * sizeof(float);
            if (fread(layer->weights->data, 1, bytes_to_read, fp) != bytes_to_read) {
                LOG_ERROR("Failed to read weights for layer %zu from checkpoint", i);
                goto cleanup;
            }
        }

        if (layer->trainable && layer->bias) {
            size_t bytes_to_read = layer->bias->size * sizeof(float);
            if (fread(layer->bias->data, 1, bytes_to_read, fp) != bytes_to_read) {
                LOG_ERROR("Failed to read bias for layer %zu from checkpoint", i);
                goto cleanup;
            }
        }
    }

    success = true;
    LOG_INFO("Checkpoint loaded: epoch=%d, loss=%.4f", epoch_int32, loss_value);

cleanup:
    if (fp) fclose(fp);
    return success;
}

// 导出模型到文本格式
bool model_export_text(Model* model, const char* filepath) {
    FILE* fp = NULL;
    bool success = false;

    CHECK_NULL_RETURN(model, false);
    CHECK_NULL_RETURN(filepath, false);

    fp = fopen(filepath, "w");
    if (!fp) {
        LOG_ERROR("Failed to open file for writing: %s", filepath);
        return false;
    }

    fprintf(fp, "=== Model Structure ===\n\n");
    fprintf(fp, "Number of layers: %zu\n\n", model->num_layers);

    for (size_t i = 0; i < model->num_layers; i++) {
        Layer* layer = model->layers[i];
        fprintf(fp, "Layer %zu:\n", i);
        fprintf(fp, "  Type: %d\n", layer->type);
        fprintf(fp, "  Trainable: %s\n", layer->trainable ? "Yes" : "No");

        if (layer->weights) {
            fprintf(fp, "  Weights shape: [");
            if (layer->weights->ndim > 0) {
                for (size_t j = 0; j < layer->weights->ndim; j++) {
                    fprintf(fp, "%zu", layer->weights->shape[j]);
                    if (j < layer->weights->ndim - 1) fprintf(fp, ", ");
                }
            }
            fprintf(fp, "]\n");

            // 计算真实的权重范围
            if (layer->weights->size > 0) {
                float* data = (float*)layer->weights->data;
                float min_val = data[0];
                float max_val = data[0];

                for (size_t j = 1; j < layer->weights->size; j++) {
                    if (data[j] < min_val) min_val = data[j];
                    if (data[j] > max_val) max_val = data[j];
                }

                fprintf(fp, "  Weights range: [%.6f, %.6f]\n", min_val, max_val);
            }
        }

        if (layer->bias) {
            fprintf(fp, "  Bias shape: [");
            if (layer->bias->ndim > 0) {
                for (size_t j = 0; j < layer->bias->ndim; j++) {
                    fprintf(fp, "%zu", layer->bias->shape[j]);
                    if (j < layer->bias->ndim - 1) fprintf(fp, ", ");
                }
            }
            fprintf(fp, "]\n");
        }

        fprintf(fp, "\n");
    }

    success = true;
    LOG_INFO("Model exported to text: %s", filepath);

    if (fp) fclose(fp);
    return success;
}

// 比较两个模型是否相等
bool model_equal(Model* model1, Model* model2, float tolerance) {
    CHECK_NULL_RETURN(model1, false);
    CHECK_NULL_RETURN(model2, false);

    if (model1->num_layers != model2->num_layers) {
        return false;
    }

    for (size_t i = 0; i < model1->num_layers; i++) {
        Layer* layer1 = model1->layers[i];
        Layer* layer2 = model2->layers[i];

        if (layer1->type != layer2->type) return false;

        // 检查权重是否存在性是否一致
        if ((layer1->weights == NULL) != (layer2->weights == NULL)) {
            return false;
        }

        // 比较权重
        if (layer1->weights && layer2->weights) {
            if (layer1->weights->size != layer2->weights->size) return false;

            float* w1 = (float*)layer1->weights->data;
            float* w2 = (float*)layer2->weights->data;

            for (size_t j = 0; j < layer1->weights->size; j++) {
                if (fabsf(w1[j] - w2[j]) > tolerance) return false;
            }
        }

        // 检查偏置是否存在性是否一致
        if ((layer1->bias == NULL) != (layer2->bias == NULL)) {
            return false;
        }

        // 比较偏置
        if (layer1->bias && layer2->bias) {
            if (layer1->bias->size != layer2->bias->size) return false;

            float* b1 = (float*)layer1->bias->data;
            float* b2 = (float*)layer2->bias->data;

            for (size_t j = 0; j < layer1->bias->size; j++) {
                if (fabsf(b1[j] - b2[j]) > tolerance) return false;
            }
        }
    }

    return true;
}
