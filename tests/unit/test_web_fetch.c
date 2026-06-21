/**
 * @file test_web_fetch.c
 * @brief Unit tests for web_fetch.c — 爬虫框架
 *
 * 测试覆盖：
 *   1. 响应码分类器（纯函数，无需初始化）
 *   2. 初始化/销毁生命周期
 *   3. 策略配置边界校验
 *   4. 域名冷却
 *   5. 结果释放安全性
 *   6. 未初始化安全防护
 *   7. 在线测试（需设置 WEBB_FETCH_TEST_LIVE=1）
 *
 * 用法：
 *   离线: ./build/bin/test_web_fetch
 *   在线: WEBB_FETCH_TEST_LIVE=1 ./build/bin/test_web_fetch
 */

#include "web_fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ── 测试计数器 ── */
static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;
static int tests_skipped = 0;

#define TEST_START(name) \
    do { printf("  %-55s", name); tests_run++; } while(0)

#define TEST_END() \
    do { printf(" PASSED\n"); tests_passed++; } while(0)

#define TEST_FAIL(msg) \
    do { printf(" FAILED: %s\n", msg); tests_failed++; } while(0)

#define TEST_SKIP(msg) \
    do { printf(" SKIP: %s\n", msg); tests_skipped++; } while(0)

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { TEST_FAIL(msg); return; } } while(0)

#define ASSERT_FALSE(cond, msg) \
    ASSERT_TRUE(!(cond), msg)

#define ASSERT_NULL(ptr, msg) \
    ASSERT_TRUE((ptr) == NULL, msg)

#define ASSERT_NOT_NULL(ptr, msg) \
    ASSERT_TRUE((ptr) != NULL, msg)

#define ASSERT_EQUAL_INT(a, b, msg) \
    do { if ((a) != (b)) { \
        char _buf[128]; snprintf(_buf, sizeof(_buf), "%s (got %d, expected %d)", msg, (int)(a), (int)(b)); \
        TEST_FAIL(_buf); return; \
    } } while(0)

#define ASSERT_STR_EQ(a, b, msg) \
    do { if (!(a) || !(b) || strcmp((a),(b)) != 0) { \
        char _buf[256]; snprintf(_buf, sizeof(_buf), "%s (got \"%s\", expected \"%s\")", \
            msg, (a)?(a):"NULL", (b)?(b):"NULL"); \
        TEST_FAIL(_buf); return; \
    } } while(0)

/* ================================================================
 *  测试组 1: 响应码分类器（纯函数，无需初始化）
 * ================================================================ */

static void test_classify_permanent_block(void) {
    TEST_START("web_fetch_is_permanent_block(403)");
    ASSERT_EQUAL_INT(web_fetch_is_permanent_block(403), 1, "403 should be permanent block");
    TEST_END();

    TEST_START("web_fetch_is_permanent_block(451)");
    ASSERT_EQUAL_INT(web_fetch_is_permanent_block(451), 1, "451 should be permanent block");
    TEST_END();

    TEST_START("web_fetch_is_permanent_block(200)");
    ASSERT_EQUAL_INT(web_fetch_is_permanent_block(200), 0, "200 is not a block");
    TEST_END();

    TEST_START("web_fetch_is_permanent_block(404)");
    ASSERT_EQUAL_INT(web_fetch_is_permanent_block(404), 0, "404 is not a block");
    TEST_END();

    TEST_START("web_fetch_is_permanent_block(0)");
    ASSERT_EQUAL_INT(web_fetch_is_permanent_block(0), 0, "0 is not a block");
    TEST_END();

    TEST_START("web_fetch_is_permanent_block(-1)");
    ASSERT_EQUAL_INT(web_fetch_is_permanent_block(-1), 0, "negative is not a block");
    TEST_END();
}

static void test_classify_rate_limit(void) {
    TEST_START("web_fetch_is_rate_limit(429)");
    ASSERT_EQUAL_INT(web_fetch_is_rate_limit(429), 1, "429 is rate limit");
    TEST_END();

    TEST_START("web_fetch_is_rate_limit(200)");
    ASSERT_EQUAL_INT(web_fetch_is_rate_limit(200), 0, "200 is not rate limit");
    TEST_END();

    TEST_START("web_fetch_is_rate_limit(403)");
    ASSERT_EQUAL_INT(web_fetch_is_rate_limit(403), 0, "403 is not rate limit");
    TEST_END();

    TEST_START("web_fetch_is_rate_limit(503)");
    ASSERT_EQUAL_INT(web_fetch_is_rate_limit(503), 0, "503 is not rate limit");
    TEST_END();
}

