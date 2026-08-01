/**
 * @file pivotmind_gateway.c
 * @brief PivotMind HTTP Gateway - REST API 服务网关
 *
 * 暴露 HTTP 接口，支持远程对话和状态查询
 * 链接 libpivotmind.a，复用完整认知引擎
 *
 * API:
 *   POST /chat       {"msg":"..."}     -> {"reply":"...","nodes":492}
 *   POST /learn      {"msg":"..."}     -> {"result":"learned","added":N}
 *   POST /feedback   {"msg":"...","rating":"correct|wrong"} -> {"result":"ok"}
 *   POST /media/feed {"path":"/videos/","mode":"subtitle|visual"} -> {...}
 *   GET  /media/status                 -> {"media_reader":{...},"visual_cortex":{...}}
 *   GET  /status                      -> {"nodes":492,"uptime":3600,...}
 *   GET  /health                      -> {"status":"ok"}
 *
 * 用法:
 *   pivotmind_gateway [port] [workdir]
 *   pivotmind_gateway           # 默认 8080, 工作目录 .
 *   pivotmind_gateway 9090 /opt/pivotmind
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
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
#include "broca.h"
#include "node_cache.h"

#include "self_learner.h"
#include "prefrontal_executive.h"  /* v0.3 前额叶执行器 — 推理编排 */
#include "idea_arena.h"            /* v0.3 想法竞争竞技场 */
#include "hypothalamus.h"          /* v0.4 下丘脑 — 需求/动机调控 */
#include "json_config.h"           /* v0.4.7 运行时配置 */
#include "web_fetch.h"             /* 爬虫框架 */
#include "visual_cortex.h"         /* v0.5 视觉皮层脑区 — 多模态感知+对齐 */

// 前向声明
static int _learn_tokens(SubTopology* vocab, const char* text, int* p_prev_id, EmergentPOS* ep);

// ==================== 配置 ====================

#define GW_DEFAULT_PORT    8080
#define GW_MAX_REQUEST     (64 * 1024)   // 64KB 请求上限
#define GW_MAX_RESPONSE    (128 * 1024)  // 128KB 响应上限
#define GW_READ_TIMEOUT_S  10
#define GW_BACKLOG         16

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

    /* 多轮对话上下文 */
    char  last_answer[1024];     /* 上一轮回复（注入扩散引擎保持连贯） */
    int   dialog_context_ready;  /* 是否有可用上下文 */
} GatewaySystem;
static void handle_qa(GatewaySystem* gw, int fd, const char* body);

static GatewaySystem* g_gw = NULL;

// ==================== 信号处理 ====================

static void gw_signal_handler(int signum) {
    (void)signum;
    if (g_gw) {
        const char msg[] = "\n[gateway] 收到退出信号，正在关闭...\n";
        (void)!write(STDOUT_FILENO, msg, sizeof(msg) - 1);
        g_gw->shutdown_requested = 1;
    }
}

// ==================== JSON 工具 ====================

// 返回 UTF-8 序列长度：1/2/3/4，0=无效字节
static int utf8_seq_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xC0) == 0x80) return 0;   /* 孤立的续字节 */
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 0;
}

static int utf8_valid(const unsigned char* s) {
    int len = utf8_seq_len(s[0]);
    if (len == 0) return 0;
    for (int k = 1; k < len; k++)
        if (!s[k] || (s[k] & 0xC0) != 0x80) return 0;
    return len;
}

// 简易 JSON 字符串转义 + UTF-8 校验 (跳过无效字节避免 JSON 崩坏)
static int json_escape(const char* src, char* dst, int dst_size) {
    if (!src || !dst || dst_size < 2) return -1;
    int j = 0;
    for (int i = 0; src[i] && j < dst_size - 8; i++) {
        unsigned char c = (unsigned char)src[i];

        /* 控制字符 (0x00-0x1F, 不包括 \t \n \r) → \uXXXX */
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') {
            j += snprintf(dst + j, 8, "\\u%04x", c);
            continue;
        }

        /* 多字节 UTF-8 校验 */
        if (c >= 0x80) {
            int len = utf8_valid((const unsigned char*)&src[i]);
            if (len <= 1) continue;  /* 无效 UTF-8 → 跳过 */
            for (int k = 0; k < len && j < dst_size - 2; k++)
                dst[j++] = src[i + k];
            i += len - 1;
            continue;
        }

        switch (src[i]) {
            case '"':  if (j + 2 >= dst_size) goto done; dst[j++] = '\\'; dst[j++] = '"';  break;
            case '\\': if (j + 2 >= dst_size) goto done; dst[j++] = '\\'; dst[j++] = '\\'; break;
            case '\n': if (j + 2 >= dst_size) goto done; dst[j++] = '\\'; dst[j++] = 'n';  break;
            case '\r': if (j + 2 >= dst_size) goto done; dst[j++] = '\\'; dst[j++] = 'r';  break;
            case '\t': if (j + 2 >= dst_size) goto done; dst[j++] = '\\'; dst[j++] = 't';  break;
            default:   dst[j++] = src[i]; break;
        }
    }
done:
    dst[j] = '\0';
    return j;
}

