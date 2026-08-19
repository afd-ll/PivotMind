/**
 * @file web_fetch.c
 * @brief PivotMind 爬虫框架实现 — libcurl 引擎 + 策略管控层
 *
 * 架构：
 *   ┌─────────────────────────────────────┐
 *   │  策略层（本文件）                      │
 *   │  • 域名限速 (domain throttle)         │
 *   │  • robots.txt 缓存                   │
 *   │  • UA 轮换池                          │
 *   │  • 响应码 → FetchClass 映射           │
 *   │  • 重试 + 指数退避                     │
 *   ├─────────────────────────────────────┤
 *   │  libcurl 引擎（传输层）                │
 *   │  • HTTP/1.1 + HTTP/2                 │
 *   │  • TLS 1.3 + 证书链验证               │
 *   │  • gzip / Brotli 自动解压             │
 *   │  • 302 重定向自动跟随                  │
 *   │  • Cookie jar（内存）                  │
 *   └─────────────────────────────────────┘
 *
 * 线程安全：每个线程持有 _Thread_local CURL* 句柄
 */

#include "web_fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define msleep(ms) Sleep(ms)
#else
#include <unistd.h>
#include <strings.h>
#define msleep(ms) usleep((ms) * 1000)
#endif

#include <stdint.h>
#include <curl/curl.h>

/* ================================================================
 *  全局状态
 * ================================================================ */

static CrawlPolicy g_policy;
static int         g_initialized = 0;

/* 全局互斥锁：保护 curl_easy_perform 防止 SSL 多线程初始化竞态 */
#ifndef _WIN32
#include <pthread.h>
static pthread_mutex_t g_fetch_lock = PTHREAD_MUTEX_INITIALIZER;
#else
static CRITICAL_SECTION g_fetch_lock;
static int g_fetch_lock_init_done = 0;
static void fetch_lock_init(void) {
    if (!g_fetch_lock_init_done) {
        InitializeCriticalSection(&g_fetch_lock);
        g_fetch_lock_init_done = 1;
    }
}
#endif

/* 遗留助手：实际路径走 web_fetch_lock_timeout（带超时），保留以防回退 */
__attribute__((unused))
static void web_fetch_lock(void) {
#ifdef _WIN32
    fetch_lock_init();
    EnterCriticalSection(&g_fetch_lock);
#else
    pthread_mutex_lock(&g_fetch_lock);
#endif
}
static void web_fetch_unlock(void) {
#ifdef _WIN32
    LeaveCriticalSection(&g_fetch_lock);
#else
    pthread_mutex_unlock(&g_fetch_lock);
#endif
}

/* v0.5.8: 带超时的锁获取——某请求挂起长持锁时，其他线程超时放弃而非无限等待。
 * 修复：08-07 脑干卡死——感知搜索同步 curl 持锁挂起，主循环等锁等到死。
 * @return 0=拿到锁，非0=超时未拿到（调用方应放弃本次请求） */
static int web_fetch_lock_timeout(int timeout_ms) {
#ifdef _WIN32
    fetch_lock_init();
    EnterCriticalSection(&g_fetch_lock);
    return 0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_mutex_timedlock(&g_fetch_lock, &ts);
#endif
}

/* ================================================================
 *  UA 轮换池 — 5 个真实浏览器 UA（无 PivotMind 字样）
 *  覆盖 Chrome/Edge/Firefox/Safari，降低指纹识别
 * ================================================================ */

static const char* UA_POOL[] = {
    /* Chrome 126 on Windows */
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36",

    /* Chrome 126 on macOS */
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36",

    /* Edge 126 on Windows */
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36 Edg/126.0.0.0",

    /* Firefox 128 on Linux */
    "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0",

    /* Safari 17 on macOS */
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) Version/17.5 Safari/605.1.15",
};
#define UA_POOL_SIZE 5

static const char* ua_rotate(void) {
    static _Thread_local unsigned int seed = 0;
    if (seed == 0) {
        seed = (unsigned int)time(NULL) ^ (unsigned int)(uintptr_t)&seed;
    }
    seed = seed * 1103515245 + 12345;
    return UA_POOL[((seed >> 16) & 0x7FFF) % UA_POOL_SIZE];
}

/* ================================================================
 *  域名限速器 — 同 domain 两次请求间隔 ≥ request_delay_ms
 * ================================================================ */