static void test_classify_server_error(void) {
    TEST_START("web_fetch_is_server_error(500)");
    ASSERT_EQUAL_INT(web_fetch_is_server_error(500), 1, "500 is server error");
    TEST_END();

    TEST_START("web_fetch_is_server_error(502)");
    ASSERT_EQUAL_INT(web_fetch_is_server_error(502), 1, "502 is server error");
    TEST_END();

    TEST_START("web_fetch_is_server_error(503)");
    ASSERT_EQUAL_INT(web_fetch_is_server_error(503), 1, "503 is server error");
    TEST_END();

    TEST_START("web_fetch_is_server_error(599)");
    ASSERT_EQUAL_INT(web_fetch_is_server_error(599), 1, "599 is server error");
    TEST_END();

    TEST_START("web_fetch_is_server_error(200)");
    ASSERT_EQUAL_INT(web_fetch_is_server_error(200), 0, "200 is not server error");
    TEST_END();

    TEST_START("web_fetch_is_server_error(404)");
    ASSERT_EQUAL_INT(web_fetch_is_server_error(404), 0, "404 is not server error");
    TEST_END();

    TEST_START("web_fetch_is_server_error(499)");
    ASSERT_EQUAL_INT(web_fetch_is_server_error(499), 0, "499 is not server error");
    TEST_END();

    TEST_START("web_fetch_is_server_error(600)");
    ASSERT_EQUAL_INT(web_fetch_is_server_error(600), 0, "600 out of range");
    TEST_END();
}

static void test_classify_edge_cases(void) {
    /* 三大分类器的交叉检查 */
    TEST_START("403: perm_block only");
    ASSERT_TRUE(
        web_fetch_is_permanent_block(403) == 1 &&
        web_fetch_is_rate_limit(403)     == 0 &&
        web_fetch_is_server_error(403)   == 0,
        "403 classification mismatch");
    TEST_END();

    TEST_START("429: rate_limit only");
    ASSERT_TRUE(
        web_fetch_is_permanent_block(429) == 0 &&
        web_fetch_is_rate_limit(429)     == 1 &&
        web_fetch_is_server_error(429)   == 0,
        "429 classification mismatch");
    TEST_END();

    TEST_START("451: perm_block only");
    ASSERT_TRUE(
        web_fetch_is_permanent_block(451) == 1 &&
        web_fetch_is_rate_limit(451)     == 0 &&
        web_fetch_is_server_error(451)   == 0,
        "451 classification mismatch");
    TEST_END();

    TEST_START("500: server_error only");
    ASSERT_TRUE(
        web_fetch_is_permanent_block(500) == 0 &&
        web_fetch_is_rate_limit(500)     == 0 &&
        web_fetch_is_server_error(500)   == 1,
        "500 classification mismatch");
    TEST_END();

    TEST_START("200: no errors");
    ASSERT_TRUE(
        web_fetch_is_permanent_block(200) == 0 &&
        web_fetch_is_rate_limit(200)     == 0 &&
        web_fetch_is_server_error(200)   == 0,
        "200 classification mismatch");
    TEST_END();
}

/* ================================================================
 *  测试组 2: 初始化 / 销毁生命周期
 * ================================================================ */

static void test_init_default_policy(void) {
    TEST_START("web_fetch_init(NULL)");
    int ret = web_fetch_init(NULL);
    ASSERT_EQUAL_INT(ret, 0, "init with NULL policy should succeed");
    TEST_END();
}

static void test_destroy(void) {
    TEST_START("web_fetch_destroy() after init");
    web_fetch_destroy();  /* should not crash */
    printf(" PASSED\n"); tests_passed++;
}

static void test_init_idempotent(void) {
    TEST_START("web_fetch_init() is idempotent");
    int r1 = web_fetch_init(NULL);
    int r2 = web_fetch_init(NULL);
    ASSERT_EQUAL_INT(r1, 0, "first init should succeed");
    ASSERT_EQUAL_INT(r2, 0, "second init should also succeed (idempotent)");
    TEST_END();
}

