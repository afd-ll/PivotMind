#ifndef COGNITIVE_PARAMS_H
#define COGNITIVE_PARAMS_H

#include <stdbool.h>
#include <time.h>

// ==================== 认知架构参数系统 ====================
// 基于效价、动机、情感的新一代AI参数体系

// ==================== 效价 (Valence) ====================
#define VALENCE_MIN -1.0f
#define VALENCE_MAX  1.0f
#define VALENCE_NEUTRAL 0.0f

// ==================== 置信度 (Confidence) - 三维定义 ====================
typedef struct {
    float predictive_accuracy;  // 认知层：预测能力 (40%)
    float user_satisfaction;    // 情感层：用户满意度 (30%)
    float novelty_bonus;        // 探索层：新异性奖励 (30%)
    float combined;             // 综合置信度
} CognitiveConfidence;

// ==================== 边权重 - 双重定义 ====================
typedef struct {
    float logical_strength;      // 逻辑强度：客观连接强度
    float motivational_bias;    // 动机倾向：愿意使用这条边的程度
    float combined;             // 综合权重
} EdgeWeightDual;

// ==================== 认知状态 ====================
typedef struct {
    // 需求 (Drive)
    float drive_curiosity;      // 好奇驱动：想探索新事物
    float drive_hunger;        // 饥饿驱动：想获取资源
    float drive_social;        // 社交驱动：想互动
    float drive_comfort;       // 舒适驱动：想保持现状

    // 情感 (Emotion)
    float emotion_pleasure;     // 愉悦度
    float emotion_pain;        // 痛苦度
    float emotion_security;     // 安全感

    // 效价 (Valence) - 综合情感-动机值
    float valence;             // -1.0 ~ +1.0

    // 意图 (Intention)
    char* current_goal;       // 当前目标
    float goal_strength;       // 目标强度
    int plan_step;            // 计划步骤

    // 探索/利用平衡
    float explore_rate;        // 探索率 (0-1)
} CognitiveState;

// ==================== 交互记录 ====================
typedef struct {
    char* user_input;          // 用户输入
    char* system_response;    // 系统回复
    char* feedback;           // 用户反馈
    float response_time;       // 响应时间(秒)
    void* concept;            // 涉及的概念
    time_t timestamp;         // 时间戳
} Interaction;

// ==================== 置信度函数 ====================

/**
 * 创建置信度结构
 */
CognitiveConfidence* cognitive_confidence_create(void);

/**
 * 计算综合置信度
 * confidence = predictive_accuracy * 0.4 + user_satisfaction * 0.3 + novelty_bonus * 0.3
 */
void cognitive_confidence_compute(CognitiveConfidence* conf);

/**
 * 更新置信度的各个维度
 */
void cognitive_confidence_update(CognitiveConfidence* conf,
                                float predictive,
                                float satisfaction,
                                float novelty);

/**
 * 销毁置信度结构
 */
void cognitive_confidence_destroy(CognitiveConfidence* conf);

// ==================== 效价函数 ====================

/**
 * 从交互中自动计算效价
 * 不需要人工标注，从交互中自动计算
 */
float compute_valence_from_interaction(Interaction* interaction,
                                      float current_confidence,
                                      bool is_novel_concept);

/**
 * 获取用户反馈中的效价
 */
float get_feedback_valence(const char* feedback);

/**
 * 获取互动特性的效价
 */
float get_interaction_valence(float response_time, bool is_novel);

/**
 * 获取自我评估的效价
 */
float get_self_assessment_valence(float confidence, float prediction_error);

/**
 * 更新节点的效价（带时间衰减）
 */
void node_update_valence(float* current_valence, float new_valence, float learning_rate);

/**
 * 计算效价的时间衰减权重
 */
float calculate_recency_weight(time_t interaction_time);

// ==================== 边权重函数 ====================

/**
 * 创建双重边权重
 */
EdgeWeightDual* edge_weight_create(void);

/**
 * 计算综合边权重
 * combined = logical_strength * motivational_bias
 */
void edge_weight_compute(EdgeWeightDual* weight);

/**
 * 根据效价更新边权重
 * 积极体验 -> 更强连接
 * 消极体验 -> 较弱连接
 */
void edge_weight_update_from_valence(EdgeWeightDual* weight, float valence);

/**
 * 销毁边权重
 */
void edge_weight_destroy(EdgeWeightDual* weight);

// ==================== 认知状态函数 ====================

/**
 * 创建认知状态
 */
CognitiveState* cognitive_state_create(void);

/**
 * 初始化默认认知状态
 */
void cognitive_state_init(CognitiveState* state);

/**
 * 更新认知状态
 */
void cognitive_state_update(CognitiveState* state, Interaction* interaction, float outcome);

/**
 * 计算探索/利用平衡
 * 体验差 -> 多探索
 * 体验好 -> 多利用
 */
float compute_explore_exploit_balance(float recent_valence);

/**
 * 更新效价
 */
void cognitive_state_update_valence(CognitiveState* state, float new_valence);

/**
 * 销毁认知状态
 */
void cognitive_state_destroy(CognitiveState* state);

// ==================== 节点激活函数 ====================

/**
 * 带效价的节点激活
 */
void activate_node_with_valence(float* activation, float input_signal,
                                float logical_weight, float motivational_bias,
                                float node_valence);

/**
 * 带效价的边激活计算
 */
float compute_edge_activation_with_valence(float logical_strength,
                                          float motivational_bias,
                                          float valence);

// ==================== 工具函数 ====================

/**
 * 限制值在范围内
 */
float clamp_float(float value, float min_val, float max_val);

/**
 * 线性插值
 */
float lerp(float a, float b, float t);

#endif // COGNITIVE_PARAMS_H
