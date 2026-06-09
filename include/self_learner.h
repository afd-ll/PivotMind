/**
 * @file self_learner.h
 * @brief 后台自主学习引擎 — 好奇心驱动探索 + 知识审查 + 自纠错
 *
 * 与梦境引擎的区别：
 *   - 梦境：随机游走 + 弱边强化（被动联想）
 *   - 自主学习：好奇心驱动 + 深度探索 + 逻辑审查 + 自动修正（主动学习）
 *
 * 核心机制：
 *   1. 好奇心采样：选择低置信度/未探索的节点作为起点
 *   2. 深度游走：5-8跳的拓扑探索，记录完整路径
 *   3. 知识审查：
 *      - 传递性检测：A→B→C 但没有 A→C → 建议新建
 *      - 矛盾检测：A→B 且 A→非B → 降低两者权重
 *      - 缺失检测：语义相似但无连接 → 建议新建
 *   4. 自纠错：自动创建传递边、衰减矛盾边、创建语义边
 *   5. 探索记录：已探索区域标记，避免重复
 */

#ifndef SELF_LEARNER_H
#define SELF_LEARNER_H

#include "multi_topology.h"
#include "memory_system.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 自主学习器配置 */
typedef struct {
    int   seeds_per_cycle;       /* 每轮采样起始节点数 (默认 8) */
    int   walk_depth;            /* 探索深度/跳数 (默认 6) */
    float transitive_boost;      /* 传递性新建边权重 (默认 0.3) */
    float contradiction_decay;   /* 矛盾边衰减系数 (默认 0.7) */
    float similarity_threshold;  /* 语义相似度阈值 (默认 0.55) */
    float novelty_decay;         /* 探索后新奇度衰减 (默认 0.5) */
    int   max_explore_history;   /* 探索历史容量/节点 (默认 5000) */
    int   verbose;
} SelfLearnerConfig;

#define SELF_LEARNER_DEFAULT_CONFIG { \
    8, 6, 0.3f, 0.7f, 0.55f, 0.5f, 5000, 1 \
}

/** 自主学习器句柄 */
typedef struct SelfLearner SelfLearner;

SelfLearner* self_learner_create(MasterTopology* master,
                                  SelfLearnerConfig* config);
void self_learner_destroy(SelfLearner* sl);

/**
 * 执行一轮自主学习循环
 * @return 本轮修改的边数
 */
int self_learner_cycle(SelfLearner* sl);

/** 获取统计信息 */
void self_learner_stats(SelfLearner* sl, int* total_cycles,
                        int* total_created, int* total_demoted,
                        int* total_transitive);

#ifdef __cplusplus
}
#endif

#endif /* SELF_LEARNER_H */
