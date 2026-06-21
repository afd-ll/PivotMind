/**
 * @file web_fetch.h
 * @brief PivotMind 爬虫框架 — libcurl 引擎 + 策略管控层
 *
 * 设计原则：
 *   libcurl 负责传输（HTTP/2, TLS 1.3, gzip/Brotli, 重定向, Cookie）
 *   策略层负责管控（域名限速, robots.txt, UA轮换, 响应码→熔断信号）
 *   调用方只需 web_fetch(url) → FetchResult，其余全部透明。
 *
 * 海外合规：
 *   - robots.txt 遵守（可配置）
 *   - 合理请求频率（同 domain ≥ 2s）
 *   - 真实浏览器 UA 轮换
 *   - TLS 证书验证（libcurl 默认开启）
 */

#ifndef WEB_FETCH_H
#define WEB_FETCH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <time.h>

/* ── 爬虫策略配置 ── */
typedef struct {
    int   request_delay_ms;      /* 同 domain 最小间隔 (默认 2000)       */
    int   max_redirects;         /* 最大重定向次数 (默认 5)              */
    int   connect_timeout_ms;    /* 连接超时 ms (默认 5000)              */
    int   total_timeout_ms;      /* 总超时 ms (默认 15000)               */
    int   max_body_bytes;        /* 最大响应体 (默认 524288 = 512KB)     */
    int   respect_robots;        /* 是否遵守 robots.txt (默认 1)         */
    int   max_retries;           /* 临时失败重试次数 (默认 2)            */
    int   max_retry_delay_ms;    /* 重试最大退避延迟 (默认 8000)         */
    char* proxy_url;             /* HTTP 代理 URL (NULL = 直连)          */
} CrawlPolicy;

#define CRAWL_POLICY_DEFAULT { 2000, 5, 5000, 15000, 524288, 1, 2, 8000, NULL }

/* ── 响应码分类枚举 ── */
typedef enum {
    FETCH_OK             = 0,   /* 2xx 成功                              */
    FETCH_REDIRECT       = 1,   /* 3xx 重定向（libcurl 已自动跟随）       */
    FETCH_CLIENT_ERR     = 2,   /* 4xx 客户端错误                        */
    FETCH_PERM_BLOCK     = 3,   /* 403/451 永久封禁 → 应冷却该 domain     */
    FETCH_RATE_LIMIT     = 4,   /* 429 限速 → 应加大延迟                 */
    FETCH_SERVER_ERR     = 5,   /* 5xx 服务端错误 → 可重试               */
    FETCH_NETWORK_ERR    = 6,   /* DNS/连接/超时 → 可重试                */
    FETCH_PARSE_ERR      = 7,   /* 响应无法解析                          */
    FETCH_TOO_LARGE      = 8    /* 响应体超出 max_body_bytes              */
} FetchClass;

/* ── 抓取结果 ── */
typedef struct {
    char*  body;                 /* 响应体（已解压）                      */
    int    body_len;
    int    status_code;          /* HTTP 状态码                           */
    char*  final_url;            /* 最终 URL（重定向后）                  */
    char*  content_type;         /* Content-Type 响应头                   */
    int    fetch_class;          /* FETCH_OK / FETCH_PERM_BLOCK ...       */
    int    redirect_count;       /* 实际重定向次数                        */
} FetchResult;

/* ── API ── */

/**
 * 初始化爬虫框架（启动时调用一次）
 * @param policy  策略配置，NULL 则使用 CRAWL_POLICY_DEFAULT
 * @return 0=成功, -1=失败
 */
int  web_fetch_init(const CrawlPolicy* policy);

/**
 * 关闭爬虫框架（退出时调用）
 */
void web_fetch_destroy(void);

/**
 * 抓取一个 URL（线程安全，_Thread_local CURL 句柄）
 * @param url  目标 URL（HTTP 或 HTTPS）
 * @return FetchResult*，调用方负责 web_fetch_result_free()
 *         失败返回 NULL
 */
FetchResult* web_fetch(const char* url);

/**
 * 释放抓取结果
 */
void web_fetch_result_free(FetchResult* r);

/**
 * 手动标记某域名应冷却（外部熔断器触发）
 * @param domain          域名（如 "baike.baidu.com"）
 * @param cooldown_seconds 冷却秒数
 */
void web_fetch_cool_domain(const char* domain, int cooldown_seconds);

/* ── 响应码分类辅助（供 perception 熔断器使用） ── */

/** 返回 1 如果状态码表示永久封禁 (403/451) */
int  web_fetch_is_permanent_block(int status_code);

/** 返回 1 如果状态码表示限速 (429) */
int  web_fetch_is_rate_limit(int status_code);

/** 返回 1 如果状态码表示服务端错误 (5xx) */
int  web_fetch_is_server_error(int status_code);

#ifdef __cplusplus
}
#endif

#endif /* WEB_FETCH_H */
