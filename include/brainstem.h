/**
 * @file brainstem.h
 * @brief 脑干节律控制器 — 维持数字生命的基础节律
 *
 * 大脑类比：脑干控制心跳、呼吸、睡眠-觉醒周期。
 * 系统映射：心跳节律、昼夜周期、稀疏衰减、节点冻结、自主子系统调度。
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
    void* thalamus;
    void* perception;
    void* hippocampus;
    void* cerebellum;
    void* cognitive_controller;   /* CognitiveController*, 供 health_monitor 干预 */
    void* topo_brain;             /* TopologyBrain*, 脑区索引扫描 */
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
void brainstem_set_node_cache(Brainstem* bs, void* nc);
void brainstem_set_thalamus(Brainstem* bs, void* th);
void brainstem_set_perception(Brainstem* bs, void* p);
void brainstem_set_hippocampus(Brainstem* bs, void* hc);
void brainstem_set_cerebellum(Brainstem* bs, void* cb);
void brainstem_set_cognitive_controller(Brainstem* bs, void* cc);
void brainstem_set_self_learner(Brainstem* bs, void* sl);
void brainstem_set_topo_brain(Brainstem* bs, void* tb);

#endif
