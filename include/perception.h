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
struct EmergentPOS;

/* v0.5.8: 异步搜索队列容量——主循环只入队，工作线程串行执行网络搜索 */
#define PERCEPT_QUEUE_CAP 16

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
    /* 🔴 v0.5.40 修 bug：原声明为 `int`，而 PERCEPTION_DEFAULT_CONFIG 给的是
     * `0.1f` ⇒ **截断为 0** ⇒ `node->confidence < 0` 恒假 ⇒ 拓扑缺口判据
     * 在结构上永不命中（与 is_valid_query 恒假同类，见工程纪律档案 §7.24）。
     * 语义上它本来就是「阈值」，必须是 float。 */
    float min_confidence_for_search;  /* 置信度低于此值才搜 (默认0.1) */
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

/** 搜索引擎定义 */
#define PM_SEARCH_ENGINE_MAX 8
typedef struct {
    const char* name;              /* 引擎标识，如 "sogou_wx" */
    const char* url_fmt;           /* URL 模板，%s 处替换为查询词（需调用方做 URL 编码） */
    int   timeout_ms;              /* 单次请求超时(ms) */
    float quality_weight;          /* 结果质量权重 (0.0~1.0)，排序时用 */
    int   min_request_interval_ms; /* 同引擎两次请求最小间隔(ms) */
    int   max_requests_per_hour;   /* 每小时请求上限 */

    /* 运行时状态（由感知皮层自动维护） */
    int   failures;                /* 连续失败计数 */
    int   cooldown_until_tick;     /* 冷却到哪个 tick */
    int   requests_today;          /* 今日已请求次数 */
    time_t last_request_time;      /* 上次请求时间戳 */
} SearchEngine;

/** 搜索结果片段（给用户看的摘要） */
typedef struct {
    char  title[256];
    char  snippet[1024];
    char  url[512];
    char  source[32];              /* 来源引擎名 */
    float score;                   /* quality_weight * 本地相关性 */
} SearchSnippet;

