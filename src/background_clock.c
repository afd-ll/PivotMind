/**
 * @file background_clock.c
 * @brief 后台时钟循环实现 �?数字生命的心�?
 *
 * 独立 pthread 线程，每�?tick 一次，在后台维持系统的时间连续性�?
 * 四个核心行为：激活衰减、自发激活、状态漂移、记忆巩固�?
 *
 * 线程安全策略�?
 *   - 激活衰�?自发激活：�?MasterTopology.rwlock 读锁
 *   - 认知状态漂移：无锁（CognitiveState 仅被 DialogSystem 低频修改�?
 *   - 记忆巩固：memory_consolidate() 内部已有互斥�?
 */

#include "background_clock.h"
#include "huarong_topology.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define msleep(ms) Sleep(ms)
#else
#define msleep(ms) usleep((ms) * 1000)
#endif

// ==================== 认知状态默认基�?====================

/** 认知状态回归基�?�?模拟"平静中�?的内稳�?*/
static const CognitiveState BASELINE_STATE = {
    .drive_curiosity  = 0.5f,
    .drive_hunger     = 0.3f,
    .drive_social     = 0.5f,
    .drive_comfort    = 0.5f,
    .emotion_pleasure = 0.5f,
    .emotion_pain     = 0.1f,
    .emotion_security = 0.6f,
    .valence          = 0.0f,
    .current_goal     = NULL,
    .goal_strength    = 0.0f,
    .plan_step        = 0,
    .explore_rate     = 0.5f
};

// ==================== 内部辅助函数 ====================

/**
 * 对一个子拓扑的所有节点施加激活衰�?
 *
 * activation *= decay_rate ; if < floor �?0
 * 在调用者的读锁保护下执行，安全地修改浮点值�?
 */
static void decay_sub_topology(SubTopology* sub, float decay_rate, float floor_value) {
    if (!sub || !sub->net || !sub->net->nodes) return;

    HuarongTopologyNet* net = sub->net;
    for (int i = 0; i < net->node_count; i++) {
        ReasoningNode* node = net->nodes[i];
        if (!node) continue;
        if (node->activation <= floor_value) {
            node->activation = 0.0f;
            continue;
        }
        node->activation *= decay_rate;
        if (node->activation < floor_value) {
            node->activation = 0.0f;
        }
    }
}

/**
 * 低噪自发激活：在随机拓扑的随机节点上注入弱激�?
 *
 * 模拟大脑即使无外部输入时的背景放电活动�?
 * 在调用者的读锁下执行�?
 */
/* 线程安全本地随机数生成器 (LCG) */
static unsigned int local_rand(unsigned int* seed) {
    *seed = *seed * 1103515245 + 12345;
    return (*seed >> 16) & 0x7FFF;
}

static void spontaneous_activate(BackgroundClock* clock) {
    MasterTopology* master = clock->master;
    if (!master || master->sub_topo_count == 0) return;

    /* �?tick 激�?3 个随机节点（原来只激�?1 个，不足以打破冻住的拓扑�?*/
    for (int attempt = 0; attempt < 3; attempt++) {
        int topo_idx = local_rand(&clock->_rng_seed) % master->sub_topo_count;
        SubTopology* sub = master->sub_topologies[topo_idx];
        if (!sub || !sub->net || sub->net->node_count == 0) continue;

        int node_idx = local_rand(&clock->_rng_seed) % sub->net->node_count;
        ReasoningNode* node = sub->net->nodes[node_idx];
        if (!node) continue;

        float added = clock->spontaneous_strength * (0.5f + local_rand(&clock->_rng_seed) / 65535.0f);
        node->activation += added;
        if (node->activation > 1.0f) node->activation = 1.0f;
    }
}

/**
 * 认知状态漂移：drive/emotion/valence 缓慢回归基线
 *
 * 无锁操作 �?CognitiveState 的低频修改天然安全�?
 * 浮点数在 x86 上的对齐写入本身就是原子的�?
 */
