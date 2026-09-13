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

#include "pivotmind_paths.h"
#include "gateway_internal.h"

/* 全局网关实例：learn worker 等跨模块访问（见 gateway_internal.h extern）。
 * C1/P1-3: 它指向 main 里堆上的 GatewaySystem；关闭拆解完成后必须置 NULL
 * （旧版永不置 NULL，main 返回后悬垂，学习 worker / 信号处理仍可能解引用）。 */
GatewaySystem* g_gw = NULL;

/* C1: 在途连接线程登记表——旧版只有原子计数 + pthread_detach：关闭时无处可
 * join，只能"轮询计数 15s 然后强拆 main 栈上的 gw"，慢连接线程接着访问已
 * 释放的 gw->topology 等（use-after-free）。
 * 现在每个连接线程占一个槽：线程退出时自己清 used；主循环每轮回收已结束的槽；
 * 关闭时对 ever==1 的槽逐个 join，join 返回即代表该线程彻底离开 gw。
 * 定义放在 GW_MAX_CONN 之后（见下方）。 */

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

/* C1: 连接线程登记表（全部读写都在 g_conn_mutex 内）
 * used = 正在运行、槽被占用（线程退出时清 0 → 槽可被后续连接复用）
 * ever = 该槽当前句柄是否尚未 join（回收器/关闭路径据此 join，join 后清 0） */
static pthread_mutex_t g_conn_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       g_conn_slots[GW_MAX_CONN];
static unsigned char   g_conn_slot_used[GW_MAX_CONN];
static unsigned char   g_conn_slot_ever[GW_MAX_CONN];
static int             g_conn_count = 0;

typedef struct {
    GatewaySystem* gw;
    int client_fd;
    int slot;            /* C1: 本线程占用的槽号 */
} ConnArg;

/* C1: 主循环里的机会式回收——把已结束但尚未 join 的连接线程收掉。
 * 必须在不持 g_conn_mutex 的情况下 join：线程退出路径要拿同一把锁清 used,
 * 持锁 join 会死锁。先复制 tid、清 ever（记账在锁内完成），再锁外 join。 */
static void gw_reap_conn_threads(void) {
    for (int s = 0; s < GW_MAX_CONN; s++) {
        pthread_t tid = 0;
        int need = 0;
        pthread_mutex_lock(&g_conn_mutex);
        if (g_conn_slot_ever[s] && !g_conn_slot_used[s]) {
            g_conn_slot_ever[s] = 0;
            tid = g_conn_slots[s];
            need = 1;
        }
        pthread_mutex_unlock(&g_conn_mutex);
        if (need) pthread_join(tid, NULL);
    }
}

static void* gw_conn_thread(void* arg) {
    ConnArg* ca = (ConnArg*)arg;
    GatewaySystem* gw = ca->gw;      /* C1: 先把内容取出来，ca 立刻释放 */
    int client_fd = ca->client_fd;
    int slot = ca->slot;
    free(ca);

    handle_connection(gw, client_fd);

    /* C1: 退槽 —— 关闭路径按槽 join，所以只能在线程真正结束时清标记 */
    pthread_mutex_lock(&g_conn_mutex);
    g_conn_slot_used[slot] = 0;
    g_conn_count--;
    pthread_mutex_unlock(&g_conn_mutex);
    return NULL;
}

/* P1-4: 原 handle_connection 的实体（req 改为由包装函数传入的堆对象） */
static void handle_connection_inner(GatewaySystem* gw, int client_fd, HttpRequest* req);

/* P1-4: 包装——HttpRequest 含 64KB body，旧版直接开在连接线程栈上：
 * GW_MAX_CONN=64 时 64×64KB ≈ 4MB 常驻栈（同一线程内 parse_request 还有
 * 另一份 64KB，见 gateway_http.c）。这里整对象 calloc 到堆，处理完即释放，
 * 每个连接线程栈上不再有这两个大缓冲。 */