// 从 JSON body 提取 "key":"value" (简单实现，不处理嵌套)
static char* json_extract_string(const char* json, const char* key, char* buf, int buf_size) {
    if (!json || !key || !buf || buf_size < 1) return NULL;

    // 构建 "key" 搜索模式
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char* pos = strstr(json, pattern);
    if (!pos) return NULL;

    pos += strlen(pattern);
    // 跳过空白和冒号
    while (*pos == ' ' || *pos == ':' || *pos == '\t') pos++;
    if (*pos != '"') return NULL;
    pos++; // 跳过开头引号

    int i = 0;
    while (*pos && *pos != '"' && i < buf_size - 1) {
        if (*pos == '\\' && *(pos + 1)) {
            pos++;
            /* \uXXXX → UTF-8 解码：读 4 位 hex，编码为 UTF-8 字节 */
            if (*pos == 'u') {
                unsigned int cp = 0;
                int valid = 1;
                for (int k = 1; k <= 4; k++) {
                    unsigned char hc = (unsigned char)pos[k];
                    if (!hc || !isxdigit(hc)) { valid = 0; break; }
                    cp = cp * 16 + (unsigned int)(hc >= '0' && hc <= '9' ? hc - '0' :
                                                  hc >= 'a' && hc <= 'f' ? hc - 'a' + 10 :
                                                  hc >= 'A' && hc <= 'F' ? hc - 'A' + 10 : 0);
                }
                if (valid && cp > 0) {
                    if (cp < 0x80) {
                        if (i + 1 >= buf_size) break;
                        buf[i++] = (char)cp;
                    } else if (cp < 0x800) {
                        if (i + 2 >= buf_size) break;
                        buf[i++] = (char)(0xC0 | (cp >> 6));
                        buf[i++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        if (i + 3 >= buf_size) break;
                        buf[i++] = (char)(0xE0 | (cp >> 12));
                        buf[i++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[i++] = (char)(0x80 | (cp & 0x3F));
                    }
                }
                pos += 5;  /* 跳过 'u' + 4 位 hex */
                continue;  /* 跳过循环尾部的 pos++ */
            }
            switch (*pos) {
                case 'n':  buf[i++] = '\n'; break;
                case 'r':  buf[i++] = '\r'; break;
                case 't':  buf[i++] = '\t'; break;
                case '"':  buf[i++] = '"';  break;
                case '\\': buf[i++] = '\\'; break;
                default:   buf[i++] = *pos; break;
            }
        } else {
            buf[i++] = *pos;
        }
        pos++;
    }
    buf[i] = '\0';
    return (i > 0) ? buf : NULL;
}

// ==================== HTTP 工具 ====================

// 发送 HTTP 响应 (v0.5.1: 加固 — 检查 write 返回值, 防 EPIPE/ECONNRESET 丢日志)
static void http_send(int fd, int status, const char* content_type, const char* body) {
    const char* status_text = (status == 200) ? "OK" :
                              (status == 400) ? "Bad Request" :
                              (status == 404) ? "Not Found" :
                              (status == 413) ? "Payload Too Large" :
                              "Internal Server Error";

    char header[1024];
    int body_len = (int)strlen(body);
    int hdr_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, body_len);

    /* MSG_NOSIGNAL: write 到已关闭 socket 时返回 -1 而非杀进程 */
    ssize_t sent = send(fd, header, hdr_len, MSG_NOSIGNAL);
    if (sent >= 0)
        send(fd, body, body_len, MSG_NOSIGNAL);
    /* sent < 0: 客户端已断开, 静默忽略 */
}

// 发送 JSON 响应
static void http_json(int fd, int status, const char* json_body) {
    http_send(fd, status, "application/json; charset=utf-8", json_body);
}

// ==================== 系统初始化 (复用 digital_life 逻辑) ====================

static int gw_system_init(GatewaySystem* gw) {
    printf("[gateway] 初始化 PivotMind 引擎...\n");

    // 1. 记忆系统
    gw->memory = memory_system_create(500, 2000, 5000);
    if (!gw->memory) { fprintf(stderr, "[gateway] 记忆系统创建失败\n"); return -1; }
    printf("[gateway]   记忆系统就绪\n");

    // 2. 多拓扑网络
    gw->topology = master_topology_create(16);
    if (!gw->topology) { fprintf(stderr, "[gateway] 拓扑网络创建失败\n"); return -1; }

    master_add_sub_topology(gw->topology, TOPO_VOCABULARY, "词汇拓扑", 100000, 10);
    master_add_sub_topology(gw->topology, TOPO_SEMANTIC,  "语义拓扑", 50000, 9);
    master_add_sub_topology(gw->topology, TOPO_EMOTION,   "情绪拓扑", 4000, 8);
    master_add_sub_topology(gw->topology, TOPO_SYNTAX,    "语法拓扑", 1000, 7);
    master_add_sub_topology(gw->topology, TOPO_CONTEXT,  "上下文拓扑", 1000, 6);
    master_add_sub_topology(gw->topology, TOPO_DOMAIN,    "领域拓扑", 1000, 5);
    master_add_sub_topology(gw->topology, TOPO_PRAGMA,   "语用拓扑", 1000, 4);
    master_add_sub_topology(gw->topology, TOPO_CULTURE,  "文化拓扑", 1000, 3);
    master_add_sub_topology(gw->topology, TOPO_CONCEPT,  "概念拓扑", 50000, 9);
    master_add_sub_topology(gw->topology, TOPO_MASTER,   "主拓扑", 100, 0);
    master_add_sub_topology(gw->topology, TOPO_TEMPLATE, "模板拓扑", 20000, 8);

    // 初始知识
    SubTopology* vocab = master_get_sub_topology_by_type(gw->topology, TOPO_VOCABULARY);
    SubTopology* semantic = master_get_sub_topology_by_type(gw->topology, TOPO_SEMANTIC);
    if (vocab && semantic) {
        huarong_net_add_node(vocab->net, "我", NULL, 0);
        huarong_net_add_node(vocab->net, "你", NULL, 0);
        huarong_net_add_node(vocab->net, "是", NULL, 0);
        huarong_net_add_node(vocab->net, "什么", NULL, 0);
        huarong_net_add_node(vocab->net, "学习", NULL, 0);
        huarong_net_add_node(vocab->net, "知道", NULL, 0);
        huarong_net_add_node(vocab->net, "帮助", NULL, 0);
        huarong_net_add_node(semantic->net, "自我", NULL, 0);
        huarong_net_add_node(semantic->net, "他人", NULL, 0);
        huarong_net_add_node(semantic->net, "存在", NULL, 0);
        huarong_net_add_node(semantic->net, "知识", NULL, 0);
        huarong_net_add_node(semantic->net, "理解", NULL, 0);
        huarong_net_add_node(semantic->net, "协助", NULL, 0);
        huarong_net_add_connection(vocab->net, 0, 0, 0.9f);
        huarong_net_add_connection(vocab->net, 1, 1, 0.9f);
        huarong_net_add_connection(vocab->net, 4, 3, 0.8f);
        huarong_net_add_connection(vocab->net, 5, 4, 0.8f);
        huarong_net_add_connection(vocab->net, 6, 5, 0.8f);
    }
    printf("[gateway]   认知网络就绪 (%d 拓扑)\n", gw->topology->sub_topo_count);

    // 3. 因果图
    gw->causal_graph = causal_graph_create(1000, 5000);
    if (!gw->causal_graph) { fprintf(stderr, "[gateway] 因果图创建失败\n"); return -1; }
    printf("[gateway]   因果图就绪\n");

    // 4. 学习器
    gw->learner = active_learner_create(gw->topology, gw->memory);
    if (!gw->learner) { fprintf(stderr, "[gateway] 学习器创建失败\n"); return -1; }
    active_learner_set_interval(gw->learner, 300);
    printf("[gateway]   学习器就绪 (间隔: 300s)\n");

    // 5. 前额叶（对话系统+认知调度）
    gw->prefrontal = prefrontal_create(gw->topology, gw->memory, gw->causal_graph, gw->learner);
    if (!gw->prefrontal) { fprintf(stderr, "[gateway] 前额叶创建失败\n"); return -1; }
    gw->dialog = prefrontal_dialog(gw->prefrontal);  /* 兼容旧代码 */
    printf("[gateway]   前额叶就绪\n");

    // 5b. QA 记忆检索（扩散/prefrontal 无产出时的兜底）
    gw->qa_memory = qa_memory_create("corpus/xiaohuangji_pipe.txt", 500000);
    if (!gw->qa_memory) {
        printf("[gateway]   QA记忆: 未找到语料或加载失败，继续运行\n");
    } else {
        printf("[gateway]   QA记忆就绪 (%d 对)\n", qa_memory_count(gw->qa_memory));
    }

    // 脑干
    gw->brainstem = brainstem_create(gw->topology, gw->memory, gw->dialog->cognitive_state);
    if (!gw->brainstem) { fprintf(stderr, "[gateway] 脑干创建失败\n"); return -1; }
    printf("[gateway]   脑干就绪\n");

    // 丘脑调度器
    gw->thalamus = thalamus_create();
    if (!gw->thalamus) { fprintf(stderr, "[gateway] 丘脑创建失败\n"); return -1; }
    printf("[gateway]   丘脑就绪\n");

    // 初始化爬虫框架（禁止 robots.txt 检查：AI 学习系统不做商业爬虫合规）
    {
        CrawlPolicy policy = CRAWL_POLICY_DEFAULT;
        policy.respect_robots = 0;  /* 搜狗 /sie? 等路径被 robots.txt 禁了 */
        web_fetch_init(&policy);
    }

    // 感觉皮层（自主语料输送口）
    fprintf(stderr, "[gateway]   创建感觉皮层...\n");
    gw->perception = perception_create(gw->topology, gw->memory, gw->learner, NULL);
    if (!gw->perception) { fprintf(stderr, "[gateway] 感觉皮层创建失败\n"); return -1; }
    /* g_perception 已由 perception_create 自动设置 */
    fprintf(stderr, "[gateway]   感觉皮层就绪\n");

    // 海马体（记忆+巩固，感知联动通过丘脑信号总线）
    fprintf(stderr, "[gateway]   创建海马体...\n");
    gw->hippocampus = hippocampus_create(gw->topology, gw->memory, gw->thalamus);
    if (!gw->hippocampus) { fprintf(stderr, "[gateway] 海马体创建失败\n"); return -1; }
    fprintf(stderr, "[gateway]   海马体就绪\n");

    // 小脑（资源平衡）
    fprintf(stderr, "[gateway]   创建小脑...\n");
    gw->cerebellum = cerebellum_create();
    if (!gw->cerebellum) { fprintf(stderr, "[gateway] 小脑创建失败\n"); return -1; }
    fprintf(stderr, "[gateway]   小脑就绪\n");

    // 杏仁核（情绪/文化效价调控）
    fprintf(stderr, "[gateway]   创建杏仁核...\n");
    gw->amygdala = amygdala_create(gw->topology, gw->thalamus, gw->dialog->cognitive_state);
    if (!gw->amygdala) { fprintf(stderr, "[gateway] 杏仁核创建失败\n"); return -1; }
    fprintf(stderr, "[gateway]   杏仁核就绪\n");

    /* 将认知状态指针注入拓扑，供所有模块通过 master->cognitive_state_ptr 访问 */
    gw->topology->cognitive_state_ptr = gw->dialog->cognitive_state;

    /* ── v0.3 新脑区 ── */
    // 前额叶执行器（推理编排引擎 — 任务分解/子目标调度）
    fprintf(stderr, "[gateway]   创建前额叶执行器 (v0.3)...\n");
    gw->pfe = pfe_create(gw->topology, gw->thalamus);
    if (!gw->pfe) { fprintf(stderr, "[gateway] 前额叶执行器创建失败\n"); return -1; }
    fprintf(stderr, "[gateway]   前额叶执行器就绪 (decompose_depth=%d)\n",
            gw->pfe->max_decompose_depth);

    // 想法竞争竞技场（IdeaArena — 多候选多维度竞争选择）
    fprintf(stderr, "[gateway]   创建想法竞技场 (v0.3)...\n");
    gw->arena = idea_arena_create();
    if (!gw->arena) { fprintf(stderr, "[gateway] 想法竞技场创建失败\n"); return -1; }
    fprintf(stderr, "[gateway]   想法竞技场就绪 (max_candidates=%d)\n", ARENA_MAX_CANDIDATES);

    /* ── v0.4 新脑区 ── */
    // 布罗卡区（句式模板构建与衰减自调度）
    fprintf(stderr, "[gateway]   创建布罗卡区 (v0.4)...\n");
    gw->broca = broca_create(gw->topology);
    if (!gw->broca) { fprintf(stderr, "[gateway] 布罗卡区创建失败\n"); return -1; }
    fprintf(stderr, "[gateway]   布罗卡区就绪 (interval=%d ticks)\n", gw->broca->build_interval_ticks);

    // 下丘脑（需求/动机调控 — 昼夜耦合 + 对话事件调制）
    fprintf(stderr, "[gateway]   创建下丘脑 (v0.4)...\n");
    gw->hypothalamus = hypothalamus_create(gw->dialog->cognitive_state);
    if (!gw->hypothalamus) { fprintf(stderr, "[gateway] 下丘脑创建失败\n"); return -1; }
    fprintf(stderr, "[gateway]   下丘脑就绪\n");

    // 加载持久化数据
    fprintf(stderr, "[gateway]   加载持久化状态...\n");
    if (access("pivotmind_state.dat", F_OK) == 0) {
        int loaded = master_load_state(gw->topology, "pivotmind_state.dat");
        if (loaded >= 0) fprintf(stderr, "[gateway]   加载拓扑状态: %d 节点\n", loaded);
    }

    int feat_loaded = load_features(gw->topology, "features.bin");
    if (feat_loaded > 0) fprintf(stderr, "[gateway]   加载特征向量: %d 节点\n", feat_loaded);
    else { int initted = init_random_features(gw->topology); fprintf(stderr, "[gateway]   初始化特征向量: %d 节点\n", initted); }

    /* 跨拓扑连接已在 master_load_state 中加载，无需重复 */
    fprintf(stderr, "[gateway]   跨拓扑连接已随状态加载 (节点=%d)\n",
            gw->topology->cross_link_count);

    fprintf(stderr, "[gateway]   加载记忆种子...\n");
    memory_load_seed(gw->memory, "memory_seed.dat");
    fprintf(stderr, "[gateway]   记忆种子就绪\n");

    // 模板拓扑 (懒加载：启动时不全量构建，边用边积累)
    SubTopology* tpl = master_get_sub_topology_by_type(gw->topology, TOPO_TEMPLATE);
    if (tpl && tpl->net && tpl->net->node_count > 0) {
        fprintf(stderr, "[gateway]   模板拓扑就绪 (%d 节点)\n", tpl->net->node_count);
        gw->topology->use_template_voting = 1;
    } else {
        fprintf(stderr, "[gateway]   模板拓扑空，将在对话中逐步构建\n");
    }
    // 无论模板是否就绪，都开启投票（空模板时自动降级为无模板）
    gw->topology->use_template_voting = 1;

    fprintf(stderr, "[gateway]   创建节点缓存...\n");
    gw->brain_cache = node_cache_create("brain_state.dat",
                                         gw->topology->sub_topologies[0]->net->max_nodes + 10000);
    if (!gw->brain_cache) {
        fprintf(stderr, "[gateway] 大脑缓存创建失败\n");
        return -1;
    }
    fprintf(stderr, "[gateway]   节点缓存就绪\n");

    /* 注入缓存到所有子拓扑的哈希表，启用自动解冻 */
    for (int t = 0; t < gw->topology->sub_topo_count; t++) {
        SubTopology* sub = gw->topology->sub_topologies[t];
        if (sub && sub->node_hash)
            node_hash_set_cache(sub->node_hash, gw->brain_cache);
    }

    /* ================================================================
     *  丘脑信号总线 — 注册所有脑区 + 工具组件 + 拓扑归属
     * ================================================================ */

    // 注册脑区实例
    thalamus_register_region(gw->thalamus, THAL_PREFRONTAL,  gw->prefrontal);
    thalamus_register_region(gw->thalamus, THAL_HIPPOCAMPUS, gw->hippocampus);
    thalamus_register_region(gw->thalamus, THAL_PERCEPTION,  gw->perception);
    thalamus_register_region(gw->thalamus, THAL_CEREBELLUM,  gw->cerebellum);
    thalamus_register_region(gw->thalamus, THAL_AMYGDALA,    gw->amygdala);
    thalamus_register_region(gw->thalamus, THAL_PREF_EXEC,  gw->pfe);           /* v0.3 前额叶执行器 */
    thalamus_register_region(gw->thalamus, THAL_DMN,         NULL);              /* DMN 是无状态函数 */
    thalamus_register_region(gw->thalamus, THAL_BROCA,       gw->broca);         /* v0.4 Broca 有状态 */
    thalamus_register_region(gw->thalamus, THAL_HYPOTHALAMUS, gw->hypothalamus); /* v0.4 下丘脑 */

    // 注册工具组件
    thalamus_register_utility(gw->thalamus, THAL_UTIL_NODE_CACHE,      gw->brain_cache);
    thalamus_register_utility(gw->thalamus, THAL_UTIL_COGNITIVE_CTRL,  gw->prefrontal->controller);
    thalamus_register_utility(gw->thalamus, THAL_UTIL_SELF_LEARNER,   NULL);  /* self_learner 在后面注册 */
    thalamus_register_utility(gw->thalamus, THAL_UTIL_TOPO_BRAIN,     NULL);  /* topo_brain 在后面注册 */
    thalamus_register_utility(gw->thalamus, THAL_UTIL_IDEA_ARENA,      gw->arena); /* v0.3 */

    /* 根据运行时配置禁用脑区 */
    if (gw->config && gw->config->loaded) {
        if (!gw->config->brain_regions.perception)   thalamus_enable_region(gw->thalamus, THAL_PERCEPTION, 0);
        if (!gw->config->brain_regions.hippocampus)  thalamus_enable_region(gw->thalamus, THAL_HIPPOCAMPUS, 0);
        if (!gw->config->brain_regions.dmn)          thalamus_enable_region(gw->thalamus, THAL_DMN, 0);
        if (!gw->config->brain_regions.broca)        thalamus_enable_region(gw->thalamus, THAL_BROCA, 0);
        if (!gw->config->brain_regions.cerebellum)   thalamus_enable_region(gw->thalamus, THAL_CEREBELLUM, 0);
        if (!gw->config->brain_regions.amygdala)     thalamus_enable_region(gw->thalamus, THAL_AMYGDALA, 0);
        if (!gw->config->brain_regions.hypothalamus) thalamus_enable_region(gw->thalamus, THAL_HYPOTHALAMUS, 0);
        if (!gw->config->brain_regions.visual_cortex) thalamus_enable_region(gw->thalamus, THAL_VISUAL_CORTEX, 0);
    }

    // 设置子拓扑按脑区归属（每个脑区只负责自己的子拓扑）
    {
        int prefrontal_topo[]  = {TOPO_VOCABULARY, TOPO_SEMANTIC, TOPO_PRAGMA, TOPO_CONCEPT, TOPO_DOMAIN};
        int hippocampus_topo[] = {TOPO_CONTEXT, TOPO_DOMAIN};
        thalamus_set_partition(gw->thalamus, THAL_PREFRONTAL,  prefrontal_topo,  5);
        int broca_topo[]       = {TOPO_SYNTAX, TOPO_TEMPLATE};
        int amygdala_topo[]    = {TOPO_EMOTION, TOPO_CULTURE};
        int perception_topo[]  = {TOPO_VISUAL};                      /* v0.5 视觉拓扑归属感知 */
        thalamus_set_partition(gw->thalamus, THAL_PREFRONTAL,  prefrontal_topo,  4);
        thalamus_set_partition(gw->thalamus, THAL_HIPPOCAMPUS, hippocampus_topo, 2);
        thalamus_set_partition(gw->thalamus, THAL_BROCA,       broca_topo,       2);
        thalamus_set_partition(gw->thalamus, THAL_AMYGDALA,    amygdala_topo,    2);
        thalamus_set_partition(gw->thalamus, THAL_PERCEPTION,  perception_topo,  1); /* v0.5 */
        /* 小脑无专属拓扑（全局监控角色） */
    }

    // 绑定丘脑到脑干（脑干通过丘脑获取所有其他脑区的引用）
    brainstem_set_thalamus(gw->brainstem, gw->thalamus);
    brainstem_set_verbose(gw->brainstem, 1);  /* 开启脑区日志 */

    printf("[gateway]   丘脑信号总线就绪 (%d 脑区, %d 工具槽)\n",
           THAL_SUBSYSTEM_COUNT, THAL_UTIL_COUNT);

    // 自主学习器
    {
        gw->self_learner = self_learner_create(gw->topology, NULL);
        if (gw->self_learner) {
            printf("[gateway]   自主学习器就绪\n");
            /* 通过丘洞注册，而非 brainstem_set_self_learner */
            thalamus_register_utility(gw->thalamus, THAL_UTIL_SELF_LEARNER, gw->self_learner);
        }
    }

    /* ── v0.5 视觉皮层脑区 ── */
    {
        VisualCortexConfig vcc = VISUAL_CORTEX_DEFAULT_CONFIG;
        vcc.verbose = 1;
        gw->visual_cortex = visual_cortex_create(gw->topology, &vcc);
        if (gw->visual_cortex) {
            /* 内部 MediaReader 也自动绑定丘脑 */
            MediaReader* mr = visual_cortex_get_media_reader(gw->visual_cortex);
            if (mr) media_reader_set_thalamus(mr, gw->thalamus);

            /* 注册为脑区: 脑干将通过丘脑调度 */
            thalamus_register_region(gw->thalamus, THAL_VISUAL_CORTEX, gw->visual_cortex);
            printf("[gateway]   视觉皮层脑区就绪 (v0.5, TOPO_VISUAL=%d)\n", TOPO_VISUAL);
        } else {
            fprintf(stderr, "[gateway] 视觉皮层创建失败，继续运行\n");
        }
    }

    /* 学习已由脑干统一调度 */
    brainstem_start(gw->brainstem);

    gw->start_time = time(NULL);
    gw->engine_ready = 1;  // 引擎初始化完成，可接受请求
    printf("[gateway] ===== 引擎就绪，开始接受请求 =====\n");

    // 训练模式: 引擎就绪后自动开始喂料
    printf("[DEBUG] train_mode_flag=%d, corpus=%s, topology=%p\n", gw->train_mode_flag, gw->train_config.corpus_path ? gw->train_config.corpus_path : "NULL", (void*)gw->topology);
    if (gw->train_mode_flag && gw->topology) {
        gw->train_mode = train_mode_create(gw->topology, gw->memory, gw->learner, gw->train_config);
        __sync_synchronize();  /* ARM 弱内存序: 确保 train_mode 对所有线程可见 */
        if (gw->train_mode) {
            train_mode_set_thalamus(gw->train_mode, gw->thalamus);
            /* v0.5: 媒体格式训练需要 VisualCortex */
            if (gw->visual_cortex)
                train_mode_set_visual_cortex(gw->train_mode, gw->visual_cortex);
            train_mode_start(gw->train_mode);
        } else {
            fprintf(stderr, "[gateway] 训练模式创建失败\n");
        }
    }

    // 学习调度器（始终启动，后台自学习循环）
    {
        SchedulerConfig scfg = SCHEDULER_DEFAULT_CONFIG;
        if (gw->train_config.corpus_path)
            scfg.batch_corpus_path = gw->train_config.corpus_path;
        gw->scheduler = learning_scheduler_create(gw->topology, gw->memory,
                                                   gw->learner, &scfg);
        if (gw->scheduler) {
            learning_scheduler_start(gw->scheduler);
            printf("[gateway]   学习调度器已启动 (自学习=%d次/轮, 语料=%s)\n",
                   scfg.self_learn_cycles,
                   scfg.batch_corpus_path ? scfg.batch_corpus_path : "无(仅自学习)");
        }
    }

    // 脑区索引（9+1 脑区，词性涌现模块）
    {
        gw->topo_brain = topobrain_create(65536);  // 预分配 64K 节点
        if (gw->topo_brain) {
            printf("[gateway]   脑区索引就绪 (9+1 脑区)\n");
            /* 通过丘洞注册，而非 brainstem_set_topo_brain */
            thalamus_register_utility(gw->thalamus, THAL_UTIL_TOPO_BRAIN, gw->topo_brain);
        }
    }
    printf("[gateway] PivotMind 引擎就绪\n");

    return 0;
}

/* pthread_create 兼容包装 — 消除 cast-function-type 警告 */
static void* gw_system_init_thread(void* arg) {
    gw_system_init((GatewaySystem*)arg);
    return NULL;
}

// ==================== 保存并关闭 ====================

static void gw_system_shutdown(GatewaySystem* gw) {
    if (!gw) return;
    printf("[gateway] 正在关闭...\n");

    // 1. 停止脑干 → 冻结所有活性节点 → 确保状态完整
    if (gw->brainstem) brainstem_stop(gw->brainstem);

    // 2. 保存完整状态到主文件（空启动保护：总节点 < 20 时跳过，防止覆盖有效存盘）
    if (gw->topology) {
        int total = master_count_total_nodes(gw->topology);
        if (total >= 20) {
            int saved = master_save_state(gw->topology, "pivotmind_state.dat");
            if (saved >= 0) printf("[gateway]   保存拓扑状态: %d 节点\n", saved);
            int feat_saved = save_features(gw->topology, "features.bin");
            if (feat_saved > 0) printf("[gateway]   保存特征: %d 节点\n", feat_saved);
            int cross_saved = save_cross_edges(gw->topology, "cross_edges.bin");
            if (cross_saved > 0) printf("[gateway]   保存跨拓扑连接: %d 条\n", cross_saved);
        } else {
            printf("[gateway]   跳过存盘 (总节点=%d < 门卫阈值 20，避免覆盖有效数据)\n", total);
        }
    }
    if (gw->memory) {
        int saved = memory_save_seed(gw->memory, "memory_seed.dat");
        if (saved >= 0) printf("[gateway]   保存记忆种子: %d 条\n", saved);
    }

    // 3. 删除临时状态文件（brain_state.dat 只是脑干运行缓存，主状态已在上面保存）
    remove("brain_state.dat");
    printf("[gateway]   清理临时状态文件\n");

    // 4. 停止学习调度器
    if (gw->scheduler) {
        printf("[gateway]   停止学习调度器...\n");
        learning_scheduler_destroy(gw->scheduler);
        gw->scheduler = NULL;
    }

    // 5. 脑区索引
    if (gw->topo_brain) {
        topobrain_destroy(gw->topo_brain);
        gw->topo_brain = NULL;
    }

    // 6. 销毁资源（brainstem 已在上方 stop，这里只 destroy）
    /* v0.5 视觉皮层脑区 (内部自动销毁 MediaReader) */
    if (gw->visual_cortex)  { visual_cortex_destroy(gw->visual_cortex); gw->visual_cortex = NULL; }
    if (gw->brain_cache) { node_cache_destroy(gw->brain_cache); gw->brain_cache = NULL; }
    if (gw->self_learner) { self_learner_destroy(gw->self_learner); gw->self_learner = NULL; }
    if (gw->amygdala)    amygdala_destroy(gw->amygdala);
    if (gw->hippocampus) hippocampus_destroy(gw->hippocampus);
    if (gw->cerebellum)  cerebellum_destroy(gw->cerebellum);
    if (gw->qa_memory)   qa_memory_destroy(gw->qa_memory);
    if (gw->thalamus)     thalamus_destroy(gw->thalamus);
    if (gw->perception)   perception_destroy(gw->perception);
    web_fetch_destroy();  /* 爬虫框架 */
    if (gw->brainstem)    brainstem_destroy(gw->brainstem);
    if (gw->pfe)         pfe_destroy(gw->pfe);              /* v0.3 */
    if (gw->arena)       idea_arena_destroy(gw->arena);          /* v0.3 */
    if (gw->hypothalamus) hypothalamus_destroy(gw->hypothalamus); /* v0.4 */
    if (gw->broca)       broca_destroy(gw->broca);               /* v0.4 */
    if (gw->learner)     active_learner_destroy(gw->learner);
    if (gw->prefrontal)  prefrontal_destroy(gw->prefrontal);
    if (gw->causal_graph) causal_graph_destroy(gw->causal_graph);
    if (gw->topology)    master_topology_destroy(gw->topology);
    if (gw->memory)      memory_system_destroy(gw->memory);
    if (gw->config)      config_destroy(gw->config);

    printf("[gateway] 已关闭 (运行 %lld 秒, 对话 %lld 轮)\n",
           (long long)(time(NULL) - gw->start_time), (long long)gw->total_dialogs);
}

// ==================== 请求处理 ====================

// POST /chat - 对话
static void handle_chat(GatewaySystem* gw, int fd, const char* body) {
    char msg[2048] = {0};
    if (!json_extract_string(body, "msg", msg, sizeof(msg)) || strlen(msg) == 0) {
        http_json(fd, 400, "{\"error\":\"missing or empty 'msg' field\"}");
        return;
    }

    /* ── v0.3 Phase 2: 复杂问题走 PFE 推理编排 ── */
    int use_pfe = 0;
    char* response = NULL;
    if (gw->qa_memory) { const char* qa = qa_memory_query(gw->qa_memory, msg); if (qa) { response = strdup(qa); } }

    /* v0.4.3: 对话中自动学习 — 将输入 token 注册到词汇拓扑
     * 这是端到端对话质量最大的瓶颈：不学习新词则扩散引擎 active_count=0 */
    {
        SubTopology* vocab = NULL;
        for (int t = 0; t < gw->topology->sub_topo_count; t++) {
            if (gw->topology->sub_topologies[t] &&
                gw->topology->sub_topologies[t]->type == TOPO_VOCABULARY)
                { vocab = gw->topology->sub_topologies[t]; break; }
        }
        if (vocab && vocab->net) {
            EmergentPOS* ep = (gw->prefrontal && gw->prefrontal->controller)
                              ? gw->prefrontal->controller->emergent_pos : NULL;
            int prev_id = -1;
            int learned = _learn_tokens(vocab, msg, &prev_id, ep);
            if (learned > 0)
                printf("[gateway] 对话中学习: +%d 个新词\n", learned);
        }
    }

    if (gw->pfe) {
        int complexity = pfe_assess_complexity(gw->pfe, msg);
        if (complexity > 0) {
            /* 中高复杂度 → PFE 推理管线 */
            char pfe_answer[GW_MAX_RESPONSE];
            int pfe_ok = pfe_reason(gw->pfe, msg, pfe_answer, sizeof(pfe_answer));
            if (pfe_ok == 0 && strlen(pfe_answer) > 10) {
                response = strdup(pfe_answer);
                use_pfe   = 1;
                printf("[gateway] PFE推理完成 (复杂度=%d, 周期=%d, 满意度=%.2f)\n",
                       complexity, pfe_cycle_count(gw->pfe),
                       (double)pfe_avg_satisfaction(gw->pfe));
            }
        }
    }

    /* 回退到旧路径：简单问题或 PFE 失败
     * v0.4.3: 注入上一轮回复作为上下文，使扩散引擎匹配历史 token */
    if (!response) {
        if (gw->dialog_context_ready && gw->last_answer[0]) {
            /* 拼接: 上轮回复 + 当前输入 → 扩散引擎能匹配新旧 token */
            char ctx_input[3072];
            int clen = (int)strlen(gw->last_answer);
            if (clen > 512) clen = 512; /* 截断过长上下文 */
            snprintf(ctx_input, sizeof(ctx_input), "%.*s %s", clen, gw->last_answer, msg);
            response = prefrontal_chat(gw->prefrontal, ctx_input);
        } else {
            response = prefrontal_chat(gw->prefrontal, msg);
        }
    }

    /* 最终兜底：QA 记忆检索（扩散和联想推理都无产出） */
    if (!response && gw->qa_memory) {
        const char* qa_answer = qa_memory_query(gw->qa_memory, msg);
        if (qa_answer) {
            response = strdup(qa_answer);
            printf("[gateway] QA记忆命中\n");
        }
    }

    if (response) {
        char escaped[GW_MAX_RESPONSE];
        json_escape(response, escaped, sizeof(escaped));

        int total_nodes = 0;
        for (int t = 0; t < gw->topology->sub_topo_count; t++) {
            if (gw->topology->sub_topologies[t] && gw->topology->sub_topologies[t]->net)
                total_nodes += gw->topology->sub_topologies[t]->net->node_count;
        }

        char json[GW_MAX_RESPONSE];
        snprintf(json, sizeof(json),
            "{\"reply\":\"%s\",\"nodes\":%d,\"dialogs\":%lld%s}",
            escaped, total_nodes, (long long)gw->total_dialogs + 1,
            use_pfe ? ",\"reasoning\":\"pfe\"" : "");

        http_json(fd, 200, json);
        gw->total_dialogs++;

        /* v0.4.3: 保存本轮回复到多轮对话上下文 */
        if (response && response[0]) {
            int alen = (int)strlen(response);
            if (alen > 1023) alen = 1023;
            memcpy(gw->last_answer, response, (size_t)alen);
            gw->last_answer[alen] = '\0';
            gw->dialog_context_ready = 1;
        }

        /* v0.4.3: AI回复也纳入词汇拓扑 — 双向在线学习 */
        {
            SubTopology* vocab = NULL;
            for (int t = 0; t < gw->topology->sub_topo_count; t++) {
                if (gw->topology->sub_topologies[t] &&
                    gw->topology->sub_topologies[t]->type == TOPO_VOCABULARY)
                    { vocab = gw->topology->sub_topologies[t]; break; }
            }
            if (vocab && vocab->net && response) {
                EmergentPOS* ep = (gw->prefrontal && gw->prefrontal->controller)
                                  ? gw->prefrontal->controller->emergent_pos : NULL;
                int prev_id = -1;
                int learned = _learn_tokens(vocab, response, &prev_id, ep);
                if (learned > 0)
                    printf("[gateway] 回复中学习: +%d 个新词\n", learned);
            }
        }

        /* 海马体记下这次对话 — 巩固时自动建 QA 连接 */
        if (gw->hippocampus) hippocampus_log_dialog(gw->hippocampus, msg, response);

        /* ── v0.3 Phase 2: IdeaArena 胜者反馈回流 ── */
        if (use_pfe && gw->arena && gw->topology) {
            arena_feedback_to_master(gw->arena, gw->topology);
        }

        /* 模板构建已由 Broca 自主 tick 调度（brainstem → broca_tick），
         * 对话路径不再重复调用 */

        free(response);
    } else {
        http_json(fd, 200, "{\"reply\":\"(无回应)\",\"nodes\":0}");
    }
}

/* 中文预处理：连续 CJK 字符间插入空格，使 strtok 能切出单字 */
static void _cjk_insert_spaces(const char* src, char* dst, int dst_sz) {
    int wi = 0;
    for (const char* p = src; *p && wi < dst_sz - 4; ) {
        unsigned char c = (unsigned char)*p;
        int blen = 1;
        if (c >= 0xE0 && c <= 0xEF) blen = 3;
        else if (c >= 0xC0 && c <= 0xDF) blen = 2;

        /* 多字节字符必须完整写入：检查是否有足够空间放 字符+空格+null */
        if (blen > 1 && wi + blen + 2 >= dst_sz) break;

        if (blen == 3 && wi > 0 && (unsigned char)dst[wi-1] != ' '
            && wi + 1 < dst_sz) {
            dst[wi++] = ' ';
        }
        for (int b = 0; b < blen && p[b] && wi < dst_sz - 1; b++)
            dst[wi++] = p[b];
        dst[wi] = '\0';
        p += blen;
    }
    /* 确保 null 结尾 */
    if (wi < dst_sz) dst[wi] = '\0';
}

/* 分词 + 注册到 vocab + 建相邻边，返回新增节点数
 * v0.4.3: 使用 EmergentPOS 同词类边加权 */
static int _learn_tokens(SubTopology* vocab, const char* text,
                         int* p_prev_id, EmergentPOS* ep) {
    if (!vocab || !vocab->net || !text || !text[0]) return 0;

    /* 动态分配缓冲区：CJK spaced 最坏情况 = strlen * 4/3 + 1 */
    int text_len = (int)strlen(text);
    int copy_sz = text_len * 4 / 3 + 32;
    if (copy_sz < 256) copy_sz = 256;
    char* copy = (char*)malloc((size_t)copy_sz);
    if (!copy) return 0;

    _cjk_insert_spaces(text, copy, copy_sz);
    char* tok = strtok(copy, " \t\n\r。，！？、；：\"\"''（）《》…—");
    int added = 0;
    while (tok) {
        if (strlen(tok) >= 2) {
            int nid = huarong_net_find_concept(vocab->net, tok);
            if (nid < 0 && (size_t)vocab->net->node_count < vocab->net->max_nodes) {
                nid = huarong_net_dynamic_add_node(vocab->net, tok, NULL, 0);
                if (nid >= 0) {
                    added++;
                    node_hash_add(vocab->node_hash, vocab->net->nodes[nid]);
                }
            }
            if (nid >= 0) {
                vocab->net->nodes[nid]->activation += 0.1f;
                if (*p_prev_id >= 0 && *p_prev_id != nid) {
                    /* v0.4.3: 涌现词类加权 — 同词类节点边权重更高 */
                    float edge_w = 0.4f;
                    if (ep) {
                        ReasoningNode* prev = vocab->net->nodes[*p_prev_id];
                        ReasoningNode* curr = vocab->net->nodes[nid];
                        if (prev && prev->emergent_class_count > 0 &&
                            curr && curr->emergent_class_count > 0) {
                            for (int pi = 0; pi < prev->emergent_class_count && pi < 4; pi++) {
                                for (int ci = 0; ci < curr->emergent_class_count && ci < 4; ci++) {
                                    if (prev->emergent_class_ids[pi] == curr->emergent_class_ids[ci]) {
                                        edge_w = 0.65f; /* 同词类: 更强的语法关联 */
                                        break;
                                    }
                                }
                                if (edge_w > 0.4f) break;
                            }
                        }
                    }
                    huarong_net_add_connection(vocab->net, *p_prev_id, nid, edge_w);
                }
                *p_prev_id = nid;
            }
        }
        tok = strtok(NULL, " \t\n\r。，！？、；：\"\"''（）《》…—");
    }
    free(copy);
    return added;
}

/* _learn_task — 异步学习参数 */
typedef struct {
    char*  msg;
    char   domain[64];  /* "medical", "legal", "" = default vocab */
} LearnTask;

/* _learn_worker — 异步学习线程 */
static void* _learn_worker(void* arg) {
    LearnTask* task = (LearnTask*)arg;
    if (!task || !task->msg) { free(task); return NULL; }
    GatewaySystem* gw = g_gw;
    if (!gw || !gw->topology) { free(task->msg); free(task); return NULL; }
    
    /* 按域名选择目标拓扑 */
    int target_topo = TOPO_VOCABULARY;
    if (task->domain[0]) {
        target_topo = TOPO_DOMAIN;
        printf("[gateway] [领域] '%s' → 领域拓扑\n", task->domain);
    }
    
    SubTopology* topo = NULL;
    for (int t = 0; t < gw->topology->sub_topo_count; t++) {
        if (gw->topology->sub_topologies[t] && (int)gw->topology->sub_topologies[t]->type == target_topo)
            { topo = gw->topology->sub_topologies[t]; break; }
    }
    if (topo && topo->net) {
        int prev_id = -1;
        EmergentPOS* ep = (gw->prefrontal && gw->prefrontal->controller)
                          ? gw->prefrontal->controller->emergent_pos : NULL;
        _learn_tokens(topo, task->msg, &prev_id, ep);
        auto_learn_concepts(gw->topology, task->msg, NULL);
        if (gw->perception) perception_feed_learn_text(gw->perception, task->msg);
        __sync_fetch_and_add(&gw->total_learning_cycles, 1);
        /* 语法种子注入 */
        SubTopology* tpl = NULL;
        for (int t = 0; t < gw->topology->sub_topo_count; t++)
            if (gw->topology->sub_topologies[t] && gw->topology->sub_topologies[t]->type == TOPO_TEMPLATE)
                { tpl = gw->topology->sub_topologies[t]; break; }
        if (tpl && tpl->net && tpl->net->node_count < 8) broca_seed_grammar(gw->topology);
    }
    free(task->msg);
    free(task);
    return NULL;
}


// POST /learn - 主动学习
static void handle_learn(GatewaySystem* gw, int fd, const char* body) {
    char msg[2048] = {0};
    if (!json_extract_string(body, "msg", msg, sizeof(msg)) || strlen(msg) == 0) {
        http_json(fd, 400, "{\"error\":\"missing or empty 'msg' field\"}");
        return;
    }

    /* 限流：每秒最多500次，burst=500 */
    time_t now = time(NULL);
    if (now == gw->last_learn_time) {
        if (++gw->learn_burst > 500) {
            http_json(fd, 429, "{\"error\":\"rate limit\"}");
            return;
        }
    } else {
        gw->last_learn_time = now;
        gw->learn_burst = 0;
    }

    /* 异步学习：spawn detach thread, 立即返回 */
    LearnTask* task = (LearnTask*)calloc(1, sizeof(LearnTask));
    if (!task) { http_json(fd, 500, "{\"error\":\"oom\"}"); return; }
    task->msg = strdup(msg);
    if (!task->msg) { free(task); http_json(fd, 500, "{\"error\":\"oom\"}"); return; }
    json_extract_string(body, "domain", task->domain, sizeof(task->domain));

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, _learn_worker, task) != 0) {
        free(task->msg); free(task);
        http_json(fd, 500, "{\"error\":\"thread failed\"}");
        return;
    }
    pthread_attr_destroy(&attr);

    http_json(fd, 202, "{\"result\":\"accepted\"}");
}