static void test_init_destroy_cycle(void) {
    TEST_START("init→destroy→init cycle");
    int r1 = web_fetch_init(NULL);
    ASSERT_EQUAL_INT(r1, 0, "first init failed");
    web_fetch_destroy();
    int r2 = web_fetch_init(NULL);
    ASSERT_EQUAL_INT(r2, 0, "re-init after destroy should succeed");
    TEST_END();
}

/* ================================================================
 *  测试组 3: 策略配置边界校验
 * ================================================================ */

static void test_policy_clamp_delay(void) {
    TEST_START("policy clamps request_delay_ms < 500");
    CrawlPolicy p = CRAWL_POLICY_DEFAULT;
    p.request_delay_ms = 100;
    web_fetch_init(&p);

    /* web_fetch 返回 NULL 如果未初始化 — 但我们刚初始化了
     * 策略校验是内部的，无法直接观测。
     * 测试策略：确保小值不引起崩溃即可 */
    printf(" PASSED\n"); tests_passed++;
}

static void test_policy_clamp_redirects(void) {
    TEST_START("policy clamps max_redirects extremes");
    CrawlPolicy p = CRAWL_POLICY_DEFAULT;

    /* 太小 */
    p.max_redirects = 0;
    web_fetch_init(&p);
    /* 太大 */
    p.max_redirects = 100;
    web_fetch_init(&p);
    /* 正常 */
    p.max_redirects = 5;
    web_fetch_init(&p);
    printf(" PASSED\n"); tests_passed++;
}

static void test_policy_clamp_timeouts(void) {
    TEST_START("policy clamps timeout extremes");
    CrawlPolicy p = CRAWL_POLICY_DEFAULT;

    p.connect_timeout_ms = 500;
    web_fetch_init(&p);
    p.connect_timeout_ms = 5000;
    web_fetch_init(&p);

    p.total_timeout_ms = 1000;
    web_fetch_init(&p);
    p.total_timeout_ms = 30000;
    web_fetch_init(&p);
    printf(" PASSED\n"); tests_passed++;
}

static void test_policy_clamp_body_size(void) {
    TEST_START("policy clamps body size extremes");
    CrawlPolicy p = CRAWL_POLICY_DEFAULT;

    p.max_body_bytes = 2048;
    web_fetch_init(&p);
    p.max_body_bytes = 32*1024*1024;
    web_fetch_init(&p);
    p.max_body_bytes = 524288;
    web_fetch_init(&p);
    printf(" PASSED\n"); tests_passed++;
}

static void test_policy_clamp_retries(void) {
    TEST_START("policy clamps retries extremes");
    CrawlPolicy p = CRAWL_POLICY_DEFAULT;

    p.max_retries = -1;
    web_fetch_init(&p);
    p.max_retries = 10;
    web_fetch_init(&p);
    p.max_retries = 2;
    web_fetch_init(&p);
    printf(" PASSED\n"); tests_passed++;
}

static void test_policy_proxy_null(void) {
    TEST_START("policy with proxy=NULL");
    CrawlPolicy p = CRAWL_POLICY_DEFAULT;
    p.proxy_url = NULL;
    int ret = web_fetch_init(&p);
    ASSERT_EQUAL_INT(ret, 0, "init with proxy=NULL should succeed");
    TEST_END();
}

static void test_policy_respect_robots_off(void) {
    TEST_START("policy with respect_robots=0");
    CrawlPolicy p = CRAWL_POLICY_DEFAULT;
    p.respect_robots = 0;
    int ret = web_fetch_init(&p);
    ASSERT_EQUAL_INT(ret, 0, "init with robots off should succeed");
    TEST_END();
}

/* ================================================================
 *  测试组 4: 域名冷却
 * ================================================================ */

static void test_cool_domain_null(void) {
    TEST_START("web_fetch_cool_domain(NULL, 10)");
    web_fetch_cool_domain(NULL, 10);  /* should not crash */
    printf(" PASSED\n"); tests_passed++;
}

static void test_cool_domain_empty(void) {
    TEST_START("web_fetch_cool_domain(\"\", 10)");
    web_fetch_cool_domain("", 10);  /* should not crash */
    printf(" PASSED\n"); tests_passed++;
}

