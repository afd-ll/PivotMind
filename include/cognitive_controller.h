/**
 * @file cognitive_controller.h
 * @brief 认知调度中心 — 位于记忆系统和子拓扑之间
 *
 * 核心职责：
 * 1. 根据记忆状态计算意图向量（intent_weights）
 * 2. 调度各子拓扑的搜索偏好
 * 3. 评估生成草案的内感受评分
 * 4. 负反馈修正：不满意就调整再试
 */

#ifndef COGNITIVE_CONTROLLER_H
#define COGNITIVE_CONTROLLER_H

#include "multi_topology.h"
#include "memory_system.h"
#include <stdbool.h>

// ==================== 常量 ====================

/** 子拓扑数量（匹配 TopologyType 枚举 0-9） */
#define MAX_SUBTOPOS 10

/** 束搜索候选路径池大小 */
#define PATH_POOL_SIZE 10

/** 路径最大长度 */
#define MAX_PATH_LENGTH 32

/** 最大修正次数 */
#define MAX_RETRY 3

/** 修正状态返回值 */
typedef enum {
    RETRY_OK          = 0,   // 通过，无需修正
    RETRY_FROM_POOL   = 1,   // 从候选池重排（不重搜）
    RETRY_WITH_SEARCH = 2,   // 缩域重搜（需重新 dialog_reason）
    RETRY_FAILED      = -1   // 已达上限或无解，强制输出
} RetryStatus;

// ==================== 路径结构 ====================

/**
 * 单条路径：节点序列 + 综合评分
 */
typedef struct {
    int node_ids[MAX_PATH_LENGTH];     // 节点ID序列
    int topo_id;                       // 所属子拓扑ID
    int length;                        // 实际长度
    float score;                       // 综合评分
    float act_sum;                     // 累计激活值
    float conf_sum;                    // 累计置信度
} PathResult;

// ==================== 认知调度中心 ====================

/**
 * 认知调度中心结构体
 *
 * 运行在主循环中，位于记忆系统和子拓扑之间。
 * 每次对话回合，根据当前记忆状态计算意图向量，
 * 指导各子拓扑的搜索方向，并对产出草案进行内感受评估。
 */
typedef struct {
    // ========== 1. 意图向量 ==========
    float intent_weights[MAX_SUBTOPOS];  // 喂给每个子拓扑的偏好系数

    // ========== 2. 调度策略偏置 ==========
    float context_bias;      // 上下文记忆给出的偏向强度 (0.0-1.0)
    float novelty_bias;      // 短时记忆给出的求新强度 (0.0-1.0)
    float valence_bias;      // 效价(用户反馈)的整体调节强度 (0.0-1.0)
    float coherence_target;  // 语义连贯性目标 (0.0-1.0)

    // ========== 3. 负反馈调节状态 ==========
    float satisfaction_threshold;  // 多高的内感受评分才算通过 (0.0-1.0)
    int   max_retry;               // 最多修正几次
    float correction_strength;     // 每次修正的力度 (0.0-1.0)
    int   retry_count;             // 当前回合己修正次数

    // ========== 4. 上次决策的快照 ==========
    float prev_intent_weights[MAX_SUBTOPOS];  // 上一轮意图向量
    float prev_satisfaction;                   // 上一轮满意度

    // ========== 5. 候选路径池（用于不重搜修正） ==========
    PathResult path_pool[MAX_SUBTOPOS][PATH_POOL_SIZE];  // 每个子拓扑的候选池
    int pool_counts[MAX_SUBTOPOS];                        // 各池当前大小

    // ========== 6. 外部分量（由主流程注入） ==========
    MasterTopology* master;
    MemorySystem*   memory;
    const char*     current_input;     // 当前用户输入（仅引用，不拥有）
    const char*     last_response;     // 上一轮回复（仅引用，不拥有）

} CognitiveController;

// ==================== API函数 ====================

/**
 * 创建认知调度中心
 */
CognitiveController* cognitive_controller_create(MasterTopology* master,
                                                  MemorySystem* memory);

/**
 * 销毁认知调度中心
 */
void cognitive_controller_destroy(CognitiveController* cc);

/**
 * 计算意图向量 —— 调度中心的核心
 *
 * 根据当前记忆状态和历史快照，为每个子拓扑计算偏好权重。
 * 权重 = 上下文相关性 × 新颖性 × 效价偏好 × 连贯性要求
 *
 * @param cc        认知调度中心
 * @param ctx_activations 各个子拓扑的当前上下文激活度（从记忆系统获取）
 */
void compute_intent(CognitiveController* cc, const float* ctx_activations);

/**
 * 内感受评估 —— 检查生成的草案是否满意
 *
 * 使用情绪子拓扑和语义子拓扑评估草案质量。
 *
 * @param cc        认知调度中心
 * @param draft     当前生成的草案路径
 * @param draft_len 草案长度
 * @return 满意度评分 (0.0-1.0)
 */
float evaluate_draft(CognitiveController* cc,
                     const PathResult* draft,
                     int draft_len);

/**
 * 计算修正向量 —— 不满意时生成修正方向
 *
 * @param cc        认知调度中心
 * @param draft     当前草案
 * @param satisfaction 满意度评分
 * @param correction   输出修正向量（长度 MAX_SUBTOPOS）
 */
void compute_correction_vector(CognitiveController* cc,
                               const PathResult* draft,
                               float satisfaction,
                               float* correction);

/**
 * 负反馈修正 —— 不满意时调整意图权重并重新调度
 *
 * @param cc        认知调度中心
 * @param draft     当前草案
 * @param satisfaction 满意度评分
 * @return RetryStatus: RETRY_OK/FROM_POOL/WITH_SEARCH/FAILED
 */
RetryStatus revise_and_retry(CognitiveController* cc,
                             const PathResult* draft,
                             float satisfaction);

/**
 * 保存路径到候选池（用于第1次修正不重搜）
 */
void pool_save_path(CognitiveController* cc, int topo_idx,
                    const PathResult* path);

/**
 * 从候选池中按新权重选出最佳路径
 */
int pool_select_best(CognitiveController* cc, int topo_idx,
                     PathResult* out);

/**
 * 重置修正状态（每轮对话开始前调用）
 */
void cognitive_controller_reset_round(CognitiveController* cc);

/**
 * 保存本轮决策快照（供下轮使用）
 */
void cognitive_controller_snapshot(CognitiveController* cc, float satisfaction);

/**
 * 设置当前用户输入和上一轮回复
 */
void cognitive_controller_set_context(CognitiveController* cc,
                                       const char* input,
                                       const char* last_response);

/**
 * 获取子拓扑名称（用于调试日志）
 */
const char* cognitive_controller_topo_name(int topo_type);

#endif // COGNITIVE_CONTROLLER_H
