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

/* 前向声明 */
struct ArticleReader;
struct ReasoningNode;

/** 搜索触发源 */
typedef enum {
    PERCEPT_CURIOSITY   = 0,  /* 好奇心驱动 */
    PERCEPT_CONSOLIDATE = 1,  /* 巩固辅助 */
    PERCEPT_DIALOG      = 2,  /* 对话被动 */
    PERCEPT_IDLE        = 3,  /* 空闲随机探索 */
} PerceptionSource;

/** 知识缺口维度 */
typedef enum {
    GAP_DIALOG    = 0,  /* 对话中未理解的概念（海马体记录） */
    GAP_TEMPLATE  = 1,  /* POS 结构无对应模板 */
    GAP_TOPOLOGY  = 2,  /* 词汇孤岛：低连接 + 低置信度 */
    GAP_COUNT     = 3
} GapDimension;

/** 感觉皮层配置 */
typedef struct {
    int   max_searches_per_cycle;    /* 每次搜索周期最多搜几个概念 (默认5) */
    int   min_confidence_for_search;  /* 置信度低于此值才搜 (默认0.3) */
    int   search_timeout_ms;         /* 单次搜索超时(ms) (默认5000) */
    int   cycle_interval_ticks;      /* 搜索周期间隔(脑干tick数) (默认300) */
    int   fallback_interval_ticks;   /* 保底触发间隔 — 无论多忙必搜 (默认1200=20分钟) */
    int   cache_ttl_seconds;         /* 搜索结果缓存有效期(秒) (默认86400=24h) */
    float gap_weights[GAP_COUNT];    /* 三维度缺口权重 (默认 {0.5,0.3,0.2}) */
    int   topo_gap_edge_threshold;   /* 拓扑缺口：低于此连接数算孤立 (默认3) */
    int   article_flush_interval;    /* 多少篇搜索结果触发一次 article_flush (默认3) */
    int   verbose;
} PerceptionConfig;

#define PERCEPTION_DEFAULT_CONFIG { \
    5, 0.1f, 5000, 60, 60, 86400, \
    { 0.5f, 0.3f, 0.2f }, 3, 3, 1 \
}

/** 搜索缓存条目 */
typedef struct SearchCacheEntry {
    char*  query;              /* 查询词 */
    time_t timestamp;          /* 搜索时间 */
    int    result_count;       /* 返回的结果/新连接数（用于评估收益） */
    struct SearchCacheEntry* next;  /* 链表（简单 LRU） */
} SearchCacheEntry;

/** 感觉皮层句柄 */
typedef struct Perception {
    MasterTopology*  topology;
    MemorySystem*    memory;
    ActiveLearner*   learner;          /* 海马体 — 用于 learn_from_dialog */
    struct ArticleReader* ar;          /* 文章阅读器 — 搜索结果语义理解管线 */
    PerceptionConfig cfg;

    /* 搜索缓存 */
    SearchCacheEntry* cache_head;      /* LRU 缓存链表头 */
    int               cache_size;      /* 当前缓存条目数 */
    int               cache_max;       /* 最大缓存条目 (默认256) */

    /* 文章积累 */
    int               article_accum_count;  /* 累计未 flush 的搜索结果数 */

    /* 搜索策略 */
    int               provider_cooldown[3]; /* 各 provider 冷却到何时(tick) */
    int               provider_failures[3]; /* 连续失败计数 */

    /* 统计 */
    long  total_searches;
    long  total_concepts_learned;
    long  total_new_connections;

    /* 状态 */
    int   tick_counter;               /* tick 计数（由外部递增） */
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
 * @return 本次实际搜索次数（用于反馈上报）
 */
int perception_tick(Perception* p, float throttle);

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

/**
 * 提交对话缺口查询列表（替代逐个调 perception_learn_concept）
 * 由认知控制器或对话系统调用，批量提交未理解的概念
 * @param queries  查询词列表（NULL 终止）
 * @return 成功搜索并学习的词数
 */
int perception_suggest_queries(Perception* p, const char** queries);

/**
 * 定时新闻搜索 — 每小时搜 Bing News 头条
 * 不同时段搜不同关键词，保持对现实世界的感知
 * @return 新学到的词数，0=无新词或跳过
 */
int perception_search_news(Perception* p);

#ifdef __cplusplus
}
#endif

#endif /* PERCEPTION_H */
