/**
 * @file hippocampus.h
 * @brief 海马体 — 记忆系统 + 自主学习 + 巩固
 *
 * 大脑类比：
 *   海马体负责记忆形成、空间导航、经验重放和记忆巩固。
 *   睡眠期间海马体会回放白天的经验，强化重要连接、弱化噪音。
 *
 * 系统映射：
 *   hippocampus_learn()      — 自主学习：好奇心驱动探索+知识审查
 *   hippocampus_remember()   — 记忆存取：STM→LTM 转移
 *   hippocampus_consolidate() — 巩固：审查连接+联网补全模糊概念
 *
 * 与感觉皮层联动：
 *   巩固时发现模糊概念 → 调用 perception 联网查证 → 联通后写入皮层
 */

#ifndef HIPPOCAMPUS_H
#define HIPPOCAMPUS_H

#include "multi_topology.h"
#include "memory_system.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Hippocampus {
    MasterTopology* topology;
    MemorySystem*   memory;
    void*           perception;    /* 感觉皮层 (opaque, 用于巩固联网) */

    /* 统计 */
    long consolidations;
    long web_queries;
} Hippocampus;

/**
 * 创建海马体
 * @param perception 感觉皮层指针，NULL=巩固时不联网
 */
Hippocampus* hippocampus_create(MasterTopology* topology,
                                  MemorySystem* memory,
                                  void* perception);

void hippocampus_destroy(Hippocampus* hc);

/** 记忆巩固（审查+联网补全模糊概念） */
int hippocampus_consolidate(Hippocampus* hc);

/** 记忆存取（委托 memory_system） */
static inline void* hippocampus_remember(Hippocampus* hc, const char* key, int ctx_id) {
    return hc ? memory_recall(hc->memory, key, ctx_id) : NULL;
}

static inline int hippocampus_store(Hippocampus* hc, const char* key, void* data, int len, int type, float conf) {
    return hc ? memory_store(hc->memory, key, data, len, type, conf) : -1;
}

#ifdef __cplusplus
}
#endif

#endif