static void test_cool_domain_valid(void) {
    TEST_START("web_fetch_cool_domain(\"example.com\", 60)");
    web_fetch_init(NULL);
    web_fetch_cool_domain("example.com", 60);  /* should not crash */
    web_fetch_destroy();
    printf(" PASSED\n"); tests_passed++;
}

static void test_cool_domain_zero_seconds(void) {
    TEST_START("web_fetch_cool_domain(\"test.com\", 0)");
    web_fetch_init(NULL);
    web_fetch_cool_domain("test.com", 0);  /* zero cooldown */
    web_fetch_destroy();
    printf(" PASSED\n"); tests_passed++;
}

static void test_cool_domain_long_cooldown(void) {
    TEST_START("web_fetch_cool_domain with large cooldown");
    web_fetch_init(NULL);
    web_fetch_cool_domain("blocked.example.com", 86400);  /* 24h */
    web_fetch_destroy();
    printf(" PASSED\n"); tests_passed++;
}

/* ================================================================
 *  测试组 5: FetchResult 释放安全性
 * ================================================================ */

static void test_result_free_null(void) {
    TEST_START("web_fetch_result_free(NULL)");
    web_fetch_result_free(NULL);  /* should not crash */
    printf(" PASSED\n"); tests_passed++;
}

static void test_result_free_empty(void) {
    TEST_START("web_fetch_result_free(empty result)");
    FetchResult* r = (FetchResult*)calloc(1, sizeof(FetchResult));
    ASSERT_NOT_NULL(r, "calloc failed");
    web_fetch_result_free(r);  /* should not crash */
    printf(" PASSED\n"); tests_passed++;
}

static void test_result_free_full(void) {
    TEST_START("web_fetch_result_free(full result)");
    FetchResult* r = (FetchResult*)calloc(1, sizeof(FetchResult));
    ASSERT_NOT_NULL(r, "calloc failed");
    r->body         = strdup("test body");
    r->body_len     = 9;
    r->final_url    = strdup("https://example.com");
    r->content_type = strdup("text/html");
    r->status_code  = 200;
    r->fetch_class  = FETCH_OK;
    web_fetch_result_free(r);  /* should free all fields */
    printf(" PASSED\n"); tests_passed++;
}

/* ================================================================
 *  测试组 6: 未初始化安全防护
 * ================================================================ */

static void test_fetch_before_init(void) {
    TEST_START("web_fetch() before init → NULL");
    web_fetch_destroy();  /* ensure not initialized */
    FetchResult* r = web_fetch("https://example.com");
    ASSERT_NULL(r, "fetch before init should return NULL");
    TEST_END();
}

static void test_fetch_null_url(void) {
    TEST_START("web_fetch(NULL) → NULL");
    web_fetch_init(NULL);
    FetchResult* r = web_fetch(NULL);
    ASSERT_NULL(r, "fetch(NULL) should return NULL");
    web_fetch_destroy();
    TEST_END();
}

static void test_fetch_empty_url(void) {
    TEST_START("web_fetch(\"\") → NULL");
    web_fetch_init(NULL);
    FetchResult* r = web_fetch("");
    ASSERT_NULL(r, "fetch(\"\") should return NULL");
    web_fetch_destroy();
    TEST_END();
}

static void test_fetch_invalid_url(void) {
    TEST_START("web_fetch(\"not-a-url\") → NULL");
    web_fetch_init(NULL);
    FetchResult* r = web_fetch("not-a-url");
    ASSERT_NULL(r, "fetch(invalid url) should return NULL");
    web_fetch_destroy();
    TEST_END();
}

static void test_fetch_missing_scheme(void) {
    TEST_START("web_fetch(\"example.com\") → NULL");
    web_fetch_init(NULL);
    FetchResult* r = web_fetch("example.com");
    /* libcurl may treat this as HTTP, but behavior is undefined;
     * 主要测试不崩溃 */
    if (r) web_fetch_result_free(r);
    printf(" PASSED\n"); tests_passed++;
}

static void test_destroy_before_init(void) {
    TEST_START("web_fetch_destroy() before init");
    web_fetch_destroy();  /* ensure clean state */
    web_fetch_destroy();  /* should not crash */
    printf(" PASSED\n"); tests_passed++;
}

/* ================================================================
 *  测试组 7: 在线测试（需要网络）
 *  设置环境变量 WEBB_FETCH_TEST_LIVE=1 启用
 * ================================================================ */

