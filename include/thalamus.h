/**
 * @file thalamus.h
 * @brief 丘脑调度器 — 系统级资源门控与子系统协调
 *
 * 大脑类比：
 *   丘脑是大脑的"感觉中继站"，筛选哪些信号进入皮层、哪些被抑制。
 *   网状核（reticular nucleus）通过 GABA 能抑制起门控作用。
 *
 * 系统映射：
 *   1. 感知层 — 收集各子系统负载信号
 *   2. 决策层 — 优先级排序 → 输出 throttle 值
 *   3. 执行层 — 各子系统根据 throttle 加速/减速/暂停
 *
 * 优先级（生物对应）：
 *   P0  前额叶 (对话)      — 永远优先，不能被抢占
 *   P1  海马体 (训练+巩固)  — 对话空闲时运行，可被 P0 中断
 *   P2  默认模式网络(梦境)  — P0/P1 都不跑时运行
 *   P3  感知系统 (好奇探索)  — 最闲时随机采样→联网搜索
 *
 * 感知信号：
 *   dialogs_per_window  — 时间窗口内对话频率
 *   learner_load        — 自学周期累计耗时
 *   hot_node_count      — 当前热节点数
 *   cpu_usage           — CPU 使用率
 *   circadian_phase     — 昼夜阶段（来自脑干）
 */

#ifndef THALAMUS_H
#define THALAMUS_H

#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  子系统枚举
 * ================================================================ */

typedef enum {
    THAL_PREFRONTAL  = 0,  /* 前额叶 — 对话/决策 */
    THAL_HIPPOCAMPUS = 1,  /* 海马体 — 学习/记忆/巩固 */
    THAL_DMN         = 2,  /* 默认模式网络 — 梦境/联想 */
    THAL_PERCEPTION  = 3,  /* 感知系统 — 联网搜索/好奇探索 */
    THAL_BROCA       = 4,  /* 布罗卡区 — 句式生成 */
    THAL_CEREBELLUM  = 5,  /* 小脑 — 微调/BPTT */
    THAL_SUBSYSTEM_COUNT = 6
} ThalamusSubsystem;

/* ================================================================
 *  调度器结构
 * ================================================================ */

typedef struct {
    /* ── 感知信号 ── */
    int   dialogs_per_hour;        /* 最近1小时对话数 */
    int   dialogs_total;           /* 累计对话数 */
    float learner_load_ms;         /* 最近一次自学周期耗时(ms) */
    int   hot_node_count;          /* 当前热节点数 */
    int   cooled_node_count;       /* 当前冷却节点数 */
    float cpu_usage;               /* CPU 使用率 (0.0~1.0) */
    float circadian;               /* 昼夜活动系数 (0.0~1.0, 来自脑干) */
    const char* circadian_phase;   /* 昼夜阶段名称 */

    /* ── 决策输出 ── */
    float throttle[THAL_SUBSYSTEM_COUNT];  /* 0.0=暂停, 0.5=半速, 1.0=全速 */
    int   active_subsystem;                /* 当前活跃的子系统 */

    /* ── 反馈信号（各脑区上报） ── */
    int   fb_hippo_consolidated;    /* 海马体本轮巩固数 */
    int   fb_percept_searched;      /* 感觉皮层本轮搜索数 */
    int   fb_dmn_dreamed;           /* DMN 梦境边修改数 */

    /* ── 历史统计 ── */
    float throttle_history[THAL_SUBSYSTEM_COUNT][32];  /* 环形历史 */
    int   throttle_history_pos;
    int   tick_count;              /* 调度器 tick 数 */

    /* ── 可配置 ── */
    int   dialog_window_seconds;   /* 对话计数窗口 (默认 3600) */
    int   idle_threshold_dialogs;  /* 低于此数视为空闲 (默认 2) */
    int   busy_threshold_dialogs;  /* 高于此数视为繁忙 (默认 10) */
    float learner_slow_ms;         /* 自学周期耗时阈值(ms) — 超过则减速 */
    float cpu_high_threshold;      /* CPU 高负载阈值 */

    /* ── 线程安全 ── */
    pthread_mutex_t lock;
} Thalamus;

/* ================================================================
 *  API
 * ================================================================ */

/**
 * 创建丘脑调度器
 */
Thalamus* thalamus_create(void);

/**
 * 销毁
 */
void thalamus_destroy(Thalamus* th);

/**
 * 更新感知信号（由外部定期调用）
 * @param dialogs_1h  最近1小时对话数，-1表示不更新
 * @param learner_ms  自学周期耗时，-1表示不更新
 * @param hot_nodes   热节点数，-1表示不更新
 * @param cpu         CPU使用率，-1表示不更新
 */
void thalamus_update_sensors(Thalamus* th,
                              int dialogs_1h, float learner_ms,
                              int hot_nodes, float cpu);

/**
 * 设置昼夜信号（由脑干推送）
 */
void thalamus_set_circadian(Thalamus* th, float circadian, const char* phase);

/**
 * 记录一次对话事件
 */
void thalamus_record_dialog(Thalamus* th);

/**
 * 执行一次调度决策（每个后台 tick 调用）
 * 根据当前感知信号计算各子系统的 throttle 值
 */
void thalamus_tick(Thalamus* th);

/**
 * 获取指定子系统的 throttle 值
 */
float thalamus_get_throttle(Thalamus* th, ThalamusSubsystem subsystem);

/**
 * 获取当前阶段描述（用于日志）
 */
const char* thalamus_phase_description(Thalamus* th);

/**
 * 获取当前决策理由（用于调试）
 */
const char* thalamus_decision_reason(Thalamus* th);

/**
 * 脑区反馈上报 — 完成一轮工作后调用
 * @param consolidated  海马体巩固结果数 (-1=不更新)
 * @param searched      感觉皮层搜索结果数 (-1=不更新)
 * @param dreamed       DMN梦境边修改数 (-1=不更新)
 */
void thalamus_report(Thalamus* th, int consolidated, int searched, int dreamed);

#ifdef __cplusplus
}
#endif

#endif /* THALAMUS_H */
