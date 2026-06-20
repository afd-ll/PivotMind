/**
 * @file thalamus.h
 * @brief 丘脑调度器 — 系统级资源门控 + 脑区信号总线
 *
 * 大脑类比：
 *   丘脑是大脑的"感觉中继站"，筛选哪些信号进入皮层、哪些被抑制。
 *   同时充当各脑区之间的信号中继，替代直接 void* 指针链。
 *
 * 系统映射：
 *   1. 感知层 — 收集各子系统负载信号
 *   2. 决策层 — 优先级排序 → 输出 throttle 值
 *   3. 信号总线 — 脑区间通信路由（替代 void* 指针链）
 *   4. 执行层 — 各子系统根据 throttle 加速/减速/暂停
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
 *  脑区子系统枚举
 * ================================================================ */

typedef enum {
    THAL_PREFRONTAL   = 0,  /* 前额叶 — 对话/决策 */
    THAL_HIPPOCAMPUS  = 1,  /* 海马体 — 学习/记忆/巩固 */
    THAL_DMN          = 2,  /* 默认模式网络 — 梦境/联想 */
    THAL_PERCEPTION   = 3,  /* 感知系统 — 联网搜索/好奇探索 */
    THAL_BROCA        = 4,  /* 布罗卡区 — 句式生成 */
    THAL_CEREBELLUM   = 5,  /* 小脑 — 微调/BPTT */
    THAL_AMYGDALA     = 6,  /* 杏仁核 — 情绪/效价调控 */
    THAL_PREF_EXEC    = 7,  /* 前额叶执行器 — 推理编排/子目标调度/想法竞争 (v0.3) */
    THAL_HYPOTHALAMUS = 8,  /* 下丘脑 — 需求/动机调控 (v0.4) */
    THAL_SUBSYSTEM_COUNT = 9
} ThalamusSubsystem;

/* ================================================================
 *  脑区信号枚举 — 丘脑总线上传输的信号类型
 * ================================================================ */

typedef enum {
    THAL_SIG_NONE = 0,

    /* 脑干 → 所有脑区：节律/资源信号 */
    THAL_SIG_HEARTBEAT,         /* 心跳 tick */
    THAL_SIG_CIRCADIAN_UPDATE,  /* 昼夜节律更新 */
    THAL_SIG_PAUSE,             /* 暂停请求 */
    THAL_SIG_RESUME,            /* 恢复请求 */

    /* 丘脑 → 脑区：throttle 更新 */
    THAL_SIG_THROTTLE_UPDATE,   /* throttle 值已重算 */

    /* 海马体 → 感觉皮层：巩固辅助 */
    THAL_SIG_CONSOLIDATE_NODE,  /* 联网审查节点 */

    /* 感觉皮层 → 海马体：搜索结果 */
    THAL_SIG_SEARCH_RESULT,     /* 搜索结果已学习 */

    /* 前额叶 → 所有脑区：对话事件 */
    THAL_SIG_DIALOG_EVENT,      /* 对话发生 */

    /* 前额叶执行器 (v0.3) — 推理引擎信号 */
    THAL_SIG_REASONING_START,   /* 开始一次推理会话 */
    THAL_SIG_REASONING_END,     /* 推理完成 */
    THAL_SIG_SUBGOAL_START,     /* 子目标开始 */
    THAL_SIG_SUBGOAL_RESULT,    /* 子目标结果 */
    THAL_SIG_IDEA_PROPOSED,     /* 候选想法已生成 */
    THAL_SIG_IDEA_SELECTED,     /* 胜出想法已选定 */

    /* 脑区 → 丘脑：反馈上报 */
    THAL_SIG_FEEDBACK_REPORT    /* 工作报告 */
} BrainSignalType;

/* ================================================================
 *  脑区信号载荷
 * ================================================================ */

typedef struct {
    BrainSignalType type;           /* 信号类型 */
    int  source;                    /* 发送方 (ThalamusSubsystem) */
    int  target;                    /* 接收方 (-1 = 广播) */
    union {
        struct { int tick;  }                 heartbeat;
        struct { float val; char phase[16]; } circadian;
        struct { int node_id; int topo_id; }  consolidate;
        struct { int count; }                 search;
        struct { char text[128]; }            dialog;
        struct { int consolidated; int searched; int dreamed; } feedback;
    } data;
} BrainSignal;

/* ── 每个脑区的信号队列大小 ── */
#define THAL_SIGNAL_QUEUE_SIZE 16

/* ================================================================
 *  自主神经系统 — 交感/副交感张力
 * ================================================================ */

typedef struct {
    float sympathetic;      /* 交感张力 (0.0~1.0) — 战斗/探索模式 */
    float parasympathetic;  /* 副交感张力 (0.0~1.0) — 休息/巩固模式 */
    float arousal;          /* 唤醒水平 (0.0~1.0) */
} AutonomicTone;

/* ================================================================
 *  子拓扑按脑区归属（每个脑区拥有哪些子拓扑）
 * ================================================================ */

#define THAL_MAX_OWNED_TOPOS 5

typedef struct {
    int topo_ids[THAL_MAX_OWNED_TOPOS];  /* 该脑区拥有的子拓扑ID列表 */
    int count;                            /* 拥有的拓扑数 */
} BrainTopoPartition;

/* ================================================================
 *  调度器结构
 * ================================================================ */

