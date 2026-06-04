/**
 * @file background_clock.h
 * @brief 后台时钟循环 — 让系统在无人交互时持续运转
 *
 * 核心职责：
 * 1. 全拓扑激活衰减 — 模拟神经网络自然消退
 * 2. 低噪自发激活 — 模拟"念头涌现"（大脑背景放电）
 * 3. 全局认知状态漂移 — drive/emotion/valence 回归基线
 * 4. 记忆巩固 tick — 周期性 STM→LTM 迁移
 *
 * 线程模型：独立 pthread 后台线程，通过 MasterTopology.rwlock
 * 读锁与前台对话线程协调，遵循 ActiveLearner 的 create/start/stop/destroy 模式。
 *
 * 设计哲学：生命不是一个状态，而是一个持续过程。
 */

#ifndef BACKGROUND_CLOCK_H
#define BACKGROUND_CLOCK_H

#include "multi_topology.h"
#include "memory_system.h"
#include "cognitive_params.h"
#include "constants.h"
#include <pthread.h>

// ==================== 后台时钟结构 ====================

/**
 * 后台时钟 — 模拟数字生命的"心跳"
 *
 * 每秒 tick 一次，在 rwlock 读锁下执行拓扑遍历，
 * 确保不影响前台对话的写锁获取。
 */
typedef struct BackgroundClock {
    // ===== 共享资源引用（只读持有，不拥有） =====
    MasterTopology* master;          // 拓扑网络（持读锁访问）
    MemorySystem*   memory;          // 记忆系统（内部有互斥锁）
    CognitiveState* cognitive_state; // 认知状态（低竞争，无需锁）

    // ===== 线程控制 =====
    pthread_t thread;                // 时钟线程句柄
    volatile int is_running;         // 运行标志（volatile 防编译器优化）

    // ===== 运行时状态 =====
    int   tick_interval_ms;          // 时钟间隔（毫秒，默认 1000）
    int   tick_count;                // 累计 tick 数（uint32 溢出可接受）

    // ===== 可配置参数（创建时设置，运行中不可变） =====
    float decay_per_tick;            // 每 tick 衰减率（默认 0.97）
    float spontaneous_prob;          // 自发激活概率（默认 0.0001）
    float spontaneous_strength;      // 自发激活强度（默认 0.15）
    int   consolidate_every_n_ticks; // 每 N 个 tick 做一次记忆巩固（默认 10）

    int   verbose;                   // 是否打印后台日志（0=静默 1=详细）
    unsigned int _rng_seed;          // 线程安全本地 RNG 种子
} BackgroundClock;

// ==================== API 函数 ====================

/**
 * 创建后台时钟
 *
 * @param master  主拓扑（共享资源，调用者管理生命周期）
 * @param memory  记忆系统（共享资源，调用者管理生命周期）
 * @param state   认知状态（共享资源，调用者管理生命周期；可为 NULL 跳过状态漂移）
 * @return        创建的时钟对象，失败返回 NULL
 */
BackgroundClock* background_clock_create(MasterTopology* master,
                                        MemorySystem* memory,
                                        CognitiveState* state);

/**
 * 启动后台时钟线程
 *
 * 创建独立 pthread，开始周期性 tick。
 * 调用前需确保 master/memory/state 已就绪。
 */
void background_clock_start(BackgroundClock* clock);

/**
 * 停止后台时钟线程
 *
 * 设置 is_running = 0，等待线程 join。
 * 阻塞直到线程安全退出。
 */
void background_clock_stop(BackgroundClock* clock);

/**
 * 销毁后台时钟
 *
 * 如果仍在运行会先 stop。
 * 释放 BackgroundClock 结构体自身，不释放其引用的共享资源。
 */
void background_clock_destroy(BackgroundClock* clock);

/**
 * 获取累计 tick 数（调试用）
 */
int background_clock_tick_count(BackgroundClock* clock);

/**
 * 设置日志输出开关
 * @param verbose 0=静默 1=详细
 */
void background_clock_set_verbose(BackgroundClock* clock, int verbose);

#endif // BACKGROUND_CLOCK_H