static void drift_cognitive_state(BackgroundClock* clock) {
    CognitiveState* state = clock->cognitive_state;
    if (!state) return;

    const float keep = 0.995f;
    const float pull = 0.005f;

    // Drive �?向基线回�?
    state->drive_curiosity = state->drive_curiosity * keep + BASELINE_STATE.drive_curiosity * pull;
    state->drive_hunger    = state->drive_hunger    * keep + BASELINE_STATE.drive_hunger    * pull;
    state->drive_social    = state->drive_social    * keep + BASELINE_STATE.drive_social    * pull;
    state->drive_comfort   = state->drive_comfort   * keep + BASELINE_STATE.drive_comfort   * pull;

    // Emotion �?向基线回�?
    state->emotion_pleasure = state->emotion_pleasure * keep + BASELINE_STATE.emotion_pleasure * pull;
    state->emotion_pain     = state->emotion_pain     * keep + BASELINE_STATE.emotion_pain     * pull;
    state->emotion_security = state->emotion_security * keep + BASELINE_STATE.emotion_security * pull;

    // 回路8: 节点级valence反馈到全局情绪
    // 从情绪拓扑采样节点valence，将其平均值的10%混入全局valence漂移
    {
        SubTopology* emo = NULL;
        for (int t = 0; t < clock->master->sub_topo_count; t++) {
            SubTopology* s = clock->master->sub_topologies[t];
            if (s && s->type == TOPO_EMOTION) { emo = s; break; }
        }
        if (emo && emo->net && emo->net->node_count > 0) {
            float sample_sum = 0.0f;
            int sample_n = emo->net->node_count < 20 ? emo->net->node_count : 20;
            for (int i = 0; i < sample_n; i++) {
                int idx = local_rand(&clock->_rng_seed) % emo->net->node_count;
                ReasoningNode* n = emo->net->nodes[idx];
                if (n) sample_sum += n->valence;
            }
            float topo_val = sample_sum / sample_n;
            state->valence = state->valence * 0.9f + topo_val * 0.1f;
        }
    }
    // Valence �?向中�?0 缓慢回归
    state->valence *= 0.998f;
    if (fabsf(state->valence) < 0.001f) {
        state->valence = 0.0f;
    }

    // Explore rate �?效价调制探索�?
    state->explore_rate = 0.5f + state->valence * 0.5f;
    if (state->explore_rate < 0.05f) state->explore_rate = 0.05f;
    if (state->explore_rate > 0.95f) state->explore_rate = 0.95f;
}

// ==================== 主时钟循�?====================

/**
 * 后台时钟线程主循�?
 *
 * 每个 tick 执行四个阶段�?
 *  1. 激活衰�?�?持读锁遍历全拓扑
 *  2. 自发激�?�?持读锁随机注�?
 *  3. 状态漂�?�?无锁更新 CognitiveState
 *  4. 记忆巩固 �?�?N �?tick 调用一�?
 */