#define DOMAIN_HASH_SIZE  64
#define DOMAIN_MAX_LEN    256

typedef struct {
    char   domain[DOMAIN_MAX_LEN];
    time_t last_request;         /* 上次请求时间 (epoch) */
    time_t cooldown_until;       /* 外部熔断冷却到何时 (epoch) */
    int    in_use;
} DomainRecord;

static DomainRecord g_domain_table[DOMAIN_HASH_SIZE];
#ifndef _WIN32
#include <pthread.h>
static pthread_mutex_t g_domain_lock = PTHREAD_MUTEX_INITIALIZER;
#define DOMAIN_LOCK()   pthread_mutex_lock(&g_domain_lock)
#define DOMAIN_UNLOCK() pthread_mutex_unlock(&g_domain_lock)
#else
static CRITICAL_SECTION g_domain_lock;
static int g_domain_lock_init = 0;
static void domain_lock_init(void) {
    if (!g_domain_lock_init) {
        InitializeCriticalSection(&g_domain_lock);
        g_domain_lock_init = 1;
    }
}
#define DOMAIN_LOCK()   do { domain_lock_init(); EnterCriticalSection(&g_domain_lock); } while(0)
#define DOMAIN_UNLOCK() LeaveCriticalSection(&g_domain_lock)
#endif

static unsigned int domain_hash(const char* domain) {
    unsigned int h = 5381;
    for (const char* p = domain; *p; p++)
        h = ((h << 5) + h) + (unsigned char)(*p);
    return h % DOMAIN_HASH_SIZE;
}

/** 从 URL 提取域名（如 "https://baike.baidu.com/item/xxx" → "baike.baidu.com"） */
static void extract_domain(const char* url, char* domain, int domain_sz) {
    if (!url || !domain || domain_sz <= 0) { if (domain) domain[0] = '\0'; return; }

    const char* p = url;
    /* 跳过 scheme */
    if (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strncmp(p, "http://", 7) == 0) p += 7;

    int i = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && i < domain_sz - 1) {
        domain[i++] = *p++;
    }
    domain[i] = '\0';
}

/** 检查请求速率限制，必要时 sleep */
static void domain_throttle(const char* domain) {
    if (!domain || !domain[0]) return;

    DOMAIN_LOCK();

    unsigned int h = domain_hash(domain);
    DomainRecord* rec = NULL;

    /* 查找或分配 */
    for (int i = 0; i < DOMAIN_HASH_SIZE; i++) {
        unsigned int idx = (h + i) % DOMAIN_HASH_SIZE;
        if (!g_domain_table[idx].in_use) {
            rec = &g_domain_table[idx];
            rec->in_use = 1;
            rec->last_request = 0;
            rec->cooldown_until = 0;
            snprintf(rec->domain, DOMAIN_MAX_LEN, "%s", domain);
            break;
        }
        if (strcmp(g_domain_table[idx].domain, domain) == 0) {
            rec = &g_domain_table[idx];
            break;
        }
    }

    if (!rec) {
        /* 哈希满：逐出最旧的记录 */
        time_t oldest_time = (time_t)(-1);
        int oldest_idx = -1;
        for (int i = 0; i < DOMAIN_HASH_SIZE; i++) {
            if (g_domain_table[i].in_use && g_domain_table[i].last_request < oldest_time) {
                oldest_time = g_domain_table[i].last_request;
                oldest_idx = i;
            }
        }
        if (oldest_idx >= 0) {
            rec = &g_domain_table[oldest_idx];
            snprintf(rec->domain, DOMAIN_MAX_LEN, "%s", domain);
        }
    }

    if (rec) {
        time_t now = time(NULL);

        /* 外部熔断冷却检查 */
        if (rec->cooldown_until > now) {
            int wait_sec = (int)(rec->cooldown_until - now);
            DOMAIN_UNLOCK();
            if (wait_sec > 120) wait_sec = 120;  /* 最多等 2 分钟 */
            msleep(wait_sec * 1000);
            DOMAIN_LOCK();
            now = time(NULL);
        }

        /* 首次请求跳过限速（last_request==0 时差值溢出 32-bit int） */
        if (rec->last_request == 0) {
            rec->last_request = now;
            DOMAIN_UNLOCK();
            return;
        }

        /* 速率限制 */
        int elapsed_ms = (int)((now - rec->last_request) * 1000);
        int wait_ms = g_policy.request_delay_ms - elapsed_ms;
        if (wait_ms > 0) {
            DOMAIN_UNLOCK();
            msleep(wait_ms);
            DOMAIN_LOCK();
        }

        rec->last_request = time(NULL);
    }

    DOMAIN_UNLOCK();
}

