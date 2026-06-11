/**
 * @file prefrontal.h
 * @brief 前额叶皮层 — 对话决策 + 意图规划 + 认知控制
 *
 * 大脑类比：
 *   前额叶是大脑"CEO"，负责高阶认知——语言理解、意图推断、行为规划、
 *   错误修正、注意力调控。它整合来自海马体的记忆、丘脑的调度信号、
 *   布罗卡区的语言产出能力。
 *
 * 系统映射：
 *   prefrontal_chat()       — 完整的对话处理流水线
 *   prefrontal_feedback()   — 反馈学习
 */

#ifndef PREFRONTAL_H
#define PREFRONTAL_H

#include "multi_topology.h"
#include "memory_system.h"
#include "dialog_system.h"
#include "cognitive_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Prefrontal {
    DialogSystem*        dialog;
    CognitiveController* controller;
    MasterTopology*      topology;
    MemorySystem*        memory;
} Prefrontal;

/**
 * 创建前额叶（包装 dialog_system + cognitive_controller）
 */
Prefrontal* prefrontal_create(MasterTopology* topology,
                               MemorySystem* memory,
                               CausalGraph* causal_graph,
                               ActiveLearner* learner);

void prefrontal_destroy(Prefrontal* pf);

/**
 * 对话处理：输入 → 意图推断 → 认知调度 → 推理 → 语言输出
 * @return 回复字符串（调用者负责 free）
 */
char* prefrontal_chat(Prefrontal* pf, const char* input);

/**
 * 反馈处理：用户对回复的评价
 */
void prefrontal_feedback(Prefrontal* pf, const char* input, const char* response,
                          const char* rating);

/** 获取内部 dialog system（用于兼容旧代码） */
static inline DialogSystem* prefrontal_dialog(Prefrontal* pf) {
    return pf ? pf->dialog : NULL;
}

/** 获取认知状态（用于脑干漂移） */
static inline CognitiveState* prefrontal_cognitive_state(Prefrontal* pf) {
    if (!pf || !pf->dialog) return NULL;
    return pf->dialog->cognitive_state;
}

#ifdef __cplusplus
}
#endif

#endif