// POST /feedback - 反馈
static void handle_feedback(GatewaySystem* gw, int fd, const char* body) {
    char msg[2048] = {0};
    char rating[64] = {0};

    if (!json_extract_string(body, "msg", msg, sizeof(msg)) || strlen(msg) == 0) {
        http_json(fd, 400, "{\"error\":\"missing 'msg' field\"}");
        return;
    }
    if (!json_extract_string(body, "rating", rating, sizeof(rating)) || strlen(rating) == 0) {
        http_json(fd, 400, "{\"error\":\"missing 'rating' field (correct/wrong)\"}");
        return;
    }

    // 处理反馈
    float confidence = 0.5f;
    if (strcmp(rating, "correct") == 0 || strcmp(rating, "对") == 0) {
        confidence = 0.95f;
    } else if (strcmp(rating, "wrong") == 0 || strcmp(rating, "错") == 0) {
        confidence = 0.2f;
    } else {
        http_json(fd, 400, "{\"error\":\"rating must be 'correct' or 'wrong'\"}");
        return;
    }

    // 存入记忆
    char key[512];
    snprintf(key, sizeof(key), "feedback:%s", msg);
    memory_store(gw->memory, key, (void*)rating, strlen(rating) + 1, MEMORY_TYPE_STRING, confidence);

    http_json(fd, 200, "{\"result\":\"ok\"}");
}