/* ================================================================
 *  写回调 — libcurl 写入缓冲区
 * ================================================================ */

typedef struct {
    char* data;
    int   len;
    int   cap;
} WriteBuffer;

static size_t write_callback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    WriteBuffer* buf = (WriteBuffer*)userdata;
    size_t total = size * nmemb;
    if (buf->len + (int)total > buf->cap) {
        total = (size_t)(buf->cap - buf->len);
    }
    if (total > 0) {
        memcpy(buf->data + buf->len, ptr, total);
        buf->len += (int)total;
    }
    return size * nmemb;  /* 始终返回全部，即使截断（防止 libcurl 报错） */
}

/* ================================================================
 *  robots.txt 缓存 — 首次访问 domain 时抓取并缓存 (TTL 1h)
 * ================================================================ */

#define ROBOTS_CACHE_SIZE  64
#define ROBOTS_CACHE_TTL   3600   /* 1 小时 */

typedef struct {
    char   domain[DOMAIN_MAX_LEN];
    char*  disallow_paths[32];   /* Disallow 路径前缀 */
    int    disallow_count;
    time_t fetch_time;
    int    in_use;
    int    fetched;              /* 1=已抓取过 (即使没拿到) */
} RobotsCache;

static RobotsCache g_robots_cache[ROBOTS_CACHE_SIZE];

static RobotsCache* robots_cache_get(const char* domain) {
    if (!domain || !domain[0] || !g_policy.respect_robots) return NULL;

    unsigned int h = domain_hash(domain);
    for (int i = 0; i < ROBOTS_CACHE_SIZE; i++) {
        unsigned int idx = (h + i) % ROBOTS_CACHE_SIZE;
        if (g_robots_cache[idx].in_use &&
            strcmp(g_robots_cache[idx].domain, domain) == 0) {
            /* 检查 TTL */
            if (time(NULL) - g_robots_cache[idx].fetch_time > ROBOTS_CACHE_TTL) {
                /* 过期，清理后重新抓取 */
                for (int j = 0; j < g_robots_cache[idx].disallow_count; j++)
                    free(g_robots_cache[idx].disallow_paths[j]);
                g_robots_cache[idx].disallow_count = 0;
                g_robots_cache[idx].fetched = 0;
            }
            return &g_robots_cache[idx];
        }
    }
    return NULL;
}

static RobotsCache* robots_cache_alloc(const char* domain) {
    unsigned int h = domain_hash(domain);
    for (int i = 0; i < ROBOTS_CACHE_SIZE; i++) {
        unsigned int idx = (h + i) % ROBOTS_CACHE_SIZE;
        if (!g_robots_cache[idx].in_use) {
            memset(&g_robots_cache[idx], 0, sizeof(RobotsCache));
            g_robots_cache[idx].in_use = 1;
            snprintf(g_robots_cache[idx].domain, DOMAIN_MAX_LEN, "%s", domain);
            return &g_robots_cache[idx];
        }
    }
    return NULL;  /* 满了 */
}

