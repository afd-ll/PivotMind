/**
 * @file learning_scheduler.h
 * @brief 学习调度器 — 协调自学习 + 批量训练 + 评估的闭环
 *
 * 将 self_learner 的自主探索和 train_mode 的批量训练统一为
 * 一个后台循环：自学习跑 N 轮 → 触发增量训练 → 节点评估。
 *
 * 生命周期（后台线程循环）：
 *   IDLE → SELF_LEARN (N cycle) → BATCH_LEARN (增量) → EVALUATE → 回到 SELF_LEARN
 *
 * 用法：
 *   scheduler = learning_scheduler_create(master, memory, learner, cfg);
 *   learning_scheduler_start(scheduler);       // 启动后台线程
 *   ...                                      // 通过 HTTP 接口查询状态
 *   learning_scheduler_stop(scheduler);
 *   learning_scheduler_destroy(scheduler);
 */

#ifndef LEARNING_SCHEDULER_H
#define LEARNING_SCHEDULER_H

#include "multi_topology.h"
#include "memory_system.h"
#include "active_learner.h"
#include "self_learner.h"
#include "train_mode.h"
#include <pthread.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 阶段定义 ==================== */

typedef enum {
    LS_IDLE         = 0,  // 初始/停止态
    LS_SELF_LEARN   = 1,  // 好奇心探索阶段
    LS_BATCH_LEARN  = 2,  // 增量批量训练阶段
    LS_EVALUATE     = 3,  // 节点评估/冷冻标记阶段
} LearningPhase;

static inline const char* learning_phase_name(LearningPhase p) {
    switch (p) {
        case LS_IDLE:       return "idle";
        case LS_SELF_LEARN: return "self_learn";
        case LS_BATCH_LEARN:return "batch_learn";
        case LS_EVALUATE:   return "evaluate";
        default:            return "unknown";
    }
}

/* ==================== 调度器配置 ==================== */

typedef struct {
    /** 每轮调度中自学习的 cycle 次数 (默认 60, ~1 小时) */
    int self_learn_cycles;
    /** 两次自学习 cycle 之间的休眠秒数 (默认 60) */
    int self_learn_interval_s;
    /** 后批量训练的语料路径 (NULL = 不触发批量训练, 仍走 self_learn 闭环) */
    const char* batch_corpus_path;
    /** 评估阶段保留的节点比例上限 (0.0~1.0, 默认 0.9 = 关闭 10%) */
    float eval_keep_ratio;
    /** 详细日志输出 */
    int verbose;
} SchedulerConfig;

#define SCHEDULER_DEFAULT_CONFIG { \
    60, 60, NULL, 0.9f, 1 \
}

/* ==================== 调度器结构 ==================== */

typedef struct LearningScheduler {
    // 配置
    SchedulerConfig cfg;

    // 当前阶段
    volatile LearningPhase phase;
    volatile int should_stop;

    // 自学习
    SelfLearner*     self_learner;
    int              self_learn_remaining;  // 本轮还需跑多少 cycles

    // 批量训练
    TrainMode*       train_mode;

    // 统计
    int              total_loops;           // 完整闭环次数
    int              last_sel_cycles;       // 上次自学习执行的 cycle 数
    int              last_sel_mods;         // 上次自学习的修改数
    int              last_batch_nodes;      // 上次批量训练的新建节点数
    int              last_batch_edges;      // 上次批量训练的新建边数
    int              last_eval_candidates;  // 上次评估的冷冻候选数
    time_t           phase_start_time;      // 当前阶段开始时间

    // 外部组件（不拥有）
    MasterTopology*  master;
    MemorySystem*    memory;
    ActiveLearner*   learner;

    // 后台线程
    pthread_t        thread;
    pthread_mutex_t  lock;
} LearningScheduler;

/* ==================== API ==================== */

/**
 * 创建学习调度器
 * @param master    多拓扑网络
 * @param memory    记忆系统
 * @param learner   主动学习器
 * @param cfg       调度配置 (NULL = 默认)
 * @return 调度器实例
 */
LearningScheduler* learning_scheduler_create(MasterTopology* master,
                                             MemorySystem* memory,
                                             ActiveLearner* learner,
                                             SchedulerConfig* cfg);

/** 销毁调度器（自动 stop） */
void learning_scheduler_destroy(LearningScheduler* ls);

/**
 * 启动调度器后台线程
 * @return 0=成功, -1=已在运行, -2=创建失败
 */
int learning_scheduler_start(LearningScheduler* ls);

/** 停止调度器（等待线程退出） */
void learning_scheduler_stop(LearningScheduler* ls);

/** 获取当前状态（线程安全） */
LearningPhase learning_scheduler_get_phase(LearningScheduler* ls);

/** 填充调度器统计到提供的结构 */
void learning_scheduler_get_stats(LearningScheduler* ls,
                                  int* total_loops,
                                  int* last_sel_cycles,
                                  int* last_sel_mods,
                                  int* last_batch_nodes,
                                  int* last_batch_edges,
                                  int* last_eval_candidates,
                                  const char** phase_name,
                                  long* phase_elapsed_s);

#ifdef __cplusplus
}
#endif

#endif /* LEARNING_SCHEDULER_H */