// ==================== v0.5 多模态媒体投喂 API ====================

// POST /media/feed - 入队视频文件到视觉皮层任务队列
static void handle_media_feed(GatewaySystem* gw, int fd, const char* body) {
    char path[1024] = {0};
    char mode[64] = "visual";  /* 默认: 视觉皮层对齐模式 */

    if (!json_extract_string(body, "path", path, sizeof(path)) || strlen(path) == 0) {
        http_json(fd, 400, "{\"error\":\"missing 'path' field\"}");
        return;
    }
    json_extract_string(body, "mode", mode, sizeof(mode));

    /* v0.5.1: 诊断模式 — 列出视频的所有轨道信息 */
    if (strcmp(mode, "diagnose") == 0) {
        if (!gw->visual_cortex) {
            http_json(fd, 500, "{\"error\":\"visual cortex not initialized\"}");
            return;
        }
        MediaReader* mr = visual_cortex_get_media_reader(gw->visual_cortex);
        if (!mr) {
            http_json(fd, 500, "{\"error\":\"media reader not available\"}");
            return;
        }
        int tracks = media_diagnose_tracks(mr, path);
        char resp[512];
        snprintf(resp, sizeof(resp),
                 "{\"result\":\"%s\",\"tracks\":%d,\"has_subtitle\":%s}",
                 (tracks > 0) ? "ok" : "no_tracks",
                 tracks,
                 (tracks > 0) ? "check_server_stdout" : "none");
        http_json(fd, 200, resp);
        return;
    }

    if (!gw->visual_cortex) {
        http_json(fd, 500, "{\"error\":\"visual cortex not initialized\"}");
        return;
    }

    int enqueued = 0;

    /* 检查是否为目录 */
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        char recursive[8] = "0";
        int rec = 0;
        if (json_extract_string(body, "recursive", recursive, sizeof(recursive))) {
            rec = (strcmp(recursive, "1") == 0 || strcmp(recursive, "true") == 0) ? 1 : 0;
        }
        enqueued = visual_cortex_enqueue_directory(gw->visual_cortex, path, NULL, rec);
    } else {
        int ret = visual_cortex_enqueue(gw->visual_cortex, path, mode);
        if (ret == 0) enqueued = 1;
    }

    int queue_size = visual_cortex_queue_size(gw->visual_cortex);

    char resp[512];
    snprintf(resp, sizeof(resp),
             "{\"result\":\"enqueued\",\"mode\":\"%s\",\"enqueued\":%d,\"queue_size\":%d}",
             mode, enqueued, queue_size);
    http_json(fd, (enqueued > 0) ? 200 : 500, resp);
}

