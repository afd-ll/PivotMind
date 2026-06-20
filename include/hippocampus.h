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
 *   巩固时发现模糊概念 → 通过丘脑信号总线请求感知皮层联网查证
 *
 * 子拓扑归属（architecture note）：
 *   海马体拥有 [上下文拓扑, 领域拓扑]
 *   通过 thalamus_set_partition() 注册
 */

#ifndef HIPPOCAMPUS_H
#define HIPPOCAMPUS_H

#include "multi_topology.h"
#include "memory_system.h"
#include "thalamus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Hippocampus {
    MasterTopology* topology;
    MemorySystem*   memory;

    /* ── 丘脑信号总线（替代 void* 指针链） ── */
    Thalamus*       thalamus;

    /* 对话日志缓冲 — 用于自动抽 QA 对 */
    char dialog_log[4][1024];   /* 最近4轮对话 */
    int  log_pos;
    int  log_count;

    /* 统计 */
    long consolidations;
    long web_queries;
} Hippocampus;

/**
 * 创建海马体
 * @param thalamus 丘脑信号总线（通过它获取感知皮层等依赖）
 */
Hippocampus* hippocampus_create(MasterTopology* topology,
                                 MemorySystem* memory,
                                 Thalamus* thalamus);

void hippocampus_destroy(Hippocampus* hc);

/** 记忆巩固（审查+联网补全模糊概念） */
int hippocampus_consolidate(Hippocampus* hc);

/** 记录对话到海马体日志 */
void hippocampus_log_dialog(Hippocampus* hc, const char* input, const char* response);

/** 记忆存取（委托 memory_system） */
static inline void* hippocampus_remember(Hippocampus* hc, const char* key) {
    return hc ? memory_retrieve(hc->memory, key) : NULL;
}

static inline int hippocampus_store(Hippocampus* hc, const char* key, void* data, int len, int type, float conf) {
    return hc ? memory_store(hc->memory, key, data, len, type, conf) : -1;
}

#ifdef __cplusplus
}
#endif

#endif