static void* clock_loop(void* arg) {
    BackgroundClock* clock = (BackgroundClock*)arg;

    if (clock->verbose) printf("[后台时钟] 启动，tick 间隔=%d ms\n", clock->tick_interval_ms);

    while (clock->is_running) {
        /* 基于绝对时间的节拍，避免累积漂移 */
        time_t tick_start = time(NULL);
        int slept = 0;
        int tick_ms = clock->tick_interval_ms;
        while (slept < tick_ms && clock->is_running) {
            msleep(100);
            slept += 100;
        }
        if (!clock->is_running) break;

        /* 如果系统调度导致超时，跳过本 tick 的额外等�?*/
        (void)tick_start;  /* 预留：未来可改用 clock_gettime 做精确补�?*/

        clock->tick_count++;

        // ════════════════════════════════════════════════
        // 阶段 1+2：激活衰�?+ 自发激活（持读锁）
        // 读锁之间不互斥，不阻塞前台对�?
        // ════════════════════════════════════════════════
        pthread_rwlock_rdlock(&clock->master->rwlock);

        // 1. 全拓扑激活衰�?
        for (int t = 0; t < clock->master->sub_topo_count; t++) {
            decay_sub_topology(clock->master->sub_topologies[t],
                               clock->decay_per_tick,
                               PM_CLOCK_ACTIVATION_FLOOR);
        }

        // 2. 自发激活（概率性）
        // 按每拓扑平均节点数计算期望次�?
        float total_nodes = 0.0f;
        for (int t = 0; t < clock->master->sub_topo_count; t++) {
            SubTopology* sub = clock->master->sub_topologies[t];
            if (sub && sub->net) total_nodes += sub->net->node_count;
        }
        float expected = total_nodes * clock->spontaneous_prob;
        int num_spontaneous = (int)expected;
        // 小数部分概率触发一次额外的
        if ((float)local_rand(&clock->_rng_seed) / 32767.0f < (expected - num_spontaneous)) {
            num_spontaneous++;
        }
        for (int s = 0; s < num_spontaneous; s++) {
            spontaneous_activate(clock);
        }

        pthread_rwlock_unlock(&clock->master->rwlock);

        // ════════════════════════════════════════════════
        // 阶段 3：认知状态漂移（无锁�?
        // ════════════════════════════════════════════════
        drift_cognitive_state(clock);

        // ════════════════════════════════════════════════
        // 阶段 4：记忆巩固（�?N tick�?
        // memory_consolidate 内部�?mutex
        // ════════════════════════════════════════════════
        if (clock->memory &&
            clock->tick_count % clock->consolidate_every_n_ticks == 0) {
            memory_consolidate(clock->memory);
        }
    }

    if (clock->verbose) printf("[后台时钟] 已停止，�?tick=%d\n", clock->tick_count);
    return NULL;
}

// ==================== API 实现 ====================

BackgroundClock* background_clock_create(MasterTopology* master,
                                        MemorySystem* memory,
                                        CognitiveState* state) {
    if (!master) return NULL;

    BackgroundClock* clock = (BackgroundClock*)calloc(1, sizeof(BackgroundClock));
    if (!clock) return NULL;

    clock->master          = master;
    clock->memory          = memory;
    clock->cognitive_state = state;

    // 默认参数
    clock->tick_interval_ms       = PM_CLOCK_TICK_INTERVAL_MS;
    clock->decay_per_tick         = PM_CLOCK_DECAY_PER_TICK;
    clock->spontaneous_prob       = PM_CLOCK_SPONTANEOUS_PROB;
    clock->spontaneous_strength   = PM_CLOCK_SPONTANEOUS_STRENGTH;
    clock->consolidate_every_n_ticks = PM_CLOCK_CONSOLIDATE_INTERVAL;

    clock->is_running = 0;
    clock->tick_count = 0;

    /* 初始化线程安全本�?RNG 种子 */
    clock->_rng_seed = (unsigned int)time(NULL) ^ (unsigned int)(uintptr_t)clock;

    return clock;
}

void background_clock_start(BackgroundClock* clock) {
    if (!clock || clock->is_running) return;

    clock->is_running = 1;
    int ret = pthread_create(&clock->thread, NULL, clock_loop, (void*)clock);
    if (ret != 0) {
        clock->is_running = 0;
        fprintf(stderr, "[后台时钟] 线程创建失败 (errno=%d)\n", ret);
        return;
    }

    if (clock->verbose) printf("[后台时钟] 线程已启�?(tid=%p)\n", (void*)clock->thread);
}

void background_clock_stop(BackgroundClock* clock) {
    if (!clock) return;
    /* 原子交换：防止双重停止导致双�?pthread_join */
    int was_running = __sync_lock_test_and_set(&clock->is_running, 0);
    if (!was_running) return;

    pthread_join(clock->thread, NULL);

    if (clock->verbose) printf("[后台时钟] 线程已停�?(tick=%d)\n", clock->tick_count);
}

void background_clock_destroy(BackgroundClock* clock) {
    if (!clock) return;

    if (clock->is_running) {
        background_clock_stop(clock);
    }

    free(clock);
}

int background_clock_tick_count(BackgroundClock* clock) {
    return clock ? clock->tick_count : 0;
}

void background_clock_set_verbose(BackgroundClock* clock, int verbose) {
    if (!clock) return;
    clock->verbose = verbose;
}
