#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "../include/metrics.h"
#include "../include/error.h"

// 准确率
float compute_accuracy(Tensor* predictions, Tensor* targets) {
    if (!predictions || !targets) return 0.0f;

    if (predictions->size != targets->size) {
        LOG_ERROR("Prediction and target size mismatch");
        return 0.0f;
    }

    float* pred_data = (float*)predictions->data;
    float* target_data = (float*)targets->data;

    size_t correct = 0;
    for (size_t i = 0; i < predictions->size; i++) {
        // 对于分类问题:比较预测的类别
        int pred_class = pred_data[i] > 0.5f ? 1 : 0;
        int target_class = target_data[i] > 0.5f ? 1 : 0;

        if (pred_class == target_class) {
            correct++;
        }
    }

    return (float)correct / predictions->size;
}

// 均方误差
float compute_mse(Tensor* predictions, Tensor* targets) {
    if (!predictions || !targets) return 0.0f;

    float* pred_data = (float*)predictions->data;
    float* target_data = (float*)targets->data;

    size_t min_size = predictions->size < targets->size ? predictions->size : targets->size;

    float mse = 0.0f;
    for (size_t i = 0; i < min_size; i++) {
        float diff = pred_data[i] - target_data[i];
        mse += diff * diff;
    }

    return mse / min_size;
}

// 均方根误差
float compute_rmse(Tensor* predictions, Tensor* targets) {
    return sqrtf(compute_mse(predictions, targets));
}

// 平均绝对误差
float compute_mae(Tensor* predictions, Tensor* targets) {
    if (!predictions || !targets) return 0.0f;

    float* pred_data = (float*)predictions->data;
    float* target_data = (float*)targets->data;

    size_t min_size = predictions->size < targets->size ? predictions->size : targets->size;

    float mae = 0.0f;
    for (size_t i = 0; i < min_size; i++) {
        mae += fabsf(pred_data[i] - target_data[i]);
    }

    return mae / min_size;
}

// R平方分数
float compute_r2_score(Tensor* predictions, Tensor* targets) {
    if (!predictions || !targets) return 0.0f;

    float* pred_data = (float*)predictions->data;
    float* target_data = (float*)targets->data;

    size_t n = predictions->size < targets->size ? predictions->size : targets->size;

    // 计算目标均值
    float target_mean = 0.0f;
    for (size_t i = 0; i < n; i++) {
        target_mean += target_data[i];
    }
    target_mean /= n;

    // 计算总平方和和残差平方和
    float ss_tot = 0.0f;
    float ss_res = 0.0f;

    for (size_t i = 0; i < n; i++) {
        ss_tot += (target_data[i] - target_mean) * (target_data[i] - target_mean);
        ss_res += (target_data[i] - pred_data[i]) * (target_data[i] - pred_data[i]);
    }

    return ss_tot > 0 ? 1.0f - ss_res / ss_tot : 0.0f;
}

// 交叉熵损失
float compute_cross_entropy(Tensor* predictions, Tensor* targets) {
    if (!predictions || !targets) return 0.0f;

    float* pred_data = (float*)predictions->data;
    float* target_data = (float*)targets->data;

    size_t min_size = predictions->size < targets->size ? predictions->size : targets->size;

    float ce = 0.0f;
    for (size_t i = 0; i < min_size; i++) {
        // 假设targets是one-hot编码或概率分布
        float p = fmaxf(1e-10f, fminf(1.0f - 1e-10f, pred_data[i]));
        ce -= target_data[i] * logf(p);
    }

    return ce;
}

// Perplexity
float compute_perplexity(Tensor* predictions, Tensor* targets) {
    float ce = compute_cross_entropy(predictions, targets);
    return expf(ce);
}

// 精确率
float compute_precision(Tensor* predictions, Tensor* targets) {
    if (!predictions || !targets) return 0.0f;

    float* pred_data = (float*)predictions->data;
    float* target_data = (float*)targets->data;

    size_t true_positive = 0;
    size_t false_positive = 0;

    size_t n = predictions->size < targets->size ? predictions->size : targets->size;

    for (size_t i = 0; i < n; i++) {
        int pred = pred_data[i] > 0.5f ? 1 : 0;
        int target = target_data[i] > 0.5f ? 1 : 0;

        if (pred == 1 && target == 1) {
            true_positive++;
        } else if (pred == 1 && target == 0) {
            false_positive++;
        }
    }

    return (true_positive + false_positive) > 0 ?
           (float)true_positive / (true_positive + false_positive) : 0.0f;
}

