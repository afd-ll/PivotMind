/**
 * @file gateway_internal.h
 * @brief PivotMind HTTP Gateway 内部共享头（v0.5.25 P2-6 gateway 拆分）
 *
 * 由 demos/pivotmind_gateway.c 拆分而来：
 *   - gateway_http.c      JSON/HTTP 工具 + 请求解析
 *   - gateway_system.c    系统初始化/保存/关闭
 *   - gateway_learn.c     学习队列与 worker
 *   - gateway_handlers.c  REST 请求处理 (handle_*)
 *   - pivotmind_gateway.c 连接处理 / token 鉴权 / main
 *
 * 本头集中提供: 引擎头、公共宏、核心结构体与跨模块函数原型。
 */

#include "pivotmind_paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>     /* v0.5.25 P2-4: PIVOTMIND_LOG_FILE 落盘 */
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <dirent.h>   /* v0.5.10: 加载回退 scandir 备份目录 */
#include "pivotmind_version.h"
#include <arpa/inet.h>
#include <sys/stat.h>
#include "dialog_system.h"
#include "active_learner.h"
#include "brainstem.h"
#include "thalamus.h"
#include "perception.h"
#include "hippocampus.h"
#include "cerebellum.h"
#include "broca.h"
#include "prefrontal.h"
#include "emergent_pos.h"
#include "qa_memory.h"
#include "amygdala.h"
#include "multi_topology.h"
#include "memory_system.h"
#include "feature_io.h"
#include "cross_edge_io.h"
#include "feature_pretrain.h"
#include "path_encoding.h"
#include "train_mode.h"
#include "topology_brain.h"
#include "learning_scheduler.h"
#include "node_cache.h"
#include "self_learner.h"
#include "prefrontal_executive.h"  /* v0.3 前额叶执行器 — 推理编排 */
#include "idea_arena.h"            /* v0.3 想法竞争竞技场 */
#include "hypothalamus.h"          /* v0.4 下丘脑 — 需求/动机调控 */
#include "json_config.h"           /* v0.4.7 运行时配置 */
#include "web_fetch.h"             /* 爬虫框架 */
#include "visual_cortex.h"         /* v0.5 视觉皮层脑区 — 多模态感知+对齐 */

// ==================== 配置 ====================

#define GW_DEFAULT_PORT    8080
#define GW_MAX_REQUEST     (64 * 1024)   // 64KB 请求上限
#define GW_MAX_RESPONSE    (128 * 1024)  // 128KB 响应上限
#define GW_READ_TIMEOUT_S  10
#define GW_BACKLOG         16
#define GW_TOKEN_LEN       64            // C1: API token 十六进制长度 (32 字节随机数)
#define GW_TOKEN_FILE      pm_file(PM_FILE_TOKEN)  // C1+: token 持久化文件 (0600, 跨重启不变)；路径 SSOT

/* B4 (v0.5.20): learn 并发队列化开关。
 * LEARN_ASYNC_CHAT: 1 = handle_chat 两处同步 _learn_tokens 改入队（输入 flush / 回复 fire-and-forget）；
 *                   0 = 回退原同步调用（#else 分支原样保留）。
 * LEARN_WORKER_COUNT: 学习 worker 数（2→1，单消费者消除 _learn_tokens 三路并发写）。 */
#define LEARN_ASYNC_CHAT     1
#define LEARN_WORKER_COUNT   1

// ==================== 系统状态 ====================