/** 解析 robots.txt 的 Disallow 行 */
static void robots_parse(RobotsCache* rc, const char* body, int body_len) {
    if (!rc || !body || body_len <= 0) return;

    char* text = (char*)malloc(body_len + 1);
    if (!text) return;
    memcpy(text, body, body_len);
    text[body_len] = '\0';

    const char* p = text;
    while (*p) {
        /* 跳过空白 */
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;

        /* 找行尾 */
        const char* line_end = p;
        while (*line_end && *line_end != '\r' && *line_end != '\n') line_end++;

        /* 检查 "Disallow:" （大小写不敏感） */
        if (line_end - p > 9) {
            char line[256];
            int line_len = (int)(line_end - p);
            if (line_len > 255) line_len = 255;
            memcpy(line, p, line_len);
            line[line_len] = '\0';

            /* 大小写不敏感比较 "disallow:" */
            const char* lp = line;
            if ((lp[0] == 'D' || lp[0] == 'd') &&
                (lp[1] == 'i' || lp[1] == 'I') &&
                (lp[2] == 's' || lp[2] == 'S') &&
                (lp[3] == 'a' || lp[3] == 'A') &&
                (lp[4] == 'l' || lp[4] == 'L') &&
                (lp[5] == 'l' || lp[5] == 'L') &&
                (lp[6] == 'o' || lp[6] == 'O') &&
                (lp[7] == 'w' || lp[7] == 'W') &&
                lp[8] == ':') {

                lp += 9;
                while (*lp == ' ' || *lp == '\t') lp++;

                if (*lp && rc->disallow_count < 32) {
                    int path_len = 0;
                    while (lp[path_len] && lp[path_len] != '\r' &&
                           lp[path_len] != '\n' && lp[path_len] != ' ' &&
                           lp[path_len] != '\t') path_len++;

                    if (path_len > 0) {
                        char* path = (char*)malloc(path_len + 1);
                        if (path) {
                            memcpy(path, lp, path_len);
                            path[path_len] = '\0';
                            rc->disallow_paths[rc->disallow_count++] = path;
                        }
                    }
                }
            }
        }
        p = (*line_end) ? line_end + 1 : line_end;
    }
    free(text);
    rc->fetched = 1;
}

/** 抓取并缓存某 domain 的 robots.txt */
static void robots_fetch(const char* domain) {
    if (!domain || !domain[0] || !g_policy.respect_robots) return;

    RobotsCache* rc = robots_cache_get(domain);
    if (rc && rc->fetched) return;  /* 已有有效缓存 */

    if (!rc) rc = robots_cache_alloc(domain);
    if (!rc) return;

    char robots_url[512];
    snprintf(robots_url, sizeof(robots_url), "https://%s/robots.txt", domain);

    /* 用 libcurl 抓取（绕过 domain_throttle 和 robots检查，避免递归） */
    CURL* curl = curl_easy_init();
    if (!curl) { rc->fetched = 1; return; }

    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_URL, robots_url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua_rotate());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    /* robots.txt 通常很小，限制 64KB */
    WriteBuffer wbuf;
    wbuf.cap  = 65536;
    wbuf.data = (char*)malloc(wbuf.cap);
    wbuf.len  = 0;
    if (!wbuf.data) { curl_easy_cleanup(curl); rc->fetched = 1; return; }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wbuf);

    if (web_fetch_lock_timeout(8000) != 0) {
        /* v0.5.8: 拿不到锁（另一请求疑似挂起）——放弃本次 robots 抓取，不无限等 */
        curl_easy_cleanup(curl);
        free(wbuf.data);
        rc->fetched = 1;
        return;
    }
    CURLcode res = curl_easy_perform(curl);
    web_fetch_unlock();
    if (res == CURLE_OK && wbuf.len > 0) {
        robots_parse(rc, wbuf.data, wbuf.len);
    } else {
        rc->fetched = 1;  /* 抓取失败也算抓过了，不再重试 */
    }

    rc->fetch_time = time(NULL);
    free(wbuf.data);
    curl_easy_cleanup(curl);
}

/** 检查 URL 路径是否被 robots.txt 禁止 */
static int robots_is_allowed(const char* url) {
    if (!url || !g_policy.respect_robots) return 1;

    char domain[DOMAIN_MAX_LEN];
    extract_domain(url, domain, sizeof(domain));
    if (!domain[0]) return 1;

    /* 触发 robots.txt 抓取（首次访问） */
    RobotsCache* rc = robots_cache_get(domain);
    if (!rc || !rc->fetched) {
        robots_fetch(domain);
        rc = robots_cache_get(domain);
    }

    if (!rc || !rc->fetched) return 1;  /* 没缓存就放行 */

    /* 提取路径 */
    const char* path = url;
    if (strncmp(path, "https://", 8) == 0) path += 8;
    else if (strncmp(path, "http://", 7) == 0) path += 7;
    path = strchr(path, '/');
    if (!path) path = "/";

    for (int i = 0; i < rc->disallow_count; i++) {
        const char* dp = rc->disallow_paths[i];
        if (!dp) continue;
        int dp_len = (int)strlen(dp);
        if (dp_len == 0) continue;
        if (strncmp(path, dp, dp_len) == 0) return 0;  /* 被禁止 */
    }
    return 1;
}

/* ================================================================
 *  FetchClass 分类
 * ================================================================ */