static void handle_connection(GatewaySystem* gw, int client_fd) {
    HttpRequest* req = (HttpRequest*)calloc(1, sizeof(HttpRequest));
    if (!req) {
        http_json(client_fd, 500, "{\"error\":\"out of memory\"}");
        close(client_fd);
        return;
    }
    handle_connection_inner(gw, client_fd, req);
    free(req);
}

static void handle_connection_inner(GatewaySystem* gw, int client_fd, HttpRequest* req) {
    /* 加固: TCP keepalive 检测死连接 */
    int ka = 1;
    setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));
    /* 加固: 发送超时 10s */
    struct timeval stv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof(stv));

    if (parse_request(client_fd, req) < 0) {
        http_json(client_fd, 400, "{\"error\":\"bad request\"}");
        close(client_fd);
        return;
    }

    // CORS preflight
    if (strcmp(req->method, "OPTIONS") == 0) {
        http_send(client_fd, 200, "text/plain", "");
        close(client_fd);
        return;
    }

    /* 安全加固: 鉴权扩展到所有非健康检查端点 — 除 /health(/healthz) 外，
     * 所有端点必须携带有效 X-Pivot-Token（与现有 /media 端点同一机制，兼容现有调用方）。
     * 只保留 /health 匿名，供负载均衡/监控/启动探测。token 为空则拒绝（安全默认）。 */
    int is_health = (strcmp(req->path, "/health") == 0 ||
                     strcmp(req->path, "/healthz") == 0);
    if (!is_health &&
        (gw->api_token[0] == '\0' || !gw_token_equal(req->token, gw->api_token))) {
        http_json(client_fd, 401, "{\"error\":\"unauthorized: missing or invalid X-Pivot-Token\"}");
        close(client_fd);
        return;
    }

    // 路由
    if (strcmp(req->method, "GET") == 0) {
        if (strcmp(req->path, "/") == 0 || strcmp(req->path, "/dashboard") == 0) {
            handle_root(gw, client_fd);
        } else if (is_health) {   /* P2-1: /healthz 已被鉴权豁免（见上面 is_health），
                                   * 旧版这里只匹配 "/health" → /healthz 落到 404，
                                   * K8s/systemd 探针配 /healthz 永远拿 404 */
            handle_health(gw, client_fd);
        } else if (strcmp(req->path, "/qa") == 0) {
            char rs[64]; snprintf(rs, sizeof(rs), "{\"count\":%d}", gw->qa_memory ? qa_memory_count(gw->qa_memory) : 0);
            http_json(client_fd, 200, rs);
        } else if (strcmp(req->path, "/status") == 0) {
            if (!gw->engine_ready) {
                http_json(client_fd, 503, "{\"status\":\"loading\"}");
            } else {
                handle_status(gw, client_fd);
            }
        } else if (strcmp(req->path, "/train/status") == 0) {
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
        } else if (strcmp(req->path, "/scheduler") == 0) {
            handle_scheduler(gw, client_fd);
        } else if (strcmp(req->path, "/scheduler/stats") == 0) {
            handle_scheduler_self_stats(gw, client_fd);
        } else if (strcmp(req->path, "/brain") == 0) {
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
        } else if (strcmp(req->path, "/media/status") == 0) {
            handle_media_status(gw, client_fd);
        } else if (strcmp(req->path, "/debug") == 0) {
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
        } else if (strcmp(req->path, "/force_templates") == 0) {
            int built = broca_build_templates(gw->topology, 10, 4);
            char rsp[128];
            snprintf(rsp, sizeof(rsp), "{\"built\":%d}", built);
            http_json(client_fd, 200, rsp);
            fprintf(stderr, "[gateway] 强制模板构建: %d\n", built);
        } else {
            http_json(client_fd, 404, "{\"error\":\"not found\"}");
        }
    } else if (strcmp(req->method, "POST") == 0) {
        if (!gw->engine_ready) {
            http_json(client_fd, 503, "{\"status\":\"loading\",\"message\":\"engine initializing\"}");
        } else if (strcmp(req->path, "/chat") == 0) {
            handle_chat(gw, client_fd, req->body);
        } else if (strcmp(req->path, "/qa") == 0) {
            handle_qa(gw, client_fd, req->body);
        } else if (strcmp(req->path, "/learn") == 0) {
            handle_learn(gw, client_fd, req->body);
        } else if (strcmp(req->path, "/feedback") == 0) {
            handle_feedback(gw, client_fd, req->body);
        } else if (strcmp(req->path, "/media/feed") == 0) {
            handle_media_feed(gw, client_fd, req->body);
        } else if (strncmp(req->path, "/train/", 7) == 0) {
            if (!gw->train_mode) {
                http_json(client_fd, 404, "{\"error\":\"train mode not enabled\"}");
            } else if (strcmp(req->path, "/train/status") == 0) {
                TrainProgress p = train_mode_get_progress(gw->train_mode);
                const char* st = p.state==TRAIN_RUNNING?"running":p.state==TRAIN_PAUSED?"paused":p.state==TRAIN_COMPLETED?"completed":"idle";
                char tr[512];
                snprintf(tr, sizeof(tr), "{\"state\":\"%s\",\"current_round\":%d,\"total_rounds\":%d,\"current_line\":%ld,\"total_lines\":%ld,\"total_fed\":%ld,\"total_added_nodes\":%ld,\"total_added_edges\":%ld}",
                    st, p.current_round, p.total_rounds, p.current_line, p.total_lines,
                    p.total_fed, p.total_added_nodes, p.total_added_edges);
                http_json(client_fd, 200, tr);
            } else if (strcmp(req->path, "/train/pause") == 0) {
                train_mode_pause(gw->train_mode);
                http_json(client_fd, 200, "{\"result\":\"paused\"}");
            } else if (strcmp(req->path, "/train/resume") == 0) {
                train_mode_resume(gw->train_mode);
                http_json(client_fd, 200, "{\"result\":\"resumed\"}");
            } else if (strcmp(req->path, "/train/stop") == 0) {
                train_mode_stop(gw->train_mode);
                http_json(client_fd, 200, "{\"result\":\"stopped\"}");
            } else if (strcmp(req->path, "/train/start") == 0) {
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
    /* 路径 SSOT：显式设置 PIVOTMIND_LOG_FILE 才重定向（journald 默认可见性不变）；
     * 路径本身由 pm_log_path() 统一规范化（绝对路径校验 + 去尾斜杠）。 */
    const char* logf = getenv("PIVOTMIND_LOG_FILE");
    if (logf && logf[0]) {
        logf = pm_log_path();
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
                /* P1-B 双根修复：默认语料走路径 SSOT（绝对路径），
                 * 不再相对 argv[2] 解析。原实现下「语料读 <workdir>/data/」
                 * 而「状态写 <pm_home>/data/」，同一进程读写落在两个不同根上。 */
                train_config.corpus_path = pm_asset(PM_ASSET_QA_CORPUS);
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

    /* 切换工作目录 —— 只影响【语料】相对路径的解析；
     * 数据文件落点已改由路径 SSOT（pm_home() 绝对路径）决定，不再跟随 CWD。 */
    if (chdir(workdir) != 0) {
        fprintf(stderr, "[gateway] 无法切换到工作目录: %s (%s)\n", workdir, strerror(errno));
        return 1;
    }

    /* 路径 SSOT（第 2 步）：数据/日志/会话/运行/语料目录默认自建。
     * 未就绪明细逐条 WARN + 汇总；不拒绝启动（只读环境下仍可提供只读查询）。 */
    {
        unsigned bad = pm_ensure_dirs(PM_DIR_ALL);
        if (bad != 0u)
            fprintf(stderr, "[gateway] ⚠ 部分数据目录未就绪 (mask=0x%x)："
                            "加载/存盘会失败，请检查 PIVOTMIND_HOME 或 $HOME 的可写性\n", bad);
        else
            printf("[gateway] 数据根: %s\n", pm_home());
    }

    /* 旧扁平布局 fail-loud 门（v0.5.33）：数据文件仍在 <home>/ 而 SSOT 只读 <home>/data/
     * 时，继续启动会【静默从空脑开始】（不报错）= 失忆，故此处拒绝启动。
     * 显式放行：PIVOTMIND_ALLOW_LEGACY_LAYOUT=1。 */
    if (pm_legacy_layout_guard("gateway", 1) != 0) return 1;

    /* C1: GatewaySystem 放到堆上（旧版是 main 的栈对象，连接线程、学习 worker、
     * 初始化线程三方同时持有它的地址，关闭期一拆就是 use-after-free）。
     * 上堆之后生命周期仍由"连接线程 join + init 线程 join"保证，
     * 全部 join 完才允许 free（见函数末尾）。 */
    GatewaySystem* gw = (GatewaySystem*)calloc(1, sizeof(GatewaySystem));
    if (!gw) { fprintf(stderr, "[gateway] 内存不足，无法创建系统\n"); return 1; }
    gw->train_mode_flag = train_mode_flag;
    gw->train_config = train_config;
    g_gw = gw;
    gw->port = port;
    strncpy(gw->workdir, workdir, sizeof(gw->workdir) - 1);

    /* C1: 生成 API token 并打印到日志（媒体端点鉴权）
     * C1+ 持久化: 优先复用 GW_TOKEN_FILE 中的有效 token（跨重启不变，黑匣子/feed
     * 脚本无需跟随变化）；文件不存在/内容非法则随机生成并写入 (0600)。 */
    if (!gw_token_file_load(gw->api_token, sizeof(gw->api_token))) {
        gw_generate_token(gw->api_token, sizeof(gw->api_token));
        gw_token_file_save(gw->api_token);
    }
    /* C1: 打印脱敏 token（仅前 4 后 4），完整值存于 GW_TOKEN_FILE(0600)。
     * 旧版整段明文打印到 stdout，日志若被旁路读取即泄露完整凭据。 */
    size_t tlen = strlen(gw->api_token);
    if (tlen > 8) {
        printf("[gateway] API token: %.4s...%s (完整 token 见 %s, 权限 0600)\n",
               gw->api_token, gw->api_token + tlen - 4, GW_TOKEN_FILE);
    } else {
        printf("[gateway] API token: %.4s%s (完整 token 见 %s, 权限 0600)\n",
               gw->api_token, tlen > 4 ? "..." : "", GW_TOKEN_FILE);
    }
    printf("[gateway] 除 /health 外所有端点需要请求头 X-Pivot-Token 才能访问\n");

    /* load runtime config (optional; defaults if file missing) */
    gw->config = config_load(NULL);

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
    if (pthread_create(&init_thread, NULL, gw_system_init_thread, gw) != 0) {
        fprintf(stderr, "[gateway] 无法创建初始化线程\n");
        close(server_fd);
        return 1;
    }
    /* C2: 不 detach —— 关闭时必须能 join 初始化线程。旧版 detach 后永远无法
     * 等待它结束，于是 gw_system_shutdown 会与仍在跑 gw_system_init 的线程
     * 并发销毁/创建同一批对象（含 shutdown 之后又 learn_queue_init 重建
     * worker）→ use-after-free + 双重释放。 */

    // 主循环 (引擎初始化期间 /health 返回 loading，初始化完成后正常服务)
    while (!gw->shutdown_requested) {
        gw_reap_conn_threads();   /* C1: 收掉上一轮已结束的连接线程（避免僵尸线程堆积） */
        // 用非阻塞 accept + 短超时，避免初始化卡住时无法响应信号
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            if (errno == EINTR || gw->shutdown_requested) break;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            // 可能是超时，继续循环
            continue;
        }

        /* C1: 连接处理移交独立线程，主循环立即回到 accept。
         * 并发满时直接 503 拒绝并关闭，避免排队堆积。
         * 与旧版的区别：槽位预约 + 句柄登记（可 join），不再 detach。 */
        ConnArg* ca = (ConnArg*)malloc(sizeof(ConnArg));
        if (!ca) {
            http_json(client_fd, 500, "{\"error\":\"out of memory\"}");
            close(client_fd);
            continue;
        }
        ca->gw = gw;
        ca->client_fd = client_fd;

        int slot = -1;
        pthread_mutex_lock(&g_conn_mutex);
        if (g_conn_count < GW_MAX_CONN) {
            for (int s = 0; s < GW_MAX_CONN; s++)
                if (!g_conn_slot_used[s]) { slot = s; break; }
        }
        if (slot >= 0) {
            ca->slot = slot;
            g_conn_slot_used[slot] = 1;
            g_conn_slot_ever[slot] = 1;
            /* 先占位再加计数：子线程退出时只做减法，若先 create 再 ++，
             * 快退的子线程可能把计数减到 0（少算一个在途连接）。 */
            g_conn_count++;
            if (pthread_create(&g_conn_slots[slot], NULL, gw_conn_thread, ca) != 0) {
                g_conn_slot_used[slot] = 0;
                g_conn_slot_ever[slot] = 0;
                g_conn_count--;
                slot = -1;
            }
        }
        pthread_mutex_unlock(&g_conn_mutex);
        if (slot < 0) {
            http_json(client_fd, 503, "{\"error\":\"busy: too many connections\"}");
            close(client_fd);
            free(ca);
            continue;
        }
    }

    /* C2: 等初始化线程彻底结束（不再 detach，见 pthread_create 处）。
     * 旧代码 while (!engine_ready && !shutdown_requested)：收到信号后
     * shutdown_requested==1 使循环立刻退出，随后 gw_system_shutdown 与仍在跑
     * gw_system_init 的线程并发拆/建同一批对象 → UAF + 双重释放。
     * 现在无条件 join：返回 == init 线程已离开 gw。
     * （配套 H4：gw_system_init 内部检查 gw->shutdown_requested 提前收手。） */
    fprintf(stderr, "[gateway] 等待初始化线程结束...\n");
    pthread_join(init_thread, NULL);

    // 清理
    close(server_fd);
    remove("/tmp/pivotmind.port");   /* P2: 旧版只写不删，外部脚本会读到过期端口 */

    /* C1: 逐槽 join 在途连接线程（不再"轮询 15s 后强拆"）。
     * 此刻 accept 循环已退出、监听 socket 已 close → 不会再有新连接占槽，
     * 因此 ever 快照稳定。join 返回即代表该线程彻底离开 gw，之后才允许
     * destroy/free gw。代价：关闭时长 = 最慢的那个在途请求。 */
    fprintf(stderr, "[gateway] 等待在途连接结束 (%d)...\n", g_conn_count);
    for (int s = 0; s < GW_MAX_CONN; s++) {
        pthread_t tid;
        int ever;
        pthread_mutex_lock(&g_conn_mutex);
        ever = g_conn_slot_ever[s];
        tid  = g_conn_slots[s];
        pthread_mutex_unlock(&g_conn_mutex);
        if (ever) pthread_join(tid, NULL);
    }

    // 停止训练模式
    if (gw->train_mode) {
        // train_mode_destroy 内部会调 train_mode_stop，不重复调
        train_mode_destroy(gw->train_mode);
        gw->train_mode = NULL;
    }

    gw_system_shutdown(gw);

    /* C1/P1-3: 拆完立刻断开全局指针，任何迟到线程都不会再解引用已释放的 gw */
    g_gw = NULL;
    free(gw);

    printf("[gateway] 再见!\n");
    return 0;
}
