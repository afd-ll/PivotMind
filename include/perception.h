/**
 * @file perception.h
 * @brief 感觉皮层 — 自主语料输送口
 *
 * 大脑类比：
 *   感觉皮层负责处理外部感官输入。在这里映射为：
 *   好奇心驱动地搜索外部世界（联网），将结果喂给海马体（学习）。
 *
 * 触发模式：
 *   1. 好奇探索 — 选低置信度节点 → web_search → learn_from_dialog
 *   2. 巩固辅助 — 海马体审查时发现模糊概念 → web_search 补全
 *   3. 话题延伸 — 对话中遇到未知概念 → 立即搜索（被动触发）
 *
 * 与丘脑的关系：
 *   丘脑通过 throttle[THAL_PERCEPTION] 控制感觉皮层的活跃度，
 *   感觉皮层执行搜索后将结果反馈给海马体，报告热节点变化给丘脑。
 */

#ifndef PERCEPTION_H
#define PERCEPTION_H

#include "multi_topology.h"
#include "memory_system.h"
#include "active_learner.h"

/** 搜索触发源 */
typedef enum {
    PERCEPT_CURIOSITY   = 0,  /* 好奇心驱动 */
    PERCEPT_CONSOLIDATE = 1,  /* 巩固辅助 */
    PERCEPT_DIALOG      = 2,  /* 对话被动 */
    PERCEPT_IDLE        = 3,  /* 空闲随机探索 */
} PerceptionSource;

/** 感觉皮层配置 */
typedef struct {
    int   max_searches_per_cycle;   /* 每次搜索周期最多搜几个概念 (默认5) */
    int   min_confidence_for_search; /* 置信度低于此值才搜 (默认0.3) */
    int   search_timeout_ms;        /* 单次搜索超时(ms) (默认5000) */
    int   cycle_interval_ticks;     /* 搜索周期间隔(脑干tick数) (默认300) */
    int   fallback_interval_ticks;  /* 保底触发间隔 — 无论多忙必搜 (默认1200=20分钟) */
    int   verbose;
} PerceptionConfig;

#define PERCEPTION_DEFAULT_CONFIG { \
    5, 0.3f, 5000, 300, 1200, 1 \
}

/** 感觉皮层句柄 */
typedef struct Perception {
    MasterTopology*  topology;
    MemorySystem*    memory;
    ActiveLearner*   learner;          /* 海马体 — 用于 learn_from_dialog */
    PerceptionConfig cfg;

    /* 统计 */
    long  total_searches;
    long  total_concepts_learned;
    long  total_new_connections;

    /* 状态 */
    int   tick_counter;               /* tick 计数（由外部递增） */
    int   fallback_counter;           /* 保底触发计时器 */
} Perception;

/**
 * 创建感觉皮层
 */
Perception* perception_create(MasterTopology* topology,
                               MemorySystem* memory,
                               ActiveLearner* learner,
                               PerceptionConfig* cfg);

void perception_destroy(Perception* p);

/**
 * 每次脑干 tick 调用，根据 throttle 决定是否执行搜索
 * @param throttle  丘脑给 THAL_PERCEPTION 的 throttle 值 (0.0~1.0)
 */
void perception_tick(Perception* p, float throttle);

/**
 * 被动触发：对话中遇到一个未知概念，立即搜索学习
 * @param concept  概念名称（中文词）
 * @return 新创建的连接数，-1=失败
 */
int perception_learn_concept(Perception* p, const char* concept);

/**
 * 巩固辅助：审查一个冷却节点，联网补全其知识
 * @param node_id  节点ID
 * @return 新创建的连接数
 */
int perception_consolidate_node(Perception* p, int node_id);

/**
 * 获取统计信息
 */
void perception_stats(Perception* p, long* searches, long* learned, long* new_conns);

#ifdef __cplusplus
}
#endif

#endif /* PERCEPTION_H */