static int classify_response(int status_code, int curl_code) {
    if (curl_code != CURLE_OK) {
        /* libcurl 错误分类 */
        switch (curl_code) {
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_CONNECT:
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_GOT_NOTHING:
            return FETCH_NETWORK_ERR;
        case CURLE_URL_MALFORMAT:
            return FETCH_PARSE_ERR;
        case CURLE_TOO_MANY_REDIRECTS:
            return FETCH_REDIRECT;
        default:
            return FETCH_NETWORK_ERR;
        }
    }

    if (status_code >= 200 && status_code < 300) return FETCH_OK;
    if (status_code >= 300 && status_code < 400) return FETCH_REDIRECT;

    if (status_code == 403 || status_code == 451) return FETCH_PERM_BLOCK;
    if (status_code == 429) return FETCH_RATE_LIMIT;
    if (status_code == 404 || status_code == 410) return FETCH_CLIENT_ERR;
    if (status_code >= 400 && status_code < 500) return FETCH_CLIENT_ERR;
    if (status_code >= 500) return FETCH_SERVER_ERR;

    return FETCH_PARSE_ERR;
}

/* ================================================================
 *  响应码辅助函数（公开 API）
 * ================================================================ */

int web_fetch_is_permanent_block(int status_code) {
    return (status_code == 403 || status_code == 451) ? 1 : 0;
}

int web_fetch_is_rate_limit(int status_code) {
    return (status_code == 429) ? 1 : 0;
}

int web_fetch_is_server_error(int status_code) {
    return (status_code >= 500 && status_code < 600) ? 1 : 0;
}

/* ================================================================
 *  头回调 — 提取 Content-Type
 * ================================================================ */

typedef struct {
    char* content_type;
    char* final_url;
    int   status_code;
} HeaderData;

static size_t header_callback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    HeaderData* hd = (HeaderData*)userdata;
    size_t total = size * nmemb;
    if (total > 512) total = 512;

    char line[520];
    memcpy(line, ptr, total);
    line[total] = '\0';

    /* 检查 Content-Type */
    if (strncasecmp(line, "Content-Type:", 13) == 0 ||
        strncasecmp(line, "content-type:", 13) == 0) {
        const char* val = line + 13;
        while (*val == ' ' || *val == ':') val++;
        const char* end = val;
        while (*end && *end != '\r' && *end != '\n') end++;
        int vlen = (int)(end - val);
        if (vlen > 0 && vlen < 256) {
            free(hd->content_type);
            hd->content_type = (char*)malloc(vlen + 1);
            if (hd->content_type) {
                memcpy(hd->content_type, val, vlen);
                hd->content_type[vlen] = '\0';
            }
        }
    }

    return total;
}

/* ================================================================
 *  主 API：web_fetch()
 * ================================================================ */