// GET /media/status - 查询多模态管道状态
static void handle_media_status(GatewaySystem* gw, int fd) {
    char resp[1024];

    int qsize = 0;
    long vc_frames = 0;
    int vc_vis = 0, vc_xmod = 0;
    if (gw->visual_cortex)
        visual_cortex_get_stats(gw->visual_cortex, &qsize, &vc_frames, &vc_vis, &vc_xmod);

    /* 内部 MediaReader 统计 */
    long mr_files = 0, mr_lines = 0, mr_words = 0;
    if (gw->visual_cortex) {
        MediaReader* mr = visual_cortex_get_media_reader(gw->visual_cortex);
        if (mr) media_reader_get_stats(mr, &mr_files, &mr_lines, &mr_words);
    }

    /* 视觉拓扑统计 */
    int vis_topo_nodes = 0;
    SubTopology* vt = master_get_sub_topology_by_type(gw->topology, TOPO_VISUAL);
    if (vt && vt->net) vis_topo_nodes = vt->net->node_count;

    snprintf(resp, sizeof(resp),
             "{\"queue_size\":%d,"
             "\"media_reader\":{\"files\":%ld,\"lines\":%ld,\"words\":%ld},"
             "\"visual_cortex\":{\"frames\":%ld,\"visual_nodes\":%d,\"cross_modal_edges\":%d,\"topo_visual_nodes\":%d}}",
             qsize,
             mr_files, mr_lines, mr_words,
             vc_frames, vc_vis, vc_xmod, vis_topo_nodes);
    http_json(fd, 200, resp);
}

