#include "../include/optimizer.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

// 创建SGD优化器
Optimizer* optimizer_create_sgd(float learning_rate, float momentum) {
    Optimizer* optimizer = malloc(sizeof(Optimizer));
    if (!optimizer) return NULL;

    SGDOptimizer* sgd = malloc(sizeof(SGDOptimizer));
    if (!sgd) {
        free(optimizer);
        return NULL;
    }

    sgd->learning_rate = learning_rate;
    sgd->momentum = momentum;
    sgd->velocity = NULL;
    sgd->num_params = 0;

    optimizer->type = OPTIMIZER_SGD;
    optimizer->optimizer = sgd;

    return optimizer;
}

// 创建Adam优化器
Optimizer* optimizer_create_adam(float learning_rate, float beta1, float beta2, float epsilon) {
    Optimizer* optimizer = malloc(sizeof(Optimizer));
    if (!optimizer) return NULL;

    AdamOptimizer* adam = malloc(sizeof(AdamOptimizer));
    if (!adam) {
        free(optimizer);
        return NULL;
    }

    adam->learning_rate = learning_rate;
    adam->beta1 = beta1;
    adam->beta2 = beta2;
    adam->epsilon = epsilon;
    adam->t = 0;
    adam->m = NULL;
    adam->v = NULL;
    adam->num_params = 0;

    optimizer->type = OPTIMIZER_ADAM;
    optimizer->optimizer = adam;

    return optimizer;
}

// SGD优化步骤
void optimizer_step_sgd(SGDOptimizer* sgd, Tensor** params, size_t num_params) {
    // 初始化速度数组
    if (sgd->num_params != num_params) {
        if (sgd->velocity) {
            for (size_t i = 0; i < sgd->num_params; i++) {
                if (sgd->velocity[i]) {
                    tensor_destroy(sgd->velocity[i]);
                }
            }
            free(sgd->velocity);
        }

        sgd->velocity = malloc(num_params * sizeof(Tensor*));
        for (size_t i = 0; i < num_params; i++) {
            size_t* shape = malloc(params[i]->ndim * sizeof(size_t));
            memcpy(shape, params[i]->shape, params[i]->ndim * sizeof(size_t));
            sgd->velocity[i] = tensor_zeros(params[i]->dtype, params[i]->ndim, shape);
            free(shape);
        }
        sgd->num_params = num_params;
    }

    // 更新参数
    for (size_t i = 0; i < num_params; i++) {
        Tensor* param = params[i];
        Tensor* grad = (Tensor*)param->grad;

        if (!grad) continue;

        float* param_data = (float*)param->data;
        float* grad_data = (float*)grad->data;
        float* vel_data = (float*)sgd->velocity[i]->data;

        if (sgd->momentum > 0.0f) {
            // 带动量的SGD
            for (size_t j = 0; j < param->size; j++) {
                vel_data[j] = sgd->momentum * vel_data[j] + sgd->learning_rate * grad_data[j];
                param_data[j] -= vel_data[j];
            }
        } else {
            // 标准SGD
            for (size_t j = 0; j < param->size; j++) {
                param_data[j] -= sgd->learning_rate * grad_data[j];
            }
        }

        // 清零梯度
        tensor_zero_grad(param);
    }
}

// Adam优化步骤
void optimizer_step_adam(AdamOptimizer* adam, Tensor** params, size_t num_params) {
    // 初始化一阶和二阶矩估计
    if (adam->num_params != num_params) {
        if (adam->m) {
            for (size_t i = 0; i < adam->num_params; i++) {
                if (adam->m[i]) tensor_destroy(adam->m[i]);
                if (adam->v[i]) tensor_destroy(adam->v[i]);
            }
            free(adam->m);
            free(adam->v);
        }

        adam->m = malloc(num_params * sizeof(Tensor*));
        adam->v = malloc(num_params * sizeof(Tensor*));

        for (size_t i = 0; i < num_params; i++) {
            size_t* shape = malloc(params[i]->ndim * sizeof(size_t));
            memcpy(shape, params[i]->shape, params[i]->ndim * sizeof(size_t));
            adam->m[i] = tensor_zeros(params[i]->dtype, params[i]->ndim, shape);
            adam->v[i] = tensor_zeros(params[i]->dtype, params[i]->ndim, shape);
            free(shape);
        }
        adam->num_params = num_params;
    }

    adam->t++;

    // 计算偏差修正后的矩估计
    float beta1_pow = powf(adam->beta1, adam->t);
    float beta2_pow = powf(adam->beta2, adam->t);
    float alpha = adam->learning_rate * sqrtf(1.0f - beta2_pow) / (1.0f - beta1_pow + 1e-8f);

    // 更新参数
    for (size_t i = 0; i < num_params; i++) {
        Tensor* param = params[i];
        Tensor* grad = (Tensor*)param->grad;

        if (!grad) continue;

        float* param_data = (float*)param->data;
        float* grad_data = (float*)grad->data;
        float* m_data = (float*)adam->m[i]->data;
        float* v_data = (float*)adam->v[i]->data;

        for (size_t j = 0; j < param->size; j++) {
            // 更新一阶矩估计
            m_data[j] = adam->beta1 * m_data[j] + (1.0f - adam->beta1) * grad_data[j];
            // 更新二阶矩估计
            v_data[j] = adam->beta2 * v_data[j] + (1.0f - adam->beta2) * grad_data[j] * grad_data[j];
            // 计算偏差修正后的估计
            float m_hat = m_data[j] / (1.0f - beta1_pow + 1e-8f);
            float v_hat = v_data[j] / (1.0f - beta2_pow + 1e-8f);
            // 更新参数
            param_data[j] -= alpha * m_hat / (sqrtf(v_hat) + adam->epsilon);
        }

        // 清零梯度
        tensor_zero_grad(param);
    }
}

// 优化步骤
void optimizer_step(Optimizer* optimizer, Tensor** params, size_t num_params) {
    if (!optimizer || !optimizer->optimizer || !params) return;

    switch (optimizer->type) {
        case OPTIMIZER_SGD:
            optimizer_step_sgd((SGDOptimizer*)optimizer->optimizer, params, num_params);
            break;
        case OPTIMIZER_MOMENTUM:
            optimizer_step_sgd((SGDOptimizer*)optimizer->optimizer, params, num_params);
            break;
        case OPTIMIZER_ADAM:
            optimizer_step_adam((AdamOptimizer*)optimizer->optimizer, params, num_params);
            break;
        default:
            break;
    }
}

// 销毁优化器
void optimizer_destroy(Optimizer* optimizer) {
    if (!optimizer) return;

    switch (optimizer->type) {
        case OPTIMIZER_SGD:
        case OPTIMIZER_MOMENTUM: {
            SGDOptimizer* sgd = (SGDOptimizer*)optimizer->optimizer;
            if (sgd->velocity) {
                for (size_t i = 0; i < sgd->num_params; i++) {
                    if (sgd->velocity[i]) {
                        tensor_destroy(sgd->velocity[i]);
                    }
                }
                free(sgd->velocity);
            }
            free(sgd);
            break;
        }
        case OPTIMIZER_ADAM: {
            AdamOptimizer* adam = (AdamOptimizer*)optimizer->optimizer;
            if (adam->m) {
                for (size_t i = 0; i < adam->num_params; i++) {
                    if (adam->m[i]) tensor_destroy(adam->m[i]);
                    if (adam->v[i]) tensor_destroy(adam->v[i]);
                }
                free(adam->m);
                free(adam->v);
            }
            free(adam);
            break;
        }
        default:
            break;
    }

    free(optimizer);
}
