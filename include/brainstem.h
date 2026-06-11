/**
 * @file brainstem.h
 * @brief 脑干节律控制器 — 维持数字生命的基础节律
 *
 * 大脑类比：
 *   脑干控制心跳、呼吸、睡眠-觉醒周期。不参与高级认知，
 *   但为所有高级功能提供稳定的生物钟基础。
 *
 * 核心职责：
 * 1. 昼夜节律 — 根据实时时钟调制全系统活跃水平
 * 2. 稀疏激活衰减 — 仅抽样处理节点（大脑模式）
 * 3. 自发激活 — 模拟背景放电
 * 4. 认知状态漂移 — drive/emotion/valence 回归基线
 * 5. 节点冷却管理 — 冻结低激活节点到磁盘
 * 6. 触发梦境引擎 / 自主学习（接收丘脑的 throttle 信号）
 *
 * 线程模型：独立 pthread 后台线程
 */

#ifndef BRAINSTEM_H
#define BRAINSTEM_H

#include "multi_topology.h"
#include "memory_system.h"
#include "cognitive_params.h"
#include "constants.h"
#include <pthread.h>
#include <time.h>

typedef struct Brainstem {
    MasterTopology* master;
    MemorySystem*   memory;
    CognitiveState* cognitive_state;

    pthread_t thread;
    volatile int is_running;

    int   tick_interval_ms;
    int   tick_count;
    time_t start_time;
    time_t current_time;

    float decay_per_tick;
    float spontaneous_prob;
    float spontaneous_strength;
    int   consolidate_every_n_ticks;

    int   verbose;
    unsigned int _rng_seed;
    void* self_learner;
    void* node_cache;
    void* thalamus;      /* 丘脑调度器 (opaque) */
    void* perception;    /* 感觉皮层 (opaque) */
    void* hippocampus;   /* 海马体 (opaque) — 用于巩固+感知联动 */
} Brainstem;

Brainstem* brainstem_create(MasterTopology* master,
                             MemorySystem* memory,
                             CognitiveState* state);
void brainstem_start(Brainstem* bs);
void brainstem_stop(Brainstem* bs);
void brainstem_destroy(Brainstem* bs);
int  brainstem_tick_count(Brainstem* bs);
void brainstem_set_verbose(Brainstem* bs, int verbose);
const char* brainstem_get_real_time(Brainstem* bs, char* buf, int size);
long brainstem_get_uptime(Brainstem* bs);
float brainstem_get_circadian(Brainstem* bs);
const char* brainstem_get_circadian_phase(Brainstem* bs);
void brainstem_set_node_cache(Brainstem* bs, void* node_cache);
void brainstem_set_thalamus(Brainstem* bs, void* thalamus);
void brainstem_set_perception(Brainstem* bs, void* perception);
void brainstem_set_hippocampus(Brainstem* bs, void* hippocampus);

#endif