typedef struct Thalamus {
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

    /* ── 小脑保护系数（来自 cerebellum_tick，硬件级安全限速） ── */
    float cerebellum_protect;              /* 综合保护系数 (0.0~1.0) */

    /* ── 自主神经系统 ── */
    AutonomicTone autonomic;               /* 交感/副交感张力 */

    /* ── 反馈信号（各脑区上报） ── */
    int   fb_hippo_consolidated;    /* 海马体本轮巩固连接数 */
    int   fb_percept_searched;      /* 感觉皮层本轮搜索数 */
    int   fb_dmn_dreamed;           /* DMN 梦境边修改数 */

    /* ── 正反馈恢复计数 ── */
    int   idle_ticks;               /* 连续无反馈tick数（用于恢复 throttle） */

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

    /* ── 脑区指针注册表（替代 void* 指针链） ── */
    void* region_ptrs[THAL_SUBSYSTEM_COUNT];   /* 各脑区实例指针 */
    BrainTopoPartition partitions[THAL_SUBSYSTEM_COUNT]; /* 子拓扑归属 */

    /* ── 信号队列（每个脑区一个） ── */
    struct {
        BrainSignal slots[THAL_SIGNAL_QUEUE_SIZE];
        int head, tail, count;
    } signal_queues[THAL_SUBSYSTEM_COUNT];

    /* ── 工具指针注册表（非脑区组件，如 node_cache、self_learner） ── */
    void* utility_ptrs[5];
    char  utility_names[5][32];

    /* ── 决策理由（线程安全，替代旧的 static const char*） ── */
    char last_reason[128];               /* 最近一次调度决策理由 */

    /* ── 线程安全 ── */
    pthread_mutex_t lock;
} Thalamus;

/* ── 工具指针槽位枚举 ── */
typedef enum {
    THAL_UTIL_NODE_CACHE = 0,
    THAL_UTIL_SELF_LEARNER = 1,
    THAL_UTIL_COGNITIVE_CTRL = 2,
    THAL_UTIL_TOPO_BRAIN = 3,
    THAL_UTIL_IDEA_ARENA = 4,   /* 想法竞争竞技场 (v0.3) */
    THAL_UTIL_COUNT = 5
} ThalamusUtilitySlot;

/* ================================================================
 *  API
 * ================================================================ */

/* ── 生命周期 ── */

Thalamus* thalamus_create(void);
void thalamus_destroy(Thalamus* th);

/* ── 脑区注册（替代 brainstem_set_*） ── */

/**
 * 注册一个脑区实例指针
 * 之后可通过 thalamus_get_region() 按 ThalamusSubsystem 查找
 */
void thalamus_register_region(Thalamus* th, ThalamusSubsystem sys, void* ptr);

/**
 * 获取已注册的脑区实例指针（脑干通过此函数获取各脑区）
 */
void* thalamus_get_region(Thalamus* th, ThalamusSubsystem sys);

/**
 * 注册工具组件（NodeCache, SelfLearner 等非脑区组件）
 */
void thalamus_register_utility(Thalamus* th, ThalamusUtilitySlot slot, void* ptr);

/**
 * 获取工具组件指针
 */
void* thalamus_get_utility(Thalamus* th, ThalamusUtilitySlot slot);

/**
 * 设置脑区的子拓扑归属
 * 例如：前额叶拥有 [词汇,语义,语用]
 */
void thalamus_set_partition(Thalamus* th, ThalamusSubsystem sys,
                             const int* topo_ids, int count);

/**
 * 查询某拓扑是否归某脑区所有
 */
int  thalamus_owns_topo(Thalamus* th, ThalamusSubsystem sys, int topo_type);

/* ── 信号总线 ── */

/**
 * 发送一个信号到指定（或所有）脑区
 * @param target -1=广播, >=0=指定脑区
 * @return 发送成功数（0=队列满）
 */
int  thalamus_send_signal(Thalamus* th, int target, const BrainSignal* sig);

/**
 * 从指定脑区的信号队列接收信号
 * @param region 接收方脑区
 * @param out 输出缓冲区
 * @param max 最大接收数
 * @return 实际接收数
 */
int  thalamus_recv_signal(Thalamus* th, int region, BrainSignal* out, int max);

/**
 * 检查指定脑区是否有待处理信号
 */
int  thalamus_has_signal(Thalamus* th, int region);

/**
 * 发送快捷反馈信号（替代 thalamus_report）
 */
int  thalamus_send_feedback(Thalamus* th, int source,
                             int consolidated, int searched, int dreamed);

/* ── 感知信号 ── */

void thalamus_update_sensors(Thalamus* th,
                              int dialogs_1h, float learner_ms,
                              int hot_nodes, float cpu);

void thalamus_set_circadian(Thalamus* th, float circadian, const char* phase);
void thalamus_record_dialog(Thalamus* th);

/* ── 调度决策 ── */

void thalamus_tick(Thalamus* th);
float thalamus_get_throttle(Thalamus* th, ThalamusSubsystem subsystem);
const char* thalamus_phase_description(Thalamus* th);
const char* thalamus_decision_reason(Thalamus* th);

/* ── 反馈上报（兼容旧API，内部转信号） ── */

void thalamus_report(Thalamus* th, int consolidated, int searched, int dreamed);

/* ── 小脑保护回灌 ── */

/**
 * 脑干/brainstem 调用，将 cerebellum_tick 返回的保护系数注入丘脑
 */
void thalamus_set_cerebellum_protect(Thalamus* th, float protect);

#ifdef __cplusplus
}
#endif

#endif /* THALAMUS_H */