/** 感觉皮层句柄 */
typedef struct Perception {
    MasterTopology*  topology;
    MemorySystem*    memory;
    ActiveLearner*   learner;          /* 海马体 — 用于 learn_from_dialog */
    struct ArticleReader* ar;          /* 文章阅读器 — 搜索结果语义理解管线 */
    PerceptionConfig cfg;

    pthread_mutex_t   mutex;           /* 保护本结构体内所有可变状态的并发访问 */

    /* 搜索缓存 */
    SearchCacheEntry* cache_head;      /* LRU 缓存链表头 */
    int               cache_size;      /* 当前缓存条目数 */
    int               cache_max;       /* 最大缓存条目 (默认256) */

    /* 文章积累 */
    int               article_accum_count;  /* 累计未 flush 的搜索结果数 */

    /* 搜索策略 */
    SearchEngine     engines[PM_SEARCH_ENGINE_MAX]; /* 多引擎并行配置 */
    int              engine_count;
    int              provider_cooldown[3]; /* 旧 provider 冷却（兼容，逐步迁移） */
    int              provider_failures[3]; /* 旧 provider 失败计数 */
    time_t           day_reset_time;       /* 上次每日重置时间戳 */

    /* 统计 */
    long  total_searches;
    long  total_concepts_learned;
    long  total_new_connections;

    /* 状态 */
    int   tick_counter;               /* tick 计数（由外部递增） */
    int   gap_cursor;                 /* v0.5.40: 缺口扫描游标。三个 _gap_* 都从
                                       * 节点 start 起环绕扫描；游标每轮前移，避免
                                       * 反复挑同一批词（否则尾部节点永远轮不到）。 */

    /* v0.5.8: 异步搜索队列——主循环只入队不阻塞网络。
     * 修复：perception_tick 原在主循环同步执行 search_and_learn（HTTP 请求），
     * 网络抖动/锁竞争会把脑干主循环拖慢 10-25 倍甚至永久卡死（08-07 实测）。 */
    pthread_mutex_t     queue_mutex;                          /* 队列锁 */
    pthread_cond_t      queue_cond;                           /* 队列条件变量 */
    char                queue_items[PERCEPT_QUEUE_CAP][128];  /* 待搜概念环形缓冲 */
    int                 queue_head;                           /* 队头 */
    int                 queue_tail;                           /* 队尾 */
    int                 queue_count;                          /* 队列中条目数 */
    int                 worker_stop;                          /* 工作线程停止标志 */
    pthread_t           worker_thread;                        /* 搜索工作线程 */
    int                 worker_running;                       /* 工作线程已启动 */
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
 * 每次脑干 tick 调用：丘脑闸门 + 好奇心节律 + 从知识缺口选词。
 *
 * v0.5.40（B-1 感知区三路驱动）——此前本函数把 throttle 与好奇心两条线都丢了，
 * 自己定了「每 cycle_interval_ticks 拍随机挑词」的闹钟（原实现首行即
 * `(void)throttle;`）。现按「动机 / 闸门 / 目标」三路驱动：
 *
 *   闸门  throttle ≤ PERCEPT_THROTTLE_MIN ⇒ 本轮不向外看（丘脑的答案优先）
 *   动机  drive_curiosity 调制周期：好奇心越强，问得越勤
 *   目标  待搜词取自知识缺口（对话缺口 / 模板缺口 / 拓扑孤岛，按 gap_weights
 *         配额）；**不再自己随机造词** —— 没有缺口就不搜
 *
 * 🔴 入队后立即返回，网络由 perception worker 线程串行执行（v0.5.8 契约，
 *    绝不能在主循环里同步跑 HTTP）。
 *
 * @param throttle  丘脑给 THAL_PERCEPTION 的 throttle 值 (0.0~1.0)
 * @param curiosity 下丘脑 drive_curiosity 当前值 (0.05~0.95，基线 0.5)
 * @return 本次实际入队的概念数（0 = 闸门关 / 未到周期 / 无缺口）。
 *         该值由脑干经 thalamus_send_feedback 上报，构成感知区那条「刹车线」。
 */
int perception_tick(Perception* p, float throttle, float curiosity);

/** 感知区闸门下限：throttle 低于此值即认定「此刻不该向外看」。
 *  口径与 visual_cortex_tick() 的 `throttle < 0.1f ⇒ return 0` 对齐。 */
#define PERCEPT_THROTTLE_MIN 0.1f

/** 好奇心基线（= src/hypothalamus.c 的 DRIVE_BASELINE_CURIOSITY）。
 *  curiosity / 本值 = 节律倍率：1.0 = 原周期，>1 更勤，<1 更疏。 */
#define PERCEPT_CURIOSITY_BASELINE 0.5f

/** 一轮最多入队几个概念（原实现里写死的 `if (max_searches > 5) … = 5;`）。
 *  同时决定 perception_tick 里候选数组的长度。 */
#define PERCEPT_MAX_SEARCHES_PER_CYCLE 5

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
 * v0.5.8: 入队式搜索——立即返回，网络搜索由感知 worker 串行执行。
 * 供主循环/自学线程调用，绝不阻塞调用方。
 * @return 1=已入队, 0=队列满或参数无效
 */
int perception_enqueue_search(Perception* p, const char* concept);

/**
 * v0.5.39: 接受丘脑信号总线转来的「定向点单」。
 * 当前唯一来源：海马体的 THAL_SIG_CONSOLIDATE_NODE ——「这个节点置信度低，
 * 去联网查证」。只做 node_id → 概念名 → **异步入队**；
 * 🔴 绝不在此同步执行网络搜索（否则会重演 v0.5.8 之前的脑干主循环被
 * curl 拖死）。队列消费由 perception worker 线程串行负责。
 * @param node_id  词汇子拓扑(TOPO_VOCABULARY)中的节点下标
 * @return 1=已入队, 0=未入队（参数无效 / 节点无效 / 队列满）
 */
int perception_request_concept(Perception* p, int node_id);

/**
 * v0.5.8: 绑定涌现词类系统——喂料路径（article_reader）把未分类新词送入 POS 池
 * @param ep 涌现词类系统指针（可 NULL）
 */
void perception_set_emergent_pos(Perception* p, struct EmergentPOS* ep);

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

/**
 * 用户驱动的联网搜索 — 直接返回搜索结果文本给用户
 * 并行查询多个引擎，合并去重后格式化为可读文本
 * @param query  用户原始查询词（中文/英文）
 * @param max_len 返回文本最大长度（建议 4096）
 * @return malloc 的搜索结果文本，调用者需 free；失败返回 NULL
 */
char* perception_search_for_user(Perception* p, const char* query, int max_len);

/**
 * 查询扩展：从拓扑中获取与 concept 共现频率最高的 N 个关联词
 * @param concept  原始查询词
 * @param expanded 输出数组 (需预先分配 PM_QUERY_EXPAND_MAX 个槽位)
 * @param max      最多扩展几个词（建议 3-5）
 * @return 实际扩展的词数（可能为 0）
 */
int perception_expand_query(Perception* p, const char* concept,
                            char expanded[][128], int max);

/**
 * 从纯文本中提取 QA 对（问句 + 答句）
 * @param text      原始文本（可含 HTML）
 * @param questions 输出：问题数组
 * @param answers   输出：回答数组
 * @param max_pairs 最大提取对数（建议 64）
 * @return 实际提取对数
 */
int perception_extract_qa_pairs(const char* text,
                                 char questions[][512],
                                 char answers[][2048],
                                 int max_pairs);

/**
 * 搜索 + 提取 QA 对 + 喂入自主学习器
 * 一次调用完成：多引擎搜索 → HTML 提取 QA 对 → Hebbian 学习
 * @param query        搜索关键词
 * @param engine_limit 最多用几个引擎（0=全部，建议 3）
 * @return 成功学习的 QA 对数
 */
int perception_search_and_learn_qa(Perception* p, const char* query, int engine_limit);

/**
 * 喂入学习文本 — 走 article_reader PMI 管线建立词共现频率表
 * 用于 /learn 端点，让学习不只是加节点，而是建立词间关联边
 */
int perception_feed_learn_text(Perception* p, const char* text);

#ifdef __cplusplus
}
#endif

#endif /* PERCEPTION_H */