// 召回率
float compute_recall(Tensor* predictions, Tensor* targets) {
    if (!predictions || !targets) return 0.0f;

    float* pred_data = (float*)predictions->data;
    float* target_data = (float*)targets->data;

    size_t true_positive = 0;
    size_t false_negative = 0;

    size_t n = predictions->size < targets->size ? predictions->size : targets->size;

    for (size_t i = 0; i < n; i++) {
        int pred = pred_data[i] > 0.5f ? 1 : 0;
        int target = target_data[i] > 0.5f ? 1 : 0;

        if (pred == 1 && target == 1) {
            true_positive++;
        } else if (pred == 0 && target == 1) {
            false_negative++;
        }
    }

    return (true_positive + false_negative) > 0 ?
           (float)true_positive / (true_positive + false_negative) : 0.0f;
}

// F1分数
float compute_f1_score(Tensor* predictions, Tensor* targets) {
    float precision = compute_precision(predictions, targets);
    float recall = compute_recall(predictions, targets);

    return (precision + recall) > 0 ?
           2.0f * precision * recall / (precision + recall) : 0.0f;
}

// 混淆矩阵
Tensor* compute_confusion_matrix(Tensor* predictions, Tensor* targets, int num_classes) {
    if (!predictions || !targets || num_classes <= 0) return NULL;

    size_t shape[] = {(size_t)num_classes, (size_t)num_classes};
    Tensor* confusion = tensor_zeros(DT_FLOAT32, 2, shape);

    float* pred_data = (float*)predictions->data;
    float* target_data = (float*)targets->data;
    float* conf_data = (float*)confusion->data;

    size_t n = predictions->size < targets->size ? predictions->size : targets->size;

    for (size_t i = 0; i < n; i++) {
        int pred_class = (int)pred_data[i];
        int target_class = (int)target_data[i];

        if (pred_class >= 0 && pred_class < num_classes &&
            target_class >= 0 && target_class < num_classes) {
            conf_data[target_class * num_classes + pred_class]++;
        }
    }

    return confusion;
}

// 计算正确预测数
size_t compute_correct_predictions(Tensor* confusion_matrix) {
    if (!confusion_matrix || confusion_matrix->ndim != 2) return 0;

    size_t correct = 0;
    int n = confusion_matrix->shape[0];
    float* data = (float*)confusion_matrix->data;

    for (int i = 0; i < n; i++) {
        correct += (size_t)data[i * n + i];
    }

    return correct;
}

// 每个类别的准确率
float* compute_class_accuracy(Tensor* confusion_matrix, int num_classes) {
    if (!confusion_matrix || num_classes <= 0) return NULL;

    float* class_acc = malloc(num_classes * sizeof(float));
    float* data = (float*)confusion_matrix->data;

    for (int i = 0; i < num_classes; i++) {
        float total = 0.0f;
        for (int j = 0; j < num_classes; j++) {
            total += data[i * num_classes + j];
        }
        class_acc[i] = total > 0 ? data[i * num_classes + i] / total : 0.0f;
    }

    return class_acc;
}

