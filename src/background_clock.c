/**
 * @file background_clock.c
 * @brief 后台时钟循环实现 — 数字生命的心跳
 *
 * 独立 pthread 线程，每秒 tick 一次，在后台维持系统的时间连续性。
 * 四个核心行为：激活衰减、自发激活、状态漂移、记忆巩固。
 *
 * 线程安全策略：
 *   - 激活衰减/自发激活：持 MasterTopology.rwlock 读锁
 *   - 认知状态漂移：无锁（CognitiveState 仅被 DialogSystem 低频修改）
 *   - 记忆巩固：memory_consolidate() 内部已有互斥锁
 */

#include "background_clock.h"
#include "huarong_topology.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define msleep(ms) Sleep(ms)
#else
#define msleep(ms) usleep((ms) * 1000)
#endif

// ==================== 认知状态默认基线 ====================

/** 认知状态回归基线 — 模拟"平静中性"的内稳态 */
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
 * 对一个子拓扑的所有节点施加激活衰减
 *
 * activation *= decay_rate ; if < floor → 0
 * 在调用者的读锁保护下执行，安全地修改浮点值。
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
 * 低噪自发激活：在随机拓扑的随机节点上注入弱激活
 *
 * 模拟大脑即使无外部输入时的背景放电活动。
 * 在调用者的读锁下执行。
 */
static void spontaneous_activate(BackgroundClock* clock) {
    MasterTopology* master = clock->master;
    if (!master || master->sub_topo_count == 0) return;

    // 选择随机子拓扑
    int topo_idx = rand() % master->sub_topo_count;
    SubTopology* sub = master->sub_topologies[topo_idx];
    if (!sub || !sub->net || sub->net->node_count == 0) return;

    // 选择随机节点
    int node_idx = rand() % sub->net->node_count;
    ReasoningNode* node = sub->net->nodes[node_idx];
    if (!node) return;

    // 注入弱激活
    float added = clock->spontaneous_strength * (0.5f + (rand() % 100) / 200.0f);
    node->activation += added;
    if (node->activation > 1.0f) {
        node->activation = 1.0f;
    }
}

/**
 * 认知状态漂移：drive/emotion/valence 缓慢回归基线
 *
 * 无锁操作 — CognitiveState 的低频修改天然安全。
 * 浮点数在 x86 上的对齐写入本身就是原子的。
 */
static void drift_cognitive_state(BackgroundClock* clock) {
    CognitiveState* state = clock->cognitive_state;
    if (!state) return;

    const float keep = 0.995f;
    const float pull = 0.005f;

    // Drive — 向基线回归
    state->drive_curiosity = state->drive_curiosity * keep + BASELINE_STATE.drive_curiosity * pull;
    state->drive_hunger    = state->drive_hunger    * keep + BASELINE_STATE.drive_hunger    * pull;
    state->drive_social    = state->drive_social    * keep + BASELINE_STATE.drive_social    * pull;
    state->drive_comfort   = state->drive_comfort   * keep + BASELINE_STATE.drive_comfort   * pull;

    // Emotion — 向基线回归
    state->emotion_pleasure = state->emotion_pleasure * keep + BASELINE_STATE.emotion_pleasure * pull;
    state->emotion_pain     = state->emotion_pain     * keep + BASELINE_STATE.emotion_pain     * pull;
    state->emotion_security = state->emotion_security * keep + BASELINE_STATE.emotion_security * pull;

    // Valence — 向中性 0 缓慢回归
    state->valence *= 0.998f;
    if (fabsf(state->valence) < 0.001f) {
        state->valence = 0.0f;
    }

    // Explore rate — 效价调制探索率
    state->explore_rate = 0.5f + state->valence * 0.5f;
    if (state->explore_rate < 0.05f) state->explore_rate = 0.05f;
    if (state->explore_rate > 0.95f) state->explore_rate = 0.95f;
}

// ==================== 主时钟循环 ====================

/**
 * 后台时钟线程主循环
 *
 * 每个 tick 执行四个阶段：
 *  1. 激活衰减 — 持读锁遍历全拓扑
 *  2. 自发激活 — 持读锁随机注入
 *  3. 状态漂移 — 无锁更新 CognitiveState
 *  4. 记忆巩固 — 每 N 个 tick 调用一次
 */
static void* clock_loop(void* arg) {
    BackgroundClock* clock = (BackgroundClock*)arg;

    if (clock->verbose) printf("[后台时钟] 启动，tick 间隔=%d ms\n", clock->tick_interval_ms);

    while (clock->is_running) {
        int tick_ms = clock->tick_interval_ms;
        int slept = 0;
        while (slept < tick_ms && clock->is_running) {
            msleep(100);
            slept += 100;
        }
        if (!clock->is_running) break;

        clock->tick_count++;

        // ════════════════════════════════════════════════
        // 阶段 1+2：激活衰减 + 自发激活（持读锁）
        // 读锁之间不互斥，不阻塞前台对话
        // ════════════════════════════════════════════════
        pthread_rwlock_rdlock(&clock->master->rwlock);

        // 1. 全拓扑激活衰减
        for (int t = 0; t < clock->master->sub_topo_count; t++) {
            decay_sub_topology(clock->master->sub_topologies[t],
                               clock->decay_per_tick,
                               PM_CLOCK_ACTIVATION_FLOOR);
        }

        // 2. 自发激活（概率性）
        // 按每拓扑平均节点数计算期望次数
        float total_nodes = 0.0f;
        for (int t = 0; t < clock->master->sub_topo_count; t++) {
            SubTopology* sub = clock->master->sub_topologies[t];
            if (sub && sub->net) total_nodes += sub->net->node_count;
        }
        float expected = total_nodes * clock->spontaneous_prob;
        int num_spontaneous = (int)expected;
        // 小数部分概率触发一次额外的
        if ((float)(rand() % 10000) / 10000.0f < (expected - num_spontaneous)) {
            num_spontaneous++;
        }
        for (int s = 0; s < num_spontaneous; s++) {
            spontaneous_activate(clock);
        }

        pthread_rwlock_unlock(&clock->master->rwlock);

        // ════════════════════════════════════════════════
        // 阶段 3：认知状态漂移（无锁）
        // ════════════════════════════════════════════════
        drift_cognitive_state(clock);

        // ════════════════════════════════════════════════
        // 阶段 4：记忆巩固（每 N tick）
        // memory_consolidate 内部有 mutex
        // ════════════════════════════════════════════════
        if (clock->memory &&
            clock->tick_count % clock->consolidate_every_n_ticks == 0) {
            memory_consolidate(clock->memory);
        }
    }

    if (clock->verbose) printf("[后台时钟] 已停止，总 tick=%d\n", clock->tick_count);
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

    // 初始化随机种子
    srand((unsigned int)time(NULL));

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

    if (clock->verbose) printf("[后台时钟] 线程已启动 (tid=%p)\n", (void*)clock->thread);
}

void background_clock_stop(BackgroundClock* clock) {
    if (!clock || !clock->is_running) return;

    clock->is_running = 0;
    pthread_join(clock->thread, NULL);

    if (clock->verbose) printf("[后台时钟] 线程已停止 (tick=%d)\n", clock->tick_count);
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
