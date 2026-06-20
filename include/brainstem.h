/**
 * @file brainstem.h
 * @brief 脑干节律控制器 — 维持数字生命的基础节律
 *
 * 大脑类比：脑干控制心跳、呼吸、睡眠-觉醒周期。
 * 系统映射：心跳节律、昼夜周期、稀疏衰减、节点冻结、自主子系统调度。
 *
 * 通信原则：
 *   脑干不直接持有其他脑区的指针，所有跨脑区通信通过丘脑(Thalamus)信号总线。
 *   通过 thalamus_get_region() 获取脑区实例，通过 thalamus_get_utility() 获取工具组件。
 */

#ifndef BRAINSTEM_H
#define BRAINSTEM_H

#include "multi_topology.h"
#include "memory_system.h"
#include "cognitive_params.h"
#include "constants.h"
#include "thalamus.h"
#include "health_monitor.h"
#include <pthread.h>
#include <time.h>

typedef struct Brainstem {
    MasterTopology* master;
    MemorySystem*   memory;
    CognitiveState* cognitive_state;

    /* ── 统一信号总线（替代所有 void* 指针） ── */
    Thalamus* thalamus;

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

    /* 内感受自检（替代 brainstem_loop 中的 static 变量） */
    HealthMonitor* health_monitor;

    /* 不再持有 self_learner/node_cache/perception/hippocampus/cerebellum/
     * cognitive_controller/topo_brain 等 void* 字段 —— 全部通过 thalamus 访问 */
} Brainstem;

Brainstem* brainstem_create(MasterTopology* master, MemorySystem* memory,
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

/* ── 丘脑绑定（初始化时调用一次，替代 brainstem_set_* 系列函数） ── */
void brainstem_set_thalamus(Brainstem* bs, Thalamus* th);

/* 已删除：brainstem_set_node_cache / brainstem_set_perception /
 * brainstem_set_hippocampus / brainstem_set_cerebellum /
 * brainstem_set_cognitive_controller / brainstem_set_self_learner /
 * brainstem_set_topo_brain
 * — 以上全部通过 thalamus_register_region /
 * thalamus_register_utility 处理，brainstem 通过
 * thalamus_get_region / thalamus_get_utility 查询 */

#endif