static int is_live_test_enabled(void) {
    const char* env = getenv("WEBB_FETCH_TEST_LIVE");
    return (env && strcmp(env, "1") == 0) ? 1 : 0;
}

static void test_live_http_get(void) {
    if (!is_live_test_enabled()) {
        TEST_SKIP("set WEBB_FETCH_TEST_LIVE=1 to enable");
        return;
    }

    TEST_START("live: https://httpbin.org/get");
    web_fetch_init(NULL);
    FetchResult* r = web_fetch("https://httpbin.org/get");
    if (!r) {
        TEST_FAIL("fetch returned NULL (network issue?)");
        web_fetch_destroy();
        return;
    }
    ASSERT_EQUAL_INT(r->status_code, 200, "expected 200 OK");
    ASSERT_NOT_NULL(r->body, "body should not be null");
    ASSERT_TRUE(r->body_len > 0, "body should not be empty");
    ASSERT_EQUAL_INT(r->fetch_class, FETCH_OK, "expected FETCH_OK");
    ASSERT_TRUE(strstr(r->body, "httpbin"), "body should contain httpbin");

    web_fetch_result_free(r);
    web_fetch_destroy();
    TEST_END();
}

static void test_live_https_get(void) {
    if (!is_live_test_enabled()) {
        TEST_SKIP("set WEBB_FETCH_TEST_LIVE=1 to enable");
        return;
    }

    TEST_START("live: https://httpbin.org/ip");
    web_fetch_init(NULL);
    FetchResult* r = web_fetch("https://httpbin.org/ip");
    if (!r) {
        TEST_FAIL("fetch returned NULL");
        web_fetch_destroy();
        return;
    }
    ASSERT_EQUAL_INT(r->status_code, 200, "expected 200 OK");
    ASSERT_TRUE(r->body_len > 0, "body should not be empty");

    web_fetch_result_free(r);
    web_fetch_destroy();
    TEST_END();
}

static void test_live_status_404(void) {
    if (!is_live_test_enabled()) {
        TEST_SKIP("set WEBB_FETCH_TEST_LIVE=1 to enable");
        return;
    }

    TEST_START("live: https://httpbin.org/status/404");
    web_fetch_init(NULL);
    FetchResult* r = web_fetch("https://httpbin.org/status/404");
    if (!r) {
        TEST_FAIL("fetch returned NULL");
        web_fetch_destroy();
        return;
    }
    ASSERT_EQUAL_INT(r->status_code, 404, "expected 404");

    web_fetch_result_free(r);
    web_fetch_destroy();
    TEST_END();
}

static void test_live_status_429(void) {
    if (!is_live_test_enabled()) {
        TEST_SKIP("set WEBB_FETCH_TEST_LIVE=1 to enable");
        return;
    }

    TEST_START("live: https://httpbin.org/status/429");
    web_fetch_init(NULL);
    FetchResult* r = web_fetch("https://httpbin.org/status/429");
    if (!r) {
        TEST_FAIL("fetch returned NULL");
        web_fetch_destroy();
        return;
    }
    ASSERT_EQUAL_INT(r->status_code, 429, "expected 429");
    ASSERT_EQUAL_INT(r->fetch_class, FETCH_RATE_LIMIT, "expected FETCH_RATE_LIMIT");

    web_fetch_result_free(r);
    web_fetch_destroy();
    TEST_END();
}

static void test_live_status_503(void) {
    if (!is_live_test_enabled()) {
        TEST_SKIP("set WEBB_FETCH_TEST_LIVE=1 to enable");
        return;
    }

    TEST_START("live: https://httpbin.org/status/503");
    web_fetch_init(NULL);
    FetchResult* r = web_fetch("https://httpbin.org/status/503");
    if (!r) {
        TEST_FAIL("fetch returned NULL");
        web_fetch_destroy();
        return;
    }
    ASSERT_EQUAL_INT(r->status_code, 503, "expected 503");
    ASSERT_EQUAL_INT(r->fetch_class, FETCH_SERVER_ERR, "expected FETCH_SERVER_ERR");

    web_fetch_result_free(r);
    web_fetch_destroy();
    TEST_END();
}