// GET /status - 状态查询
static void handle_status(GatewaySystem* gw, int fd) {
    int total_nodes = 0;
    int template_nodes = 0;
    for (int t = 0; t < gw->topology->sub_topo_count; t++) {
        if (gw->topology->sub_topologies[t] && gw->topology->sub_topologies[t]->net)
            total_nodes += gw->topology->sub_topologies[t]->net->node_count;
    }
    SubTopology* tpl = master_get_sub_topology_by_type(gw->topology, TOPO_TEMPLATE);
    if (tpl && tpl->net) template_nodes = tpl->net->node_count;

    long long uptime = (long long)(time(NULL) - gw->start_time);
    char real_time_buf[32];
    const char* real_time = "unknown";
    if (gw->brainstem) {
        real_time = brainstem_get_real_time(gw->brainstem, real_time_buf, sizeof(real_time_buf));
    }
    int clock_ticks = gw->brainstem ? brainstem_tick_count(gw->brainstem) : 0;
    float circadian = gw->brainstem ? brainstem_get_circadian(gw->brainstem) : 0.5f;
    const char* circadian_phase = gw->brainstem ? brainstem_get_circadian_phase(gw->brainstem) : "unknown";
    long cache_frozen = gw->brain_cache ? gw->brain_cache->total_freezes : 0;
    long cache_thawed = gw->brain_cache ? gw->brain_cache->total_thaws : 0;

    char json[2048];
    snprintf(json, sizeof(json),
        "{"
        "\"status\":\"running\","
        "\"real_time\":\"%s\","
        "\"uptime\":%lld,"
        "\"clock_ticks\":%d,"
        "\"circadian\":%.2f,"
        "\"circadian_phase\":\"%s\","
        "\"dialogs\":%lld,"
        "\"learn_calls\":%lld,"
        "\"total_nodes\":%d,"
        "\"template_nodes\":%d,"
        "\"template_voting\":%s,"
        "\"brain_frozen\":%ld,"
        "\"brain_thawed\":%ld,"
        "\"topologies\":%d,"
        "\"port\":%d,"
        "\"version\":\"%s\""
        "}",
        real_time ? real_time : "unknown",
        uptime,
        clock_ticks,
        (double)circadian,
        circadian_phase,
        (long long)gw->total_dialogs,
        (long long)gw->total_learning_cycles,
        total_nodes,
        template_nodes,
        gw->topology->use_template_voting ? "true" : "false",
        cache_frozen,
        cache_thawed,
        gw->topology->sub_topo_count,
        gw->port, PIVOTMIND_VERSION);

    http_json(fd, 200, json);
}

// GET / - 仪表盘首页
static void handle_root(GatewaySystem* gw, int fd) {
    (void)gw;
    const char* html =
        "<!DOCTYPE html><html lang=zh-CN><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\"><title>玄枢 PivotMind</title><style>*{margin:0;padding:0;box-sizing:border-box}"
        "body{background:#0c1220;color:#cbd5e1;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI','PingFang SC','Microsoft YaHei',sans-serif;padding:20px;min-height:100vh}"
        "h1{font-size:20px;font-weight:400;color:#48dbfb;letter-spacing:3px;margin-bottom:2px}"
        ".sub{color:#475569;font-size:12px;margin-bottom:28px;letter-spacing:1px}"
        ".gw{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;margin-bottom:16px}"
        ".rw{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:16px}"
        ".cd{background:#131d2e;border:1px solid #1e2d45;border-radius:8px;padding:14px}"
        ".lb{font-size:10px;color:#475569;margin-bottom:5px;letter-spacing:1px;text-transform:uppercase}"
        ".vl{font-size:22px;font-weight:600;color:#e2e8f0}"
        ".gr{color:#22c55e}.cy{color:#22d3ee}.yw{color:#eab308}.bl{color:#60a5fa}"
        ".dt{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:5px;background:#22c55e}"
        ".tg{display:inline-block;padding:2px 10px;border-radius:14px;font-size:11px;background:#1e2d45;color:#48dbfb;border:1px solid #2a3f5a}"
        ".br{height:3px;border-radius:2px;background:#1e2d45;margin:8px 0 5px;overflow:hidden}"
        ".fl{height:100%;border-radius:2px;background:#48dbfb;transition:width .8s}"
        ".sg{display:grid;grid-template-columns:1fr 1fr;gap:5px;margin-top:5px}"
        ".si{text-align:center;padding:5px;background:rgba(0,0,0,.25);border-radius:5px}"
        ".sn{font-size:16px;font-weight:600}"
        ".sl{font-size:9px;color:#475569;margin-top:2px;letter-spacing:.5px}"
        ".er{color:#ef4444;font-size:11px;text-align:center;margin-top:12px;word-break:break-all}"
        "@media(max-width:640px){body{padding:10px}.rw{grid-template-columns:1fr}}</style></head><body>"
        "<h1>玄枢</h1><div class=sub>PivotMind v" PIVOTMIND_VERSION "</div>"
        "<div class=gw id=ca></div>"
        "<div class=rw>"
        "<div class=cd><div class=lb>学习调度器</div><div id=s><div class=cy vl>加载中...</div></div></div>"
        "<div class=cd><div class=lb>训练模式</div><div id=t><div class=cy vl>未激活</div></div></div>"
        "</div>"
        "<div class=rw>"
        "<div class=cd><div class=lb>脑区索引</div><div id=b><div class=cy vl>加载中...</div></div></div>"
        "<div class=cd><div class=lb>知识拓扑</div><div id=p><div class=cy vl>加载中...</div></div></div>"
        "</div>"
        "<div id=er class=er></div>"
        "<script>"
        "var er=document.getElementById('er');"
        "function $(i,h){var e=document.getElementById(i);if(e)e.innerHTML=h}"
        "function L(){"
        "fetch('/status').then(function(r){return r.json()}).then(function(s){"
        "$('ca','<div class=cd><div class=lb>状态</div><div class=vl><span class=dt></span>'+s.status+'</div></div>'"
        "+'<div class=cd><div class=lb>运行</div><div class=vl>'+Math.floor(s.uptime/3600)+'h '+Math.floor(s.uptime%3600/60)+'m</div></div>'"
        "+'<div class=cd><div class=lb>节点</div><div class=\"vl gr\">'+s.total_nodes.toLocaleString()+'</div></div>'"
        "+'<div class=cd><div class=lb>版本</div><div class=\"vl bl\">'+s.version+'</div></div>');"
        "$('p','<div class=sg>'"
        "+'<div class=si><div class=\"sn gr\">'+s.template_nodes+'</div><div class=sl>模板</div></div>'"
        "+'<div class=si><div class=\"sn bl\">'+s.topologies+'</div><div class=sl>拓扑层</div></div>'"
        "+'<div class=si><div class=\"sn cy\">'+s.brain_frozen+'</div><div class=sl>冷冻</div></div>'"
        "+'<div class=si><div class=\"sn yw\">'+s.brain_thawed+'</div><div class=sl>解冻</div></div></div>');"
        "$('er','');"
        "}).catch(function(){});"
        "fetch('/scheduler').then(function(r){return r.json()}).then(function(c){"
        "var bw=c.self_learn_mods>0?100:10;"
        "$('s','<span class=tg>'+(c.phase||'-')+'</span> <span style=font-size:11px;color:#475569>'+c.phase_elapsed_s+'s</span>'"
        "+'<div class=br><div class=fl style=width:'+bw+'%></div></div>'"
        "+'<div class=sg>'"
        "+'<div class=si><div class=\"sn gr\">'+(c.total_loops||0)+'</div><div class=sl>闭环</div></div>'"
        "+'<div class=si><div class=\"sn cy\">'+(c.self_learn_cycles||0)+'</div><div class=sl>周期</div></div>'"
        "+'<div class=si><div class=\"sn yw\">'+(c.self_learn_mods||0)+'</div><div class=sl>修正</div></div>'"
        "+'<div class=si><div class=\"sn bl\">'+(c.eval_freeze_candidates||0)+'</div><div class=sl>候选</div></div></div>');"
        "}).catch(function(){});"
        "fetch('/train/status').then(function(r){return r.json()}).then(function(t){"
        "if(t.state==='idle'||t.state==='completed'){"
        "$('t','<span class=tg>'+(t.state||'idle')+'</span> <span style=color:#475569;font-size:12px>已喂 '+(t.total_fed||0)+' 条</span>');"
        "}else{"
        "var pct=t.total_lines>0?Math.min(100,(t.current_line/t.total_lines*100)):0;"
        "$('t','<span class=tg>'+(t.state||'?')+'</span> <span style=font-size:11px;color:#475569>第'+(t.current_round||0)+'/'+(t.total_rounds||1)+'轮</span>'"
        "+'<div class=br><div class=fl style=width:'+pct+'%></div></div>'"
        "+'<div style=font-size:18px;font-weight:600;color:#22c55e;margin:4px 0>'+pct+'%</div>'"
        "+'<div class=sg>'"
        "+'<div class=si><div class=\"sn gr\">'+(t.total_added_nodes||0)+'</div><div class=sl>新节点</div></div>'"
        "+'<div class=si><div class=\"sn yw\">'+(t.total_added_edges||0)+'</div><div class=sl>新边</div></div></div>');"
        "}}).catch(function(){});"
        "fetch('/brain').then(function(r){return r.json()}).then(function(b){"
        "if(b.error){$('b','<span style=color:#475569;font-size:13px>未激活</span>');}else{"
        "$('b','<div class=sg>'"
        "+'<div class=si><div class=\"sn gr\">'+(b.entries||0)+'</div><div class=sl>已分类</div></div>'"
        "+'<div class=si><div class=\"sn cy\">'+(b.updates||0)+'</div><div class=sl>EMA</div></div>'"
        "+'<div class=si><div class=\"sn yw\">'+(b.migrations||0)+'</div><div class=sl>迁移</div></div>'"
        "+'<div class=si><div class=\"sn bl\">9+1</div><div class=sl>脑区</div></div></div>');"
        "}}).catch(function(){});"
        "setTimeout(L,5000)}"
        "L()"
        "</script></body></html>"
    ;
    http_send(fd, 200, "text/html; charset=utf-8", html);
}

