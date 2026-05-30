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

#endif // BPTT_LEARNER_H
