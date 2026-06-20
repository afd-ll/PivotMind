/**
 * @file dream_engine.h
 * @brief 梦境引擎 — 空闲时知识重组、联想与强化
 *
 * 模拟人脑睡眠中的记忆整合过程：
 *   - 随机激活节点 → 多跳随机游走 → 发现新关联
 *   - 弱边强化：语义相关但连接弱 → 在梦境中加强
 *   - 跨拓扑联想：发现隐含的跨拓扑关系
 *   - 不污染正式推理的状态（梦境激活在退出前衰减）
 *
 * 调用方：后台时钟 background_clock，每 N 秒触发一次
 */

#ifndef DREAM_ENGINE_H
#define DREAM_ENGINE_H

#include "multi_topology.h"
#include "memory_system.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 梦境配置 */
typedef struct {
    int   sample_vocab;         /* 词汇拓扑采样数 (默认 5) */
    int   sample_semantic;      /* 语义拓扑采样数 (默认 3) */
    int   sample_emotion;       /* 情绪拓扑采样数 (默认 2) */
    int   walk_hops;            /* 每次随机游走跳数 (默认 4) */
    float weak_edge_boost;      /* 弱边强化量 (默认 0.08) */
    float cross_link_threshold; /* 跨拓扑连接创建阈值 (默认 0.4) */
    float dream_decay;          /* 梦境退出时全局衰减率 (默认 0.3) */
    int   max_path_record;      /* 最多记录的路径长度 (默认 32) */
    int   verbose;              /* 是否输出梦境日志 */

    /* 跨拓扑连接噪声控制 — 双阈值固化 */
    int   cross_solidify_rounds;   /* 需要多少轮梦境反复触发才固化 (默认 3) */
    float cross_coupling_weight;   /* 临时耦合的初始权重 (默认 0.12) */
    float cross_coupling_decay;    /* 临时耦合未再触发的衰减率 (默认 0.7) */
} DreamConfig;

/** 默认梦境配置 */
#define DREAM_DEFAULT_CONFIG { \
    5, 3, 2, 4, 0.08f, 0.4f, 0.3f, 32, 1, \
    3, 0.12f, 0.7f \
}

/**
 * 执行一轮梦境循环
 *
 * @param master 主拓扑
 * @param memory 记忆系统（可选，用于记忆回放）
 * @param config 梦境配置
 * @return 本轮梦境产生的边修改数
 */
int dream_cycle(MasterTopology* master, MemorySystem* memory,
                const DreamConfig* config);

#ifdef __cplusplus
}
#endif

#endif /* DREAM_ENGINE_H */