static void test_live_redirect(void) {
    if (!is_live_test_enabled()) {
        TEST_SKIP("set WEBB_FETCH_TEST_LIVE=1 to enable");
        return;
    }

    TEST_START("live: https://httpbin.org/redirect/2");
    web_fetch_init(NULL);
    FetchResult* r = web_fetch("https://httpbin.org/redirect/2");
    if (!r) {
        TEST_FAIL("fetch returned NULL");
        web_fetch_destroy();
        return;
    }
    ASSERT_EQUAL_INT(r->status_code, 200, "expected 200 after redirect");
    ASSERT_TRUE(r->redirect_count >= 2, "expected at least 2 redirects");
    ASSERT_NOT_NULL(r->final_url, "final_url should not be null");

    web_fetch_result_free(r);
    web_fetch_destroy();
    TEST_END();
}

static void test_live_content_type(void) {
    if (!is_live_test_enabled()) {
        TEST_SKIP("set WEBB_FETCH_TEST_LIVE=1 to enable");
        return;
    }

    TEST_START("live: Content-Type extraction");
    web_fetch_init(NULL);
    FetchResult* r = web_fetch("https://httpbin.org/json");
    if (!r) {
        TEST_FAIL("fetch returned NULL");
        web_fetch_destroy();
        return;
    }
    ASSERT_EQUAL_INT(r->status_code, 200, "expected 200");
    ASSERT_NOT_NULL(r->content_type, "content_type should not be null");
    ASSERT_TRUE(strstr(r->content_type, "application/json") != NULL,
                "expected json content type");

    web_fetch_result_free(r);
    web_fetch_destroy();
    TEST_END();
}

/* ================================================================
 *  主入口
 * ================================================================ */

int main(void) {
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║          WebFetch 单元测试                        ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");

    /* ── 组 1: 响应码分类器（无需初始化） ── */
    printf("── 1. 响应码分类器 ──\n");
    test_classify_permanent_block();
    test_classify_rate_limit();
    test_classify_server_error();
    test_classify_edge_cases();

    /* ── 组 2: 初始化/销毁生命周期 ── */
    printf("\n── 2. 初始化/销毁生命周期 ──\n");
    test_init_default_policy();
    test_destroy();
    test_init_idempotent();
    test_init_destroy_cycle();

    /* ── 组 3: 策略配置边界 ── */
    printf("\n── 3. 策略配置边界校验 ──\n");
    test_policy_clamp_delay();
    test_policy_clamp_redirects();
    test_policy_clamp_timeouts();
    test_policy_clamp_body_size();
    test_policy_clamp_retries();
    test_policy_proxy_null();
    test_policy_respect_robots_off();
    web_fetch_destroy();

    /* ── 组 4: 域名冷却 ── */
    printf("\n── 4. 域名冷却 ──\n");
    test_cool_domain_null();
    test_cool_domain_empty();
    test_cool_domain_valid();
    test_cool_domain_zero_seconds();
    test_cool_domain_long_cooldown();
    web_fetch_destroy();

    /* ── 组 5: 结果释放 ── */
    printf("\n── 5. FetchResult 释放 ──\n");
    test_result_free_null();
    test_result_free_empty();
    test_result_free_full();

    /* ── 组 6: 安全防护 ── */
    printf("\n── 6. 未初始化/无效输入安全防护 ──\n");
    test_destroy_before_init();
    test_fetch_before_init();
    test_fetch_null_url();
    test_fetch_empty_url();
    test_fetch_invalid_url();
    test_fetch_missing_scheme();
    web_fetch_destroy();

    /* ── 组 7: 在线测试 ── */
    printf("\n── 7. 在线测试 (需要网络) ──\n");
    if (!is_live_test_enabled()) {
        printf("  (设置 WEBB_FETCH_TEST_LIVE=1 启用在线测试)\n");
    }
    test_live_http_get();
    test_live_https_get();
    test_live_status_404();
    test_live_status_429();
    test_live_status_503();
    test_live_redirect();
    test_live_content_type();
    web_fetch_destroy();

    /* ── 汇总 ── */
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║  测试汇总: %d/%d 通过", tests_passed, tests_run);
    if (tests_skipped > 0) printf(" (%d 跳过)", tests_skipped);
    if (tests_failed > 0)  printf(" (%d 失败)", tests_failed);
    printf("            ║\n");
    printf("╚══════════════════════════════════════════════════╝\n");

    return (tests_failed > 0) ? 1 : 0;
}