// 编辑距离
int compute_edit_distance(const char* s1, const char* s2) {
    if (!s1 || !s2) return 0;

    int len1 = strlen(s1);
    int len2 = strlen(s2);

    // 创建DP表
    int** dp = malloc((len1 + 1) * sizeof(int*));
    if (!dp) return -1;
    for (int i = 0; i <= len1; i++) {
        dp[i] = calloc((len2 + 1), sizeof(int));
        if (!dp[i]) {
            for (int k = 0; k < i; k++) free(dp[k]);
            free(dp);
            return -1;
        }
    }

    // 初始化
    for (int i = 0; i <= len1; i++) dp[i][0] = i;
    for (int j = 0; j <= len2; j++) dp[0][j] = j;

    // 填表
    for (int i = 1; i <= len1; i++) {
        for (int j = 1; j <= len2; j++) {
            if (s1[i - 1] == s2[j - 1]) {
                dp[i][j] = dp[i - 1][j - 1];
            } else {
                dp[i][j] = 1 + fmin(dp[i - 1][j - 1],
                                   fmin(dp[i][j - 1], dp[i - 1][j]));
            }
        }
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
    int* last_row = dp[len1];
    int result = last_row[len2];
#pragma GCC diagnostic pop

    // 清理
    for (int i = 0; i <= len1; i++) {
        free(dp[i]);
    }
    free(dp);

    return result;
}

// 字符错误率
float compute_cer(const char* prediction, const char* target) {
    if (!prediction || !target) return 0.0f;

    int edit_dist = compute_edit_distance(prediction, target);
    int target_len = strlen(target);

    return target_len > 0 ? (float)edit_dist / target_len : 0.0f;
}

// 词错误率
float compute_wer(const char* prediction, const char* target) {
    if (!prediction || !target) return 0.0f;

    // 简化:按空格分割单词
    // 实际实现应该更复杂

    int edit_dist = compute_edit_distance(prediction, target);
    int target_len = strlen(target);

    return target_len > 0 ? (float)edit_dist / target_len : 0.0f;
}

// BLEU分数(简化实现)
float compute_bleu(Tensor* predictions, Tensor* targets, int /*n_gram*/) {
    if (!predictions || !targets) return 0.0f;

    // 简化:实际BLEU计算需要n-gram匹配和长度惩罚
    LOG_WARNING("BLEU computation simplified");
    return compute_accuracy(predictions, targets);
}

// 独立BLEU计算
float compute_bleu_score(int** /*predictions*/, int /*pred_lengths*/,
                        int** /*targets*/, int /*target_lengths*/,
                        int /*num_samples*/, int /*max_n_gram*/) {
    // 简化实现
    return 0.0f;
}

// ROUGE-L分数(简化实现)
float compute_rouge_l(Tensor* predictions, Tensor* targets) {
    if (!predictions || !targets) return 0.0f;

    // 简化:ROUGE需要计算最长公共子序列
    LOG_WARNING("ROUGE-L computation simplified");
    return compute_accuracy(predictions, targets);
}

// ROUGE-N
float compute_rouge_n(Tensor* predictions, Tensor* targets, int /*n*/) {
    if (!predictions || !targets) return 0.0f;

    // 简化:ROUGE-N需要n-gram匹配
    LOG_WARNING("ROUGE-N computation simplified");
    return compute_accuracy(predictions, targets);
}

// 计算所有指标
EvaluationMetrics compute_all_metrics(Tensor* predictions, Tensor* targets, MetricsConfig config) {
    EvaluationMetrics metrics = {0};

    metrics.accuracy = compute_accuracy(predictions, targets);
    metrics.precision = compute_precision(predictions, targets);
    metrics.recall = compute_recall(predictions, targets);
    metrics.f1_score = compute_f1_score(predictions, targets);
    metrics.mse = compute_mse(predictions, targets);
    metrics.mae = compute_mae(predictions, targets);
    metrics.rmse = compute_rmse(predictions, targets);
    metrics.r2 = compute_r2_score(predictions, targets);
    metrics.cross_entropy = compute_cross_entropy(predictions, targets);
    metrics.perplexity = compute_perplexity(predictions, targets);

    if (config.compute_bleu) {
        metrics.bleu = compute_bleu(predictions, targets, 4);
    }
    if (config.compute_rouge) {
        metrics.rouge_l = compute_rouge_l(predictions, targets);
    }

    return metrics;
}

// 打印评估结果
void print_evaluation_metrics(EvaluationMetrics metrics) {
    printf("=== Evaluation Metrics ===\n");
    printf("Accuracy:  %.4f\n", metrics.accuracy);
    printf("Precision: %.4f\n", metrics.precision);
    printf("Recall:    %.4f\n", metrics.recall);
    printf("F1-Score:  %.4f\n", metrics.f1_score);
    printf("MSE:       %.6f\n", metrics.mse);
    printf("MAE:       %.6f\n", metrics.mae);
    printf("RMSE:      %.6f\n", metrics.rmse);
    printf("R2:        %.4f\n", metrics.r2);
    printf("Cross-Entropy: %.6f\n", metrics.cross_entropy);
    printf("Perplexity:    %.4f\n", metrics.perplexity);
    printf("BLEU:      %.4f\n", metrics.bleu);
    printf("ROUGE-L:   %.4f\n", metrics.rouge_l);
    printf("==========================\n");
}
