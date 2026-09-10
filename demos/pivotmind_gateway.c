/**
 * @file pivotmind_gateway.c
 * @brief PivotMind HTTP Gateway - REST API 服务网关（v0.5.25 P2-6 拆分后主入口）
 *
 * 本文件保留: API token 鉴权 / 连接处理 / 崩溃处理 / main。
 * 其余模块已拆分至:
 *   gateway_internal.h   共享类型与原型
 *   gateway_http.c       JSON/HTTP 工具 + 请求解析
 *   gateway_system.c     系统初始化/保存/关闭
 *   gateway_learn.c      学习队列与 worker
 *   gateway_handlers.c   REST 请求处理 (handle_*)
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
 *   PIVOTMIND_BIND_ADDR=0.0.0.0 pivotmind_gateway  # 监听全网卡（默认仅 127.0.0.1）
 */

#include "gateway_internal.h"

/* P0-1: 在途连接线程计数（GCC __sync 原子操作维护），连接处理与 main 使用 */
static volatile int g_conn_count = 0;

/* 全局网关实例：learn worker 等跨模块访问（见 gateway_internal.h extern） */
GatewaySystem* g_gw = NULL;

// ==================== API token 鉴权 (C1 修复) ====================

/* 生成随机 API token: 从 /dev/urandom 读 32 字节 → 64 个十六进制字符。
 * 零依赖，失败时回退 rand() (时间+pid 播种)。 */
static void gw_generate_token(char* buf, size_t cap) {
    unsigned char raw[32];
    size_t got = 0;
    FILE* fp = fopen("/dev/urandom", "rb");
    if (fp) {
        got = fread(raw, 1, sizeof(raw), fp);
        fclose(fp);
    }
    if (got < sizeof(raw)) {
        srand((unsigned)time(NULL) ^ ((unsigned)getpid() << 16));
        for (size_t i = got; i < sizeof(raw); i++)
            raw[i] = (unsigned char)(rand() & 0xFF);
    }
    size_t o = 0;
    for (size_t i = 0; i < sizeof(raw) && o + 2 < cap; i++) {
        snprintf(buf + o, 3, "%02x", raw[i]);
        o += 2;
    }
    buf[o] = '\0';
}

/* C1+ token 持久化: API token 跨重启保持不变。
 * 随机生成后写入 GW_TOKEN_FILE (与 pivotmind_state.dat 同目录)，下次启动直接复用。
 * 树莓派黑匣子 / feed 脚本用固定 token，无需跟随网关重启动态提取。 */
static int gw_token_file_load(char* buf, size_t cap) {
    FILE* fp = fopen(GW_TOKEN_FILE, "rb");
    if (!fp) return 0;                      /* 文件不存在 → 调用方随机生成 */
    size_t n = fread(buf, 1, cap - 1, fp);
    fclose(fp);
    if (n == 0) return 0;
    buf[n] = '\0';
    /* 允许末尾换行/空白（文本文件惯例） */
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                     buf[n - 1] == ' ' || buf[n - 1] == '\t'))
        buf[--n] = '\0';
    if (n != GW_TOKEN_LEN) return 0;        /* 长度必须恰好 64 */
    for (size_t i = 0; i < n; i++)
        if (!isxdigit((unsigned char)buf[i])) return 0;  /* 必须全是 hex */
    return 1;
}

static void gw_token_file_save(const char* token) {
    FILE* fp = fopen(GW_TOKEN_FILE, "wb");
    if (!fp) {
        fprintf(stderr, "[gateway] 警告: 无法写入 token 文件 %s: %s (token 仅本次运行有效)\n",
                GW_TOKEN_FILE, strerror(errno));
        return;
    }
    fprintf(fp, "%s\n", token);
    fclose(fp);
    if (chmod(GW_TOKEN_FILE, 0600) != 0)
        fprintf(stderr, "[gateway] 警告: chmod 0600 %s 失败: %s\n", GW_TOKEN_FILE, strerror(errno));
}

// ==================== 信号处理 ====================

static void gw_signal_handler(int signum) {
    (void)signum;
    if (g_gw) {
        const char msg[] = "\n[gateway] 收到退出信号，正在关闭...\n";
        (void)!write(STDOUT_FILENO, msg, sizeof(msg) - 1);
        g_gw->shutdown_requested = 1;
    }
}

// ==================== 连接处理 ====================

static void handle_connection(GatewaySystem* gw, int client_fd);  /* P0-1: gw_conn_thread 前置声明 */

/* P0-1 修复: 每连接独立线程处理，accept 主循环不再被慢请求阻塞。
 * 旧版单线程串行 accept→handle_connection：任一连接慢速上传(recv 超时 10s)
 * 或同步推理耗时都会阻塞后续所有连接，/health 健康探测因此被"拖死"。
 * 用原子计数限制并发连接数，超限直接 503，防慢连接洪泛打穿线程资源。
 * 注意: 引擎推理函数(handle_chat 内 pfe/prefrontal/qa_memory)若未来需
 * 高并发调用，需在引擎侧确认线程安全；learn worker 已与连接处理并发，
 * 当前架构下不新增额外风险。 */