// GET /scheduler - 学习调度器状态
static void handle_scheduler(GatewaySystem* gw, int fd) {
    if (!gw->scheduler) {
        http_json(fd, 404, "{\"error\":\"scheduler not initialized\"}");
        return;
    }

    const char* phase_name = "idle";
    int total_loops = 0;
    int sel_cycles = 0, sel_mods = 0;
    int batch_nodes = 0, batch_edges = 0;
    int eval_candidates = 0;
    long phase_elapsed = 0;

    learning_scheduler_get_stats(gw->scheduler,
        &total_loops, &sel_cycles, &sel_mods,
        &batch_nodes, &batch_edges, &eval_candidates,
        &phase_name, &phase_elapsed);

    char json[1024];
    snprintf(json, sizeof(json),
        "{"
        "\"phase\":\"%s\","
        "\"phase_elapsed_s\":%ld,"
        "\"total_loops\":%d,"
        "\"self_learn_cycles\":%d,\"self_learn_mods\":%d,"
        "\"batch_nodes\":%d,\"batch_edges\":%d,"
        "\"eval_freeze_candidates\":%d"
        "}",
        phase_name, phase_elapsed,
        total_loops, sel_cycles, sel_mods,
        batch_nodes, batch_edges, eval_candidates);

    http_json(fd, 200, json);
}

// GET /scheduler/stats - 自学习器详细统计
static void handle_scheduler_self_stats(GatewaySystem* gw, int fd) {
    if (!gw->scheduler) {
        http_json(fd, 404, "{\"error\":\"scheduler not initialized\"}");
        return;
    }

    LearningPhase phase = learning_scheduler_get_phase(gw->scheduler);
    (void)phase;

    // 从 self_learner 获取统计
    // （通过 scheduler_get_stats 已提供主要数据，此处为兼容更详细的未来扩展）
    http_json(fd, 200, "{\"detail\":\"use /scheduler for summary\"}");
}

// GET /health - 健康检查
static void handle_health(GatewaySystem* gw, int fd) {
    if (gw->engine_ready) {
        http_json(fd, 200, "{\"status\":\"ok\"}");
    } else {
        http_json(fd, 503, "{\"status\":\"loading\",\"message\":\"engine initializing\"}");
    }
}

// ==================== HTTP 请求解析 ====================

typedef struct {
    char method[8];
    char path[256];
    char body[GW_MAX_REQUEST];
    int  body_len;
} HttpRequest;

static int parse_request(int fd, HttpRequest* req) {
    memset(req, 0, sizeof(HttpRequest));

    // 读取请求 (chunked read, 每轮 4KB)
    char buf[GW_MAX_REQUEST];
    int total = 0;
    int header_end = -1;

    // 设读取超时
    struct timeval tv = { .tv_sec = GW_READ_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (total < (int)sizeof(buf) - 1) {
        int chunk = (int)sizeof(buf) - 1 - total;
        if (chunk > 4096) chunk = 4096;  /* 一次读 4KB, 非逐字节 */
        int n = recv(fd, buf + total, chunk, 0);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';

        // 检查 header 结束 (\r\n\r\n)
        if (header_end < 0 && total >= 4) {
            for (int i = 0; i <= total - 4; i++) {
                if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') {
                    header_end = i;
                    break;
                }
            }
        }

        // header 已读完，检查 Content-Length 继续读 body
        if (header_end >= 0) {
            int header_size = header_end + 4;
            int body_received = total - header_size;

            // 找 Content-Length
            char* cl = strcasestr(buf, "Content-Length:");
            if (cl) {
                int content_length = atoi(cl + 15);
                if (content_length > GW_MAX_REQUEST - 4096) return -1; /* 保护: 留出 header 空间 */
                if (body_received >= content_length) break; // body 完整
            } else {
                break; // 无 body
            }
        }
    }

    if (total == 0 || header_end < 0) return -1;

    // 解析方法
    char* p = buf;
    int i = 0;
    while (*p && *p != ' ' && i < (int)sizeof(req->method) - 1) req->method[i++] = *p++;
    req->method[i] = '\0';
    if (*p == ' ') p++;

    // 解析路径
    i = 0;
    while (*p && *p != ' ' && *p != '?' && i < (int)sizeof(req->path) - 1) req->path[i++] = *p++;
    req->path[i] = '\0';

    // 提取 body (带边界保护)
    int header_size = header_end + 4;
    req->body_len = total - header_size;
    if (req->body_len > 0 && req->body_len < (int)sizeof(req->body)) {
        memcpy(req->body, buf + header_size, req->body_len);
        req->body[req->body_len] = '\0';
    } else if (req->body_len >= (int)sizeof(req->body)) {
        return -1;  /* body 溢出 */
    }

    return 0;
}

// ==================== 连接处理 ====================

static void handle_connection(GatewaySystem* gw, int client_fd) {
    /* 加固: TCP keepalive 检测死连接 */
    int ka = 1;
    setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));
    /* 加固: 发送超时 10s */
    struct timeval stv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof(stv));

    HttpRequest req;
    if (parse_request(client_fd, &req) < 0) {
        http_json(client_fd, 400, "{\"error\":\"bad request\"}");
        close(client_fd);
        return;
    }

    // CORS preflight
    if (strcmp(req.method, "OPTIONS") == 0) {
        http_send(client_fd, 200, "text/plain", "");
        close(client_fd);
        return;
    }

    // 路由
    if (strcmp(req.method, "GET") == 0) {
        if (strcmp(req.path, "/") == 0 || strcmp(req.path, "/dashboard") == 0) {
            handle_root(gw, client_fd);
        } else if (strcmp(req.path, "/health") == 0) {
            handle_health(gw, client_fd);
        } else if (strcmp(req.path, "/qa") == 0) {
            char rs[64]; snprintf(rs, 64, "{\"count\":%d}", gw->qa_memory ? qa_memory_count(gw->qa_memory) : 0);
            http_json(client_fd, 200, rs);
        } else if (strcmp(req.path, "/status") == 0) {
            if (!gw->engine_ready) {
                http_json(client_fd, 503, "{\"status\":\"loading\"}");
            } else {
                handle_status(gw, client_fd);
            }
        } else if (strcmp(req.path, "/train/status") == 0) {
            if (!gw->train_mode) {
                http_json(client_fd, 404, "{\"error\":\"train mode not enabled\"}");
            } else {
                TrainProgress p = train_mode_get_progress(gw->train_mode);
                const char* st = p.state==TRAIN_RUNNING?"running":p.state==TRAIN_PAUSED?"paused":p.state==TRAIN_COMPLETED?"completed":"idle";
                char tr[512];
                snprintf(tr, sizeof(tr), "{\"state\":\"%s\",\"current_round\":%d,\"total_rounds\":%d,\"current_line\":%ld,\"total_lines\":%ld,\"total_fed\":%ld,\"total_added_nodes\":%ld,\"total_added_edges\":%ld}",
                    st, p.current_round, p.total_rounds, p.current_line, p.total_lines,
                    p.total_fed, p.total_added_nodes, p.total_added_edges);
                http_json(client_fd, 200, tr);
            }
        } else if (strcmp(req.path, "/scheduler") == 0) {
            handle_scheduler(gw, client_fd);
        } else if (strcmp(req.path, "/scheduler/stats") == 0) {
            handle_scheduler_self_stats(gw, client_fd);
        } else if (strcmp(req.path, "/brain") == 0) {
            if (gw->topo_brain) {
                int entries, updates, migrations;
                topobrain_get_stats(gw->topo_brain, &entries, &updates, &migrations);
                char bj[256];
                snprintf(bj, sizeof(bj), "{\"entries\":%d,\"updates\":%d,\"migrations\":%d}",
                         entries, updates, migrations);
                http_json(client_fd, 200, bj);
            } else {
                http_json(client_fd, 404, "{\"error\":\"brain not initialized\"}");
            }
        } else if (strcmp(req.path, "/media/status") == 0) {
            handle_media_status(gw, client_fd);
        } else if (strcmp(req.path, "/debug") == 0) {
            /* 调试端点 */
            SubTopology* vocab = NULL;
            for (int t = 0; t < gw->topology->sub_topo_count; t++) {
                if (gw->topology->sub_topologies[t] && gw->topology->sub_topologies[t]->type == TOPO_VOCABULARY) {
                    vocab = gw->topology->sub_topologies[t]; break;
                }
            }
            int vnodes = (vocab && vocab->net) ? vocab->net->node_count : 0;
            int fentries = (gw->topology->freq_table) ? gw->topology->freq_table->entry_count : -1;
            int tnodes = 0, total = 0;
            for (int t = 0; t < gw->topology->sub_topo_count; t++) {
                if (gw->topology->sub_topologies[t] && gw->topology->sub_topologies[t]->net) {
                    int n = gw->topology->sub_topologies[t]->net->node_count;
                    total += n;
                    if (gw->topology->sub_topologies[t]->type == TOPO_TEMPLATE) tnodes = n;
                }
            }
            char dbg[512];
            snprintf(dbg, sizeof(dbg),
                "{\"vocab_nodes\":%d,\"freq_entries\":%d,"
                "\"total_nodes\":%d,\"template_nodes\":%d,\"sub_topos\":%d}",
                vnodes, fentries, total, tnodes, gw->topology->sub_topo_count);
            http_json(client_fd, 200, dbg);
        } else if (strcmp(req.path, "/force_templates") == 0) {
            int built = broca_build_templates(gw->topology, 10, 4);
            char rsp[128];
            snprintf(rsp, sizeof(rsp), "{\"built\":%d}", built);
            http_json(client_fd, 200, rsp);
            fprintf(stderr, "[gateway] 强制模板构建: %d\n", built);
        } else {
            http_json(client_fd, 404, "{\"error\":\"not found\"}");
        }
    } else if (strcmp(req.method, "POST") == 0) {
        if (!gw->engine_ready) {
            http_json(client_fd, 503, "{\"status\":\"loading\",\"message\":\"engine initializing\"}");
        } else if (strcmp(req.path, "/chat") == 0) {
            handle_chat(gw, client_fd, req.body);
        } else if (strcmp(req.path, "/qa") == 0) {
            handle_qa(gw, client_fd, req.body);
        } else if (strcmp(req.path, "/learn") == 0) {
            handle_learn(gw, client_fd, req.body);
        } else if (strcmp(req.path, "/feedback") == 0) {
            handle_feedback(gw, client_fd, req.body);
        } else if (strcmp(req.path, "/media/feed") == 0) {
            handle_media_feed(gw, client_fd, req.body);
        } else if (strncmp(req.path, "/train/", 7) == 0) {
            if (!gw->train_mode) {
                http_json(client_fd, 404, "{\"error\":\"train mode not enabled\"}");
            } else if (strcmp(req.path, "/train/status") == 0) {
                TrainProgress p = train_mode_get_progress(gw->train_mode);
                const char* st = p.state==TRAIN_RUNNING?"running":p.state==TRAIN_PAUSED?"paused":p.state==TRAIN_COMPLETED?"completed":"idle";
                char tr[512];
                snprintf(tr, sizeof(tr), "{\"state\":\"%s\",\"current_round\":%d,\"total_rounds\":%d,\"current_line\":%ld,\"total_lines\":%ld,\"total_fed\":%ld,\"total_added_nodes\":%ld,\"total_added_edges\":%ld}",
                    st, p.current_round, p.total_rounds, p.current_line, p.total_lines,
                    p.total_fed, p.total_added_nodes, p.total_added_edges);
                http_json(client_fd, 200, tr);
            } else if (strcmp(req.path, "/train/pause") == 0) {
                train_mode_pause(gw->train_mode);
                http_json(client_fd, 200, "{\"result\":\"paused\"}");
            } else if (strcmp(req.path, "/train/resume") == 0) {
                train_mode_resume(gw->train_mode);
                http_json(client_fd, 200, "{\"result\":\"resumed\"}");
            } else if (strcmp(req.path, "/train/stop") == 0) {
                train_mode_stop(gw->train_mode);
                http_json(client_fd, 200, "{\"result\":\"stopped\"}");
            } else if (strcmp(req.path, "/train/start") == 0) {
                TrainProgress p = train_mode_get_progress(gw->train_mode);
                if (p.state == TRAIN_RUNNING) {
                    http_json(client_fd, 400, "{\"error\":\"already running\"}");
                } else if (p.state == TRAIN_IDLE || p.state == TRAIN_COMPLETED || p.state == TRAIN_ERROR) {
                    int ret = train_mode_start(gw->train_mode);
                    if (ret == 0) {
                        http_json(client_fd, 200, "{\"result\":\"started\"}");
                    } else {
                        http_json(client_fd, 500, "{\"error\":\"start failed\"}");
                    }
                } else {
                    http_json(client_fd, 400, "{\"error\":\"cannot start in current state\"}");
                }
            } else {
                http_json(client_fd, 404, "{\"error\":\"unknown train command\"}");
            }
        } else {
            http_json(client_fd, 404, "{\"error\":\"not found\"}");
        }
    } else {
        http_json(client_fd, 400, "{\"error\":\"method not allowed\"}");
    }

    close(client_fd);
}

