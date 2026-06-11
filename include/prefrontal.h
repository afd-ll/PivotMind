/**
 * @file prefrontal.h
 * @brief 前额叶皮层 — 生成皮层(背外侧) + 监控皮层(前扣带回)
 *
 * 双组件架构：
 *   DLPFC 生成 — 前额叶背外侧：多层扩散生成候选序列
 *   ACC 监控 — 前扣带回：自我评分+门控决策（PASS/BACKTRACK/REWRITE）
 */

#ifndef PREFRONTAL_H
#define PREFRONTAL_H

#include "multi_topology.h"
#include "memory_system.h"
#include "dialog_system.h"
#include "cognitive_controller.h"
#include "cingulate.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Prefrontal {
    DialogSystem*        dialog;
    CognitiveController* controller;
    MasterTopology*      topology;
    MemorySystem*        memory;

    /* ACC 门控参数 */
    float accept_threshold;     /* 通过阈值 (默认 0.55) */
    int   max_retries;          /* 最大回溯次数 (默认 3) */
    int   total_retries;        /* 累计回溯统计 */
} Prefrontal;

Prefrontal* prefrontal_create(MasterTopology* topology,
                               MemorySystem* memory,
                               CausalGraph* causal_graph,
                               ActiveLearner* learner);
void prefrontal_destroy(Prefrontal* pf);

/**
 * 对话处理 — 生成+自我监控+回溯
 * 流程：DLPFC生成 → ACC评分 → PASS/回溯/重写
 */
char* prefrontal_chat(Prefrontal* pf, const char* input);

void prefrontal_feedback(Prefrontal* pf, const char* input,
                          const char* response, const char* rating);

static inline DialogSystem* prefrontal_dialog(Prefrontal* pf) {
    return pf ? pf->dialog : NULL;
}
static inline CognitiveState* prefrontal_cognitive_state(Prefrontal* pf) {
    if (!pf || !pf->dialog) return NULL;
    return pf->dialog->cognitive_state;
}

#ifdef __cplusplus
}
#endif

#endif