#define GW_MAX_CONN 64

typedef struct {
    GatewaySystem* gw;
    int client_fd;
} ConnArg;

static void* gw_conn_thread(void* arg) {
    ConnArg* ca = (ConnArg*)arg;
    handle_connection(ca->gw, ca->client_fd);
    free(ca);
    __sync_fetch_and_sub(&g_conn_count, 1);
    return NULL;
}

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

    /* 安全加固: 鉴权扩展到所有非健康检查端点 — 除 /health(/healthz) 外，
     * 所有端点必须携带有效 X-Pivot-Token（与现有 /media 端点同一机制，兼容现有调用方）。
     * 只保留 /health 匿名，供负载均衡/监控/启动探测。token 为空则拒绝（安全默认）。 */
    int is_health = (strcmp(req.path, "/health") == 0 ||
                     strcmp(req.path, "/healthz") == 0);
    if (!is_health &&
        (gw->api_token[0] == '\0' || !gw_token_equal(req.token, gw->api_token))) {
        http_json(client_fd, 401, "{\"error\":\"unauthorized: missing or invalid X-Pivot-Token\"}");
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

/* v0.5.12: gw_crash_handler 改为纯 async-signal-safe。
 * 背景（devlog/crash-loop-0820-analysis + 复查）：上一版虽已去掉 malloc，
 * 但仍调用 backtrace()/backtrace_symbols_fd()——尽管 man 页称 fd 版
 * async-signal-safe，实际内部走 dlopen/dladdr 解析符号，会再入动态连接器
 * 与 malloc。堆损坏时（越界写毁 free list），handler 内再入堆锁即自锁
 * 死锁，进程永久冻结，watchdog 误判"存活"而反复重启 → 无限循环。
 * 今彻底删掉 backtrace 系列：预分配 static 缓冲拼出 [CRASH] 信号行，
 * 仅 write(2) 写出，不 malloc / 不 dlopen / 不 backtrace，随后立即
 * _exit(128+sig)，绝不 return。 */
/* 仅 write(2)：循环写全一段字节，处理 EINTR/部分写 */
static void gw_safe_write_all(int fd, const char* s, size_t len) {
    while (len > 0) {
        ssize_t w = write(fd, s, len);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            break;
        }
        s += w;
        len -= (size_t)w;
    }
}
static void gw_crash_handler(int sig) {
    static char buf[128];   /* 预分配静态缓冲：信号上下文禁 malloc */
    size_t n = 0;

/* 追加一串到 buf（越界即截断，绝不过写） */
#define GW_APPEND(s) do { const char* _p = (s); \
    while (*_p && n < sizeof(buf) - 1) buf[n++] = *_p++; } while (0)

    GW_APPEND("\n[CRASH] 信号 ");
    /* 十进制信号号手工拼：禁 snprintf/stdio */
    { unsigned v = (unsigned)sig, tmp[16], i = sizeof(tmp);
      do { tmp[--i] = v % 10; v /= 10; } while (v > 0);
      while (i < sizeof(tmp) && n < sizeof(buf) - 1) buf[n++] = (char)('0' + tmp[i++]); }
    GW_APPEND(" (");
    GW_APPEND(sig == SIGSEGV ? "SIGSEGV" : sig == SIGABRT ? "SIGABRT" :
              sig == SIGILL  ? "SIGILL"  : sig == SIGFPE ? "SIGFPE"  :
              sig == SIGBUS  ? "SIGBUS"  : "?");
    GW_APPEND(")\n");
#undef GW_APPEND

    buf[n] = '\0';
    gw_safe_write_all(STDERR_FILENO, buf, n);
    _exit(128 + sig);      /* 崩溃即终止，绝不 return 继续跑 */
}

