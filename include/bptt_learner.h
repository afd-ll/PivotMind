/**
 * @file bptt_learner.h
 * @brief BPTT 在线学习器 — RNN 反向传播接入对话管线
 *
 * 设计：
 * - 输入：用户对话中每个字的拓扑节点特征向量 (NODE_FEATURE_DIM=512)
 * - 目标：回复中每个字的特征向量
 * - 模型：SimpleRNN(512→256) → Linear(256→512)
 * - 损失：MSE
 * - 优化器：Adam (lr=0.001)
 *
 * 与拓扑学习的关系：
 * - 自主学习器 (autonomic_learner)：管理拓扑边的置信度
 * - BPTT 学习器 (bptt_learner)：训练神经网络权重
 * - 两者互补，同时运行
 */

#ifndef BPTT_LEARNER_H
#define BPTT_LEARNER_H

#include "model.h"
#include "optimizer.h"
#include "multi_topology.h"

typedef struct {
    Model* model;              // RNN 模型
    Optimizer* optimizer;      // Adam 优化器
    MasterTopology* master;    // 用于特征向量查找

    int input_dim;             // NODE_FEATURE_DIM
    int hidden_dim;            // RNN 隐藏层大小
    int max_seq;               // 最大序列长度

    // 工作缓冲（复用，避免反复分配）
    float* feat_buf_in;        // [max_seq * input_dim]
    float* feat_buf_tgt;       // [max_seq * input_dim]

    // 统计
    float total_loss;
    int steps;
} BpttLearner;

/**
 * 创建 BPTT 学习器
 * @param master      主拓扑（用于节点特征查询）
 * @param hidden_dim  RNN 隐藏层大小（建议 64）
 * @param max_seq     最大序列长度（建议 32）
 */
BpttLearner* bptt_learner_create(MasterTopology* master,
                                 int hidden_dim, int max_seq);

/**
 * 销毁 BPTT 学习器
 */
void bptt_learner_destroy(BpttLearner* bl);

/**
 * 从一次对话中学习
 *
 * 将 user_input 和 ai_response 分别映射为节点特征序列，
 * 训练 RNN 从输入特征序列预测回复特征序列。
 *
 * @param bl          BPTT 学习器
 * @param user_input  用户输入文本
 * @param ai_response AI 回复文本
 * @return            本轮损失，<0 表示跳过
 */
float bptt_learn_from_dialog(BpttLearner* bl,
                             const char* user_input,
                             const char* ai_response);

/**
 * 获取训练统计
 */
void bptt_learner_stats(BpttLearner* bl, float* out_avg_loss, int* out_steps);

/* ── Phase 3: BPTT 辅助生成 — RNN 前向偏置词汇拓扑激活 ── */

/**
 * 使用训练好的 RNN 模型对输入文本做前向推理，
 * 将 RNN 预测输出的特征向量与词汇拓扑节点做余弦相似度匹配，
 * 给高相似度节点增加激活偏置。
 *
 * 这使 BPTT 的神经网络学习成果能参与生成管线：
 * RNN "猜测"什么词汇可能出现在回复中 → 预激活这些词汇节点 →
 * 后续拓扑走边时这些节点有更高概率被选中。
 *
 * @param bl         BPTT 学习器（需已训练若干步）
 * @param input_text 当前用户输入文本
 * @return 被偏置的节点数，<=0 表示未生效（数据不足或模型未训练）
 */
int bptt_bias_vocab_activation(BpttLearner* bl, const char* input_text);

/**
 * 获取 BPTT 模型的置信度（基于最近训练的 loss 归一化）
 * @return 0.0~1.0，未训练时返回 0
 */
float bptt_get_confidence(BpttLearner* bl);

#endif // BPTT_LEARNER_H
