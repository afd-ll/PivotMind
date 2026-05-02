#ifndef METRICS_H
#define METRICS_H

#include "tensor.h"
#include <stdbool.h>

// BLEU分数计算
float compute_bleu(Tensor* predictions, Tensor* targets, int n_gram);

// 独立BLEU计算
float compute_bleu_score(int** predictions, int pred_lengths,
                        int** targets, int target_lengths,
                        int num_samples, int max_n_gram);

// ROUGE分数计算
float compute_rouge_l(Tensor* predictions, Tensor* targets);

// ROUGE-N计算
float compute_rouge_n(Tensor* predictions, Tensor* targets, int n);

// 准确率
float compute_accuracy(Tensor* predictions, Tensor* targets);

// 精确率(Precision)
float compute_precision(Tensor* predictions, Tensor* targets);

// 召回率(Recall)
float compute_recall(Tensor* predictions, Tensor* targets);

// F1分数
float compute_f1_score(Tensor* predictions, Tensor* targets);

// 平均绝对误差(MAE)
float compute_mae(Tensor* predictions, Tensor* targets);

// 均方误差(MSE)
float compute_mse(Tensor* predictions, Tensor* targets);

// 均方根误差(RMSE)
float compute_rmse(Tensor* predictions, Tensor* targets);

// R平方分数(R2)
float compute_r2_score(Tensor* predictions, Tensor* targets);

// 交叉熵损失
float compute_cross_entropy(Tensor* predictions, Tensor* targets);

// 混淆矩阵
Tensor* compute_confusion_matrix(Tensor* predictions, Tensor* targets, int num_classes);

// 计算混淆矩阵的对角线(正确预测数)
size_t compute_correct_predictions(Tensor* confusion_matrix);

// 每个类别的准确率
float* compute_class_accuracy(Tensor* confusion_matrix, int num_classes);

// Perplexity(困惑度)
float compute_perplexity(Tensor* predictions, Tensor* targets);

// 编辑距离
int compute_edit_distance(const char* s1, const char* s2);

// 字符错误率(CER)
float compute_cer(const char* prediction, const char* target);

// 词错误率(WER)
float compute_wer(const char* prediction, const char* target);

// 评估指标配置
typedef struct {
    bool compute_accuracy;
    bool compute_f1;
    bool compute_mse;
    bool compute_bleu;
    bool compute_rouge;
} MetricsConfig;

// 综合评估
typedef struct {
    float accuracy;
    float precision;
    float recall;
    float f1_score;
    float mse;
    float mae;
    float rmse;
    float r2;
    float bleu;
    float rouge_l;
    float cross_entropy;
    float perplexity;
} EvaluationMetrics;

// 计算所有指标
EvaluationMetrics compute_all_metrics(Tensor* predictions, Tensor* targets, MetricsConfig config);

// 打印评估结果
void print_evaluation_metrics(EvaluationMetrics metrics);

#endif // METRICS_H