// ==================== 主函数 ====================

static void handle_qa(GatewaySystem* gw, int fd, const char* body) {
    if (!gw->qa_memory) { gw->qa_memory = qa_memory_create(NULL, 500000); }
    if (!gw->qa_memory) { http_json(fd, 500, "{\"error\":\"qa failed\"}"); return; }
    int added = 0; const char* p = body;
    while (p && *p) {
        const char* qk = strstr(p, "\"q\""); if (!qk) break;
        const char* qv = strchr(qk+3,':'); if(!qv)break; qv=strchr(qv,'"'); if(!qv)break; qv++;
        const char* qe = strchr(qv,'"'); if(!qe)break;
        const char* ak = strstr(qe,"\"a\""); if(!ak)break;
        const char* av = strchr(ak+3,':'); if(!av)break; av=strchr(av,'"'); if(!av)break; av++;
        const char* ae = strchr(av,'"'); if(!ae)break;
        char q[1024]={0},a[1024]={0}; int ql=qe-qv,al=ae-av;
        if(ql>0&&ql<1024&&al>0&&al<1024){memcpy(q,qv,ql);memcpy(a,av,al); if(qa_memory_add(gw->qa_memory,q,a)==0)added++;}
        p=ae+1;
    }
    char rs[128]; snprintf(rs,128,"{\"result\":\"ok\",\"added\":%d,\"total\":%d}",added,qa_memory_count(gw->qa_memory));
    http_json(fd,200,rs);
}


int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IOLBF, 0);  /* 行缓冲，确保所有线程日志即时可见 */
    printf("[gateway] PivotMind v%s\n", PIVOTMIND_VERSION);
    // 解析参数
    int port = GW_DEFAULT_PORT;
    int train_mode_flag = 0;
    TrainConfig train_config = {NULL, CORPUS_JSON_QA, 1, 20, 100, 5000, 0};
    int format_explicit = 0;     /* v0.5: 用户显式指定了 --format */
    const char* workdir = ".";

    // 解析命令行参数（兼容旧的位置参数和新的 --train-mode 选项）
    // 先解析 --train-mode 和训练相关参数
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--train-mode") == 0) {
            train_mode_flag = 1;
            if (!train_config.corpus_path)
                train_config.corpus_path = "data/hermes_knowledge_base.json";
        } else if (strcmp(argv[i], "--corpus") == 0 && i+1 < argc) {
            train_config.corpus_path = argv[++i];
        } else if (strcmp(argv[i], "--rounds") == 0 && i+1 < argc) {
            train_config.rounds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--speed") == 0 && i+1 < argc) {
            train_config.speed = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--batch") == 0 && i+1 < argc) {
            train_config.batch_learn_interval = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--format") == 0 && i+1 < argc) {
            const char* fmt = argv[++i];
            format_explicit = 1;  /* 用户显式指定格式 */
            if (strcmp(fmt, "pipe") == 0) train_config.format = CORPUS_PIPE_QA;
            else if (strcmp(fmt, "text") == 0 || strcmp(fmt, "plain") == 0) train_config.format = CORPUS_PLAIN_TEXT;
            else if (strcmp(fmt, "article") == 0) train_config.format = CORPUS_ARTICLE;
            else if (strcmp(fmt, "media") == 0 || strcmp(fmt, "video") == 0) train_config.format = CORPUS_MEDIA;
            else train_config.format = CORPUS_JSON_QA;
        } else if (strcmp(argv[i], "--save-interval") == 0 && i+1 < argc) {
            train_config.save_interval = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            train_config.verbose = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("用法: pivotmind_gateway [port] [workdir] [--train-mode] [选项]\n");
            train_config_print_defaults();
            return 0;
        } else if (argv[i][0] != '-') {
            // 兼容旧的位置参数
            if (port == GW_DEFAULT_PORT && atoi(argv[i]) > 0)
                port = atoi(argv[i]);
            else
                workdir = argv[i];
        }
    }
    // 自动检测语料格式 (仅在用户未显式指定 --format 时)
    if (train_config.corpus_path && !format_explicit)
        train_config.format = train_detect_format(train_config.corpus_path);

    // 切换工作目录
    if (chdir(workdir) != 0) {
        fprintf(stderr, "[gateway] 无法切换到工作目录: %s (%s)\n", workdir, strerror(errno));
        return 1;
    }

    // 创建系统
    GatewaySystem gw = {0};
    gw.train_mode_flag = train_mode_flag;
    gw.train_config = train_config;
    g_gw = &gw;
    gw.port = port;
    strncpy(gw.workdir, workdir, sizeof(gw.workdir) - 1);

    /* load runtime config (optional; defaults if file missing) */
    gw.config = config_load(NULL);

    // 信号处理
    signal(SIGINT, gw_signal_handler);
    signal(SIGTERM, gw_signal_handler);
    signal(SIGPIPE, SIG_IGN); // 忽略断开连接的写

    { int pf = socket(AF_INET, SOCK_STREAM, 0); if (pf >= 0) { struct sockaddr_in pa = {.sin_family=AF_INET, .sin_addr.s_addr=inet_addr("127.0.0.1"), .sin_port=htons(port)}; if (connect(pf, (struct sockaddr*)&pa, sizeof(pa)) == 0) { fprintf(stderr, "[gateway] 端口 %d 已被占用，拒绝启动。\n", port); close(pf); return 1; } close(pf); } }
    { int pf = socket(AF_INET, SOCK_STREAM, 0); if (pf >= 0) { struct sockaddr_in pa = {.sin_family=AF_INET, .sin_addr.s_addr=inet_addr("127.0.0.1"), .sin_port=htons(port)}; if (connect(pf, (struct sockaddr*)&pa, sizeof(pa)) == 0) { fprintf(stderr, "[gateway] 端口 %d 已被占用，拒绝启动。\n", port); close(pf); return 1; } close(pf); } }
    { int pf = socket(AF_INET, SOCK_STREAM, 0); if (pf >= 0) { struct sockaddr_in pa = {.sin_family=AF_INET, .sin_addr.s_addr=inet_addr("127.0.0.1"), .sin_port=htons(port)}; if (connect(pf, (struct sockaddr*)&pa, sizeof(pa)) == 0) { fprintf(stderr, "[gateway] 端口 %d 已被占用，拒绝启动。\n", port); close(pf); return 1; } close(pf); } }
    // 创建监听 socket (先绑定端口，再初始化引擎，避免加载期间 SSH 连不上)
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        fprintf(stderr, "[gateway] socket 创建失败: %s\n", strerror(errno));
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[gateway] bind 失败: %s\n", strerror(errno));
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, GW_BACKLOG) < 0) {
        fprintf(stderr, "[gateway] listen 失败: %s\n", strerror(errno));
        close(server_fd);
        return 1;
    }

    { FILE* pf = fopen("/tmp/pivotmind.port", "w"); if (pf) { fprintf(pf, "%d", port); fclose(pf); } }
    { FILE* pf = fopen("/tmp/pivotmind.port", "w"); if (pf) { fprintf(pf, "%d", port); fclose(pf); } }
    { FILE* pf = fopen("/tmp/pivotmind.port", "w"); if (pf) { fprintf(pf, "%d", port); fclose(pf); } }
    printf("[gateway] 端口 %d 已绑定 (引擎初始化中...)\n", port);

    // 后台线程初始化引擎 (避免阻塞主循环，加载期间仍可响应 /health)
    pthread_t init_thread;
    if (pthread_create(&init_thread, NULL, gw_system_init_thread, &gw) != 0) {
        fprintf(stderr, "[gateway] 无法创建初始化线程\n");
        close(server_fd);
        return 1;
    }
    pthread_detach(init_thread);

    // 主循环 (引擎初始化期间 /health 返回 loading，初始化完成后正常服务)
    while (!gw.shutdown_requested) {
        // 用非阻塞 accept + 短超时，避免初始化卡住时无法响应信号
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            if (errno == EINTR || gw.shutdown_requested) break;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            // 可能是超时，继续循环
            continue;
        }

        handle_connection(&gw, client_fd);
    }

    // 等待引擎初始化线程结束 (如果还在跑)
    while (!gw.engine_ready && !gw.shutdown_requested) {
        usleep(100000); // 100ms
    }

    // 清理
    close(server_fd);

    // 停止训练模式
    if (gw.train_mode) {
        // train_mode_destroy 内部会调 train_mode_stop，不重复调
        train_mode_destroy(gw.train_mode);
        gw.train_mode = NULL;
    }

    gw_system_shutdown(&gw);

    printf("[gateway] 再见!\n");
    return 0;
}