int main(int argc, char* argv[]) {
    /* v0.5.25 P2-4: 支持 PIVOTMIND_LOG_FILE=路径 将 stdout/stderr 一并落盘。
     * 用 dup2 而非 freopen：crash handler 直接 write(2, ...)，
     * fd 重定向后 [CRASH] 崩溃现场同样写入文件，配合服务托管可回溯崩溃。 */
    const char* logf = getenv("PIVOTMIND_LOG_FILE");
    if (logf && logf[0]) {
        int lfd = open(logf, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (lfd >= 0) {
            if (dup2(lfd, STDOUT_FILENO) >= 0)
                dup2(lfd, STDERR_FILENO);
            if (lfd > STDERR_FILENO) close(lfd);
        } else {
            fprintf(stderr, "[gateway] 警告: 无法打开 PIVOTMIND_LOG_FILE=%s (%s)\n",
                    logf, strerror(errno));
        }
    }
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

    /* C1: 生成 API token 并打印到日志（媒体端点鉴权）
     * C1+ 持久化: 优先复用 GW_TOKEN_FILE 中的有效 token（跨重启不变，黑匣子/feed
     * 脚本无需跟随变化）；文件不存在/内容非法则随机生成并写入 (0600)。 */
    if (!gw_token_file_load(gw.api_token, sizeof(gw.api_token))) {
        gw_generate_token(gw.api_token, sizeof(gw.api_token));
        gw_token_file_save(gw.api_token);
    }
    /* C1: 打印脱敏 token（仅前 4 后 4），完整值存于 GW_TOKEN_FILE(0600)。
     * 旧版整段明文打印到 stdout，日志若被旁路读取即泄露完整凭据。 */
    size_t tlen = strlen(gw.api_token);
    if (tlen > 8) {
        printf("[gateway] API token: %.4s...%s (完整 token 见 %s, 权限 0600)\n",
               gw.api_token, gw.api_token + tlen - 4, GW_TOKEN_FILE);
    } else {
        printf("[gateway] API token: %.4s%s (完整 token 见 %s, 权限 0600)\n",
               gw.api_token, tlen > 4 ? "..." : "", GW_TOKEN_FILE);
    }
    printf("[gateway] 除 /health 外所有端点需要请求头 X-Pivot-Token 才能访问\n");

    /* load runtime config (optional; defaults if file missing) */
    gw.config = config_load(NULL);

    // 信号处理
    signal(SIGINT, gw_signal_handler);
    signal(SIGTERM, gw_signal_handler);
    signal(SIGSEGV, gw_crash_handler);   /* v0.5.7: 崩溃定位 */
    signal(SIGABRT, gw_crash_handler);
    signal(SIGPIPE, SIG_IGN); // 忽略断开连接的写

    // 创建监听 socket (先绑定端口，再初始化引擎，避免加载期间 SSH 连不上)
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        fprintf(stderr, "[gateway] socket 创建失败: %s\n", strerror(errno));
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* 安全加固: 默认仅绑定 127.0.0.1（仅本机可访问）。
     * 需要局域网/远程访问时用环境变量 PIVOTMIND_BIND_ADDR 覆盖，例如:
     *   PIVOTMIND_BIND_ADDR=0.0.0.0 pivotmind_gateway
     * 注意: 绑 127.0.0.1 后局域网/Tailscale 直接访问会断，本机调用不受影响。 */
    in_addr_t bind_addr = inet_addr("127.0.0.1");
    const char* bind_env = getenv("PIVOTMIND_BIND_ADDR");
    if (bind_env && bind_env[0] != '\0') {
        in_addr_t env_addr = inet_addr(bind_env);
        if (env_addr == (in_addr_t)INADDR_NONE) {
            fprintf(stderr, "[gateway] 警告: PIVOTMIND_BIND_ADDR=\"%s\" 无效，回退绑定 127.0.0.1\n", bind_env);
        } else {
            bind_addr = env_addr;
        }
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = bind_addr,
        .sin_port = htons(port)
    };

    /* P2-2: 不再预探测端口。旧版 connect 探测只连 127.0.0.1，与实际 bind
     * 地址（可能 0.0.0.0/局域网 IP）不一致，既可能误报也可能漏报，且探测与
     * bind 之间天然存在 TOCTOU 竞态。直接 bind，失败按 errno 判定占用。 */
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (errno == EADDRINUSE)
            fprintf(stderr, "[gateway] 端口 %d 已被占用，拒绝启动。\n", port);
        else
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
    printf("[gateway] 端口 %d 已绑定 %s (引擎初始化中...)\n", port, inet_ntoa(addr.sin_addr));

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

        /* P0-1: 连接处理移交独立线程，主循环立即回到 accept。
         * 并发满时直接 503 拒绝并关闭，避免排队堆积。 */
        if (__sync_fetch_and_add(&g_conn_count, 1) >= GW_MAX_CONN) {
            __sync_fetch_and_sub(&g_conn_count, 1);
            http_json(client_fd, 503, "{\"error\":\"busy: too many connections\"}");
            close(client_fd);
            continue;
        }
        ConnArg* ca = (ConnArg*)malloc(sizeof(ConnArg));
        if (!ca) {
            __sync_fetch_and_sub(&g_conn_count, 1);
            http_json(client_fd, 500, "{\"error\":\"out of memory\"}");
            close(client_fd);
            continue;
        }
        ca->gw = &gw;
        ca->client_fd = client_fd;
        pthread_t ct;
        if (pthread_create(&ct, NULL, gw_conn_thread, ca) != 0) {
            __sync_fetch_and_sub(&g_conn_count, 1);
            http_json(client_fd, 500, "{\"error\":\"thread create failed\"}");
            close(client_fd);
            free(ca);
            continue;
        }
        pthread_detach(ct);
    }

    // 等待引擎初始化线程结束 (如果还在跑)
    while (!gw.engine_ready && !gw.shutdown_requested) {
        usleep(100000); // 100ms
    }

    // 清理
    close(server_fd);

    /* P0-1: 有界等待在途连接线程结束，避免 main 栈上 gw 被释放后
     * 连接线程仍在访问（detached 线程不 join，只能轮询计数）。
     * 上限 15s：慢连接 recv 超时 10s + 处理余量。 */
    for (int w = 0; w < 150 && __sync_fetch_and_add(&g_conn_count, 0) > 0; w++)
        usleep(100000);  // 100ms × 150

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