typedef struct {
    // 核心组件 (与 digital_life.c 相同)
    MasterTopology*   topology;
    MemorySystem*     memory;
    CausalGraph*      causal_graph;
    DialogSystem*     dialog;        /* 兼容旧代码 — 指向 prefrontal->dialog */
    Prefrontal*       prefrontal;   /* 前额叶 — 对话+认知调度 */
    ActiveLearner*    learner;
    Brainstem*       brainstem;     /* 脑干 — 心跳+昼夜节律 */
    Thalamus*        thalamus;      /* 丘脑 — 系统调度器 */
    Perception*      perception;    /* 感觉皮层 — 自主搜索学习 */
    Hippocampus*     hippocampus;   /* 海马体 — 记忆+巩固 */
    Cerebellum*      cerebellum;    /* 小脑 — 资源平衡 */
    QAMemory*        qa_memory;     /* QA 记忆 — 检索式回复 fallback */
    NodeCache*       brain_cache;   /* 大脑式节点冷热缓存 */
    SelfLearner*     self_learner;  /* 自主学习器 — 用于析构时释放 */
    Amygdala*        amygdala;      /* 杏仁核 — 情绪/文化调控 */

    PrefrontalExecutive* pfe;       /* v0.3 前额叶执行器 — 推理编排 */
    IdeaArena*          arena;      /* v0.3 想法竞争竞技场 */
    Broca*              broca;      /* v0.4 布罗卡区 — 模板构建调度 */
    Hypothalamus*       hypothalamus; /* v0.4 下丘脑 — 需求/动机调控 */

    /* v0.5 多模态脑区 */
    VisualCortex*        visual_cortex;  /* 视觉皮层脑区 — 帧提取+跨模态对齐 */

    // 运行控制
    volatile int shutdown_requested;
    volatile int engine_ready;       // 引擎是否完成初始化
    time_t       start_time;
    long         total_dialogs;
    long         total_learning_cycles;
    time_t       last_learn_time;     // 限流用
    int          learn_burst;         // 限流burst计数
    char         last_learn_text[2048]; // 上一条/learn文本，用于建边
    // 训练模式
    TrainMode*      train_mode;      /* 训练模式实例 */
    int             train_mode_flag; /* --train-mode 标志 */
    TrainConfig     train_config;    /* 训练配置 */

    // 学习调度器（自学习 + 增量训练闭环）
    struct LearningScheduler* scheduler;

    // 脑区索引（9+1 脑区，词性涌现）
    struct TopologyBrain* topo_brain;

    // 网关配置
    int   port;
    char  workdir[512];
    ConfigContext* config;       /* 运行时配置 */

    /* C1 修复: API token (媒体喂料端点鉴权)，启动时随机生成并打印到日志 */
    char  api_token[GW_TOKEN_LEN + 1];

    /* 多轮对话上下文 */
    char  last_answer[1024];     /* 上一轮回复（注入扩散引擎保持连贯） */
    int   dialog_context_ready;  /* 是否有可用上下文 */
} GatewaySystem;

typedef struct {
    char method[8];
    char path[256];
    char token[128];     /* C1: X-Pivot-Token 请求头（媒体端点鉴权） */
    char body[GW_MAX_REQUEST];
    int  body_len;
} HttpRequest;

/* B4 (v0.5.20): learn 并发队列化——LearnTask 前向 typedef（完整定义在 gateway_learn.c） */
typedef struct LearnTask LearnTask;

/* 全局网关实例（定义于 pivotmind_gateway.c，learn worker 等跨模块访问） */
extern GatewaySystem* g_gw;

// ==================== 跨模块函数原型 ====================
/* gateway_http.c */
int   json_escape(const char* src, char* dst, int dst_size);
char* json_extract_string(const char* json, const char* key, char* buf, int buf_size);
void  http_send(int fd, int status, const char* content_type, const char* body);
void  http_json(int fd, int status, const char* json_body);
int   gw_token_equal(const char* a, const char* b);
int   parse_request(int fd, HttpRequest* req);

/* gateway_system.c */
void* gw_system_init_thread(void* arg);
void  gw_system_shutdown(GatewaySystem* gw);

/* gateway_learn.c */
int  _learn_tokens(SubTopology* vocab, const char* text, int* p_prev_id, EmergentPOS* ep);
LearnTask* learn_queue_push(const char* msg, const char* domain, int flush);
void learn_task_wait(LearnTask* task);
void learn_queue_init(void);
void learn_queue_shutdown(void);   /* v0.5.25 fix: 停 worker 并 join（关闭最前调用） */

/* gateway_handlers.c */
void handle_chat(GatewaySystem* gw, int fd, const char* body);
void handle_learn(GatewaySystem* gw, int fd, const char* body);
void handle_feedback(GatewaySystem* gw, int fd, const char* body);
void handle_media_feed(GatewaySystem* gw, int fd, const char* body);
void handle_media_status(GatewaySystem* gw, int fd);
void handle_status(GatewaySystem* gw, int fd);
void handle_reach(GatewaySystem* gw, int fd);   /* v0.6.3 */
void handle_root(GatewaySystem* gw, int fd);
void handle_scheduler(GatewaySystem* gw, int fd);
void handle_scheduler_self_stats(GatewaySystem* gw, int fd);
void handle_health(GatewaySystem* gw, int fd);
void handle_qa(GatewaySystem* gw, int fd, const char* body);