FetchResult* web_fetch(const char* url) {
    if (!url || !url[0] || !g_initialized) return NULL;

    /* robots.txt 检查 */
    if (!robots_is_allowed(url)) return NULL;

    /* 域名限速 */
    char domain[DOMAIN_MAX_LEN];
    extract_domain(url, domain, sizeof(domain));
    domain_throttle(domain);

    /* 每线程一个 CURL 句柄 */
    static _Thread_local CURL* tls_curl = NULL;
    if (!tls_curl) {
        tls_curl = curl_easy_init();
        if (!tls_curl) return NULL;

        /* 通用设置 */
        curl_easy_setopt(tls_curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
        curl_easy_setopt(tls_curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(tls_curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(tls_curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(tls_curl, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(tls_curl, CURLOPT_USERAGENT, ua_rotate());

        /* 设置超时 */
        curl_easy_setopt(tls_curl, CURLOPT_CONNECTTIMEOUT,
                         (long)(g_policy.connect_timeout_ms / 1000));
        if (g_policy.connect_timeout_ms % 1000 >= 500) {
            curl_easy_setopt(tls_curl, CURLOPT_CONNECTTIMEOUT,
                             (long)(g_policy.connect_timeout_ms / 1000 + 1));
        }
        curl_easy_setopt(tls_curl, CURLOPT_TIMEOUT,
                         (long)(g_policy.total_timeout_ms / 1000));
        curl_easy_setopt(tls_curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(tls_curl, CURLOPT_MAXREDIRS,
                         (long)g_policy.max_redirects);

        /* 代理 */
        if (g_policy.proxy_url && g_policy.proxy_url[0]) {
            curl_easy_setopt(tls_curl, CURLOPT_PROXY, g_policy.proxy_url);
        }

#ifdef HAS_OPENSSL
        /* libcurl 默认验证证书，这里保留但不再设 SSL_VERIFY_NONE */
        curl_easy_setopt(tls_curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(tls_curl, CURLOPT_SSL_VERIFYHOST, 2L);
#endif
    } else {
        /* 每次请求刷新 UA */
        curl_easy_setopt(tls_curl, CURLOPT_USERAGENT, ua_rotate());
    }

    curl_easy_setopt(tls_curl, CURLOPT_URL, url);

    /* 准备缓冲区 */
    WriteBuffer wbuf;
    wbuf.cap  = g_policy.max_body_bytes;
    wbuf.data = (char*)malloc(wbuf.cap + 1);
    wbuf.len  = 0;
    if (!wbuf.data) return NULL;

    HeaderData hd;
    memset(&hd, 0, sizeof(hd));

    curl_easy_setopt(tls_curl, CURLOPT_WRITEDATA, &wbuf);
    curl_easy_setopt(tls_curl, CURLOPT_HEADERDATA, &hd);

    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(tls_curl, CURLOPT_ERRORBUFFER, errbuf);

    /* 重试循环 */
    FetchResult* result = NULL;
    int attempt = 0;
    int max_attempts = 1 + g_policy.max_retries;

    while (attempt < max_attempts) {
        wbuf.len = 0;
        free(hd.content_type);
        hd.content_type = NULL;
        hd.status_code = 0;

    CURLcode res;
    if (web_fetch_lock_timeout(8000) != 0) {
        /* v0.5.8: 拿不到锁（另一请求疑似挂起）——放弃本次，走失败路径 */
        res = CURLE_OPERATION_TIMEDOUT;
        break;
    }
    res = curl_easy_perform(tls_curl);
    web_fetch_unlock();

        /* 获取状态码 */
        long http_code = 0;
        curl_easy_getinfo(tls_curl, CURLINFO_RESPONSE_CODE, &http_code);

        /* 获取最终 URL */
        char* eff_url = NULL;
        curl_easy_getinfo(tls_curl, CURLINFO_EFFECTIVE_URL, &eff_url);

        /* 获取重定向次数 */
        long redirect_count = 0;
        curl_easy_getinfo(tls_curl, CURLINFO_REDIRECT_COUNT, &redirect_count);

        int fc = classify_response((int)http_code, (int)res);

        /* 填充结果 */
        result = (FetchResult*)calloc(1, sizeof(FetchResult));
        if (!result) break;

        result->status_code    = (int)http_code;
        result->fetch_class    = fc;
        result->redirect_count = (int)redirect_count;

        if (eff_url && eff_url[0])
            result->final_url = strdup(eff_url);

        if (hd.content_type)
            result->content_type = hd.content_type;  /* 转移所有权 */
        hd.content_type = NULL;

        if (wbuf.len > 0) {
            result->body     = wbuf.data;  /* 转移所有权 */
            result->body_len = wbuf.len;
            wbuf.data = NULL;
        } else {
            result->body     = (char*)malloc(1);
            if (result->body) {
                result->body[0] = '\0';
                result->body_len = 0;
            }
        }

        /* 判断是否需要重试 */
        if (fc == FETCH_OK || fc == FETCH_CLIENT_ERR ||
            fc == FETCH_PERM_BLOCK || fc == FETCH_RATE_LIMIT) {
            break;  /* 成功或不可重试的错误，退出循环 */
        }

        /* 网络错误 / 服务端错误 → 指数退避重试 */
        if (attempt < max_attempts - 1) {
            int delay_ms = (g_policy.request_delay_ms * (1 << attempt));
            if (delay_ms > g_policy.max_retry_delay_ms)
                delay_ms = g_policy.max_retry_delay_ms;

            /* 释放本次的结果，准备下次重试 */
            free(result->body);
            free(result->final_url);
            free(result->content_type);
            free(result);
            result = NULL;
            wbuf.data = (char*)malloc(wbuf.cap + 1);
            if (!wbuf.data) break;

            curl_easy_setopt(tls_curl, CURLOPT_WRITEDATA, &wbuf);

            msleep(delay_ms);
        }

        attempt++;
    }

    free(wbuf.data);
    free(hd.content_type);

    if (result) {
        if (result->body) result->body[result->body_len] = '\0';
    }
    return result;
}

void web_fetch_result_free(FetchResult* r) {
    if (!r) return;
    free(r->body);
    free(r->final_url);
    free(r->content_type);
    free(r);
}

void web_fetch_cool_domain(const char* domain, int cooldown_seconds) {
    if (!domain || !domain[0]) return;

    DOMAIN_LOCK();
    unsigned int h = domain_hash(domain);
    for (int i = 0; i < DOMAIN_HASH_SIZE; i++) {
        unsigned int idx = (h + i) % DOMAIN_HASH_SIZE;
        if (g_domain_table[idx].in_use &&
            strcmp(g_domain_table[idx].domain, domain) == 0) {
            g_domain_table[idx].cooldown_until = time(NULL) + cooldown_seconds;
            break;
        }
    }
    DOMAIN_UNLOCK();
}

/* ================================================================
 *  初始化 / 销毁
 * ================================================================ */

int web_fetch_init(const CrawlPolicy* policy) {
    if (g_initialized) return 0;  /* 幂等 */

    if (policy) {
        g_policy = *policy;
    } else {
        g_policy = (CrawlPolicy)CRAWL_POLICY_DEFAULT;
    }

    /* 参数合法性检查 */
    if (g_policy.request_delay_ms < 500) g_policy.request_delay_ms = 500;
    if (g_policy.max_redirects < 1)      g_policy.max_redirects = 1;
    if (g_policy.max_redirects > 10)     g_policy.max_redirects = 10;
    if (g_policy.connect_timeout_ms < 1000) g_policy.connect_timeout_ms = 1000;
    if (g_policy.total_timeout_ms < 2000) g_policy.total_timeout_ms = 2000;
    if (g_policy.max_body_bytes < 4096)  g_policy.max_body_bytes = 4096;
    if (g_policy.max_body_bytes > 16*1024*1024) g_policy.max_body_bytes = 16*1024*1024;
    if (g_policy.max_retries < 0)        g_policy.max_retries = 0;
    if (g_policy.max_retries > 5)        g_policy.max_retries = 5;

    /* 清零域名表和 robots 缓存 */
    memset(g_domain_table, 0, sizeof(g_domain_table));
    memset(g_robots_cache, 0, sizeof(g_robots_cache));

    /* 初始化 libcurl */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* SSL 预热：主线程做一次 curl_easy_init→perform 强制 OpenSSL 初始化，
     * 避免后续多线程并发时 OPENSSL_init_ssl 竞态崩溃 */
    {
        CURL* warmup = curl_easy_init();
        if (warmup) {
            /* 设置一个无效 URL 确保不会实际联网，只触发 SSL 初始化 */
            curl_easy_setopt(warmup, CURLOPT_URL, "https://127.0.0.1:1/");
            curl_easy_setopt(warmup, CURLOPT_TIMEOUT, 1L);
            curl_easy_setopt(warmup, CURLOPT_CONNECTTIMEOUT, 1L);
            curl_easy_setopt(warmup, CURLOPT_NOSIGNAL, 1L);
            curl_easy_perform(warmup);  /* 立即失败，但触发 OpenSSL init */
            curl_easy_cleanup(warmup);
        }
    }

    g_initialized = 1;

    fprintf(stderr, "[WebFetch] 爬虫框架就绪 (delay=%dms timeout=%dms redirects=%d retries=%d "
            "robots=%d max_body=%dKB)\n",
            g_policy.request_delay_ms, g_policy.total_timeout_ms,
            g_policy.max_redirects, g_policy.max_retries,
            g_policy.respect_robots, g_policy.max_body_bytes / 1024);

    return 0;
}

void web_fetch_destroy(void) {
    if (!g_initialized) return;

    /* 清理 robots 缓存 */
    for (int i = 0; i < ROBOTS_CACHE_SIZE; i++) {
        if (g_robots_cache[i].in_use) {
            for (int j = 0; j < g_robots_cache[i].disallow_count; j++)
                free(g_robots_cache[i].disallow_paths[j]);
        }
    }
    memset(g_robots_cache, 0, sizeof(g_robots_cache));

    curl_global_cleanup();
    g_initialized = 0;

    fprintf(stderr, "[WebFetch] 爬虫框架已关闭\n");
}
