/**
 * @file test_transition_model.c
 * @brief [PROB-ASSIST v0.6.11] 转移概率模型单元测试
 *
 * 覆盖：
 *   - 基本计数 / 查询 / 自环与负 id 拒绝
 *   - add-λ 平滑的单调性（高频对概率 > 低频对）
 *   - 采样只返回该 prev 的真实 next
 *   - top_k 排序正确性
 *   - 链 logP 打分
 *   - 扩容正确性（灌入远超初始容量的对，两两可查证不丢不重）
 *   - 持久化 roundtrip
 *   - 全局开关（PIVOTMIND_PROB_ASSIST=0 时 tm_global() 返回 NULL）
 */

#include "transition_model.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int tests_run = 0, tests_passed = 0, tests_failed = 0;

#define TEST_START(name) do { \
    tests_run++; \
    printf("  [TEST] %s ... ", name); \
} while (0)

#define TEST_PASS() do { \
    tests_passed++; \
    printf("PASS\n"); \
} while (0)

#define TEST_FAIL(msg) do { \
    tests_failed++; \
    printf("FAIL: %s\n", msg); \
} while (0)

#define ASSERT_TRUE(cond, msg) do { \
    if (!(cond)) { TEST_FAIL(msg); return; } \
} while (0)

#define ASSERT_EQ_INT(a, b, msg) do { \
    if ((a) != (b)) { TEST_FAIL(msg); return; } \
} while (0)

/* ---------- 用例 ---------- */

static void test_basic_count_and_prob(void) {
    TEST_START("basic count + prob");
    TransitionModel* tm = tm_create();
    ASSERT_TRUE(tm != NULL, "tm_create returned NULL");

    /* 建链 1→2（3 次）、1→3（1 次） */
    tm_observe_pair(tm, 1, 2);
    tm_observe_pair(tm, 1, 2);
    tm_observe_pair(tm, 1, 2);
    tm_observe_pair(tm, 1, 3);
    ASSERT_EQ_INT(tm_pair_count(tm), 2, "pair count should be 2");
    ASSERT_EQ_INT((int)tm_out_count(tm, 1), 4, "out_count(1) should be 4");

    float p12 = tm_prob(tm, 1, 2);
    float p13 = tm_prob(tm, 1, 3);
    ASSERT_TRUE(p12 > p13, "p(2|1) should exceed p(3|1) (3:1 counts)");
    ASSERT_TRUE(p12 > 0.0f && p12 <= 1.0f, "p(2|1) out of range");

    /* 未观测对返回 0 */
    ASSERT_TRUE(tm_prob(tm, 1, 999) == 0.0f, "unobserved pair should be 0");
    ASSERT_TRUE(tm_prob(tm, 999, 1) == 0.0f, "unobserved prev should be 0");

    /* 自环 / 负 id 拒绝且不建项 */
    tm_observe_pair(tm, 5, 5);
    tm_observe_pair(tm, -1, 2);
    tm_observe_pair(tm, 2, -1);
    ASSERT_EQ_INT(tm_pair_count(tm), 2, "self-loop/negative ids must not create entries");

    tm_destroy(tm);
    TEST_PASS();
}

static void test_sample_and_top(void) {
    TEST_START("sample + top_k");
    TransitionModel* tm = tm_create();
    ASSERT_TRUE(tm != NULL, "tm_create returned NULL");

    /* 1→2 ×10、1→3 ×1 */
    for (int i = 0; i < 10; i++) tm_observe_pair(tm, 1, 2);
    tm_observe_pair(tm, 1, 3);

    /* 采样 200 次，只应出现 2 或 3，且 2 显著多于 3 */
    int c2 = 0, c3 = 0;
    uint32_t rng = 0x12345678u;
    for (int i = 0; i < 200; i++) {
        int32_t s = tm_sample_next(tm, 1, &rng);
        ASSERT_TRUE(s == 2 || s == 3, "sampled id outside the observed set");
        if (s == 2) c2++; else c3++;
    }
    ASSERT_TRUE(c2 > c3 * 3, "high-count next should dominate sampling");

    /* 无记录 prev 返回 -1 */
    ASSERT_EQ_INT((int)tm_sample_next(tm, 777, &rng), -1, "sample from unknown prev should be -1");

    /* top_k 排序 */
    int32_t ids[4] = {0};
    int n = tm_top_next(tm, 1, ids, 4);
    ASSERT_EQ_INT(n, 2, "top_next should return 2 candidates");
    ASSERT_EQ_INT((int)ids[0], 2, "top candidate should be id 2");
    ASSERT_EQ_INT((int)ids[1], 3, "second candidate should be id 3");

    tm_destroy(tm);
    TEST_PASS();
}

static void test_chain_logprob(void) {
    TEST_START("chain logprob");
    TransitionModel* tm = tm_create();
    ASSERT_TRUE(tm != NULL, "tm_create returned NULL");

    for (int i = 0; i < 10; i++) tm_observe_pair(tm, 1, 2);
    for (int i = 0; i < 10; i++) tm_observe_pair(tm, 2, 3);

    int32_t good_chain[] = {1, 2, 3};
    int32_t bad_chain[]  = {1, 2, 999};

    float lp_good = tm_chain_logprob(tm, good_chain, 3);
    float lp_bad  = tm_chain_logprob(tm, bad_chain, 3);
    ASSERT_TRUE(lp_good > lp_bad, "observed chain should out-score one with an unobserved hop");
    ASSERT_TRUE(lp_good < 0.0f, "log-prob of a chain should be negative");

    tm_destroy(tm);
    TEST_PASS();
}

static void test_grow_integrity(void) {
    TEST_START("grow integrity (5000 pairs)");
    TransitionModel* tm = tm_create();
    ASSERT_TRUE(tm != NULL, "tm_create returned NULL");

    /* 灌 100 个 prev × 50 个 next = 5000 对，远超初始 1024 桶 */
    for (int p = 0; p < 100; p++) {
        for (int q = 0; q < 50; q++) {
            ASSERT_EQ_INT(tm_observe_pair(tm, p, 1000 + q), 0, "observe failed during grow");
        }
    }
    ASSERT_EQ_INT(tm_pair_count(tm), 5000, "size after grow should be 5000");

    /* 抽查：每个 out_count == 50，概率在 (0,1] */
    for (int p = 0; p < 100; p += 17) {
        ASSERT_EQ_INT((int)tm_out_count(tm, p), 50, "out_count mismatch after grow");
        ASSERT_TRUE(tm_prob(tm, p, 1000) > 0.0f, "prob missing after grow");
    }

    /* 全部 5000 对可查出 */
    int found = 0;
    for (int p = 0; p < 100; p++) {
        for (int q = 0; q < 50; q++) {
            if (tm_prob(tm, p, 1000 + q) > 0.0f) found++;
        }
    }
    ASSERT_EQ_INT(found, 5000, "all 5000 pairs must remain queryable after grow");

    tm_destroy(tm);
    TEST_PASS();
}

static void test_persistence_roundtrip(void) {
    TEST_START("save/load roundtrip");
    TransitionModel* tm = tm_create();
    ASSERT_TRUE(tm != NULL, "tm_create returned NULL");

    tm_observe_pair(tm, 7, 8);
    tm_observe_pair(tm, 7, 8);
    tm_observe_pair(tm, 7, 9);
    tm_observe_pair(tm, 42, 1000);

    const char* path = "test_transitions_tmp.bin";
    ASSERT_EQ_INT(tm_save(tm, path), 0, "tm_save failed");
    int n_before = tm_pair_count(tm);
    tm_destroy(tm);

    TransitionModel* tm2 = tm_create();
    ASSERT_TRUE(tm2 != NULL, "tm_create(2) returned NULL");
    ASSERT_EQ_INT(tm_load(tm2, path), 0, "tm_load failed");
    ASSERT_EQ_INT(tm_pair_count(tm2), n_before, "size mismatch after reload");

    ASSERT_TRUE(tm_prob(tm2, 7, 8) > tm_prob(tm2, 7, 9),
                "count ratio must survive roundtrip");
    ASSERT_TRUE(tm_prob(tm2, 42, 1000) > 0.0f, "pair (42,1000) lost in roundtrip");

    tm_destroy(tm2);
    remove(path);
    TEST_PASS();
}

static void test_load_missing_file_is_ok(void) {
    TEST_START("load missing file => empty, not error");
    TransitionModel* tm = tm_create();
    ASSERT_TRUE(tm != NULL, "tm_create returned NULL");
    ASSERT_EQ_INT(tm_load(tm, "definitely_not_exists_42.bin"), -1,
                  "missing file returns -1");
    ASSERT_EQ_INT(tm_pair_count(tm), 0, "model stays empty");
    tm_destroy(tm);
    TEST_PASS();
}

static void test_global_switch(void) {
    TEST_START("global env switch");
    /* 默认（不设置环境变量）下应为启用 */
    ASSERT_EQ_INT(tm_global_enabled(), 1, "default should be enabled");

    /* 关闭时 tm_global() 必须返回 NULL 且不触发创建（本进程此前未调过
     * tm_global，g_tm 仍为 NULL；开关关闭路径不会创建）。 */
    putenv("PIVOTMIND_PROB_ASSIST=0");
    ASSERT_EQ_INT(tm_global_enabled(), 0, "env=0 should disable");
    ASSERT_TRUE(tm_global() == NULL, "tm_global must be NULL when disabled");

    /* 恢复启用（不再调 tm_global()，避免单测访问真实 pm_home 路径） */
    putenv("PIVOTMIND_PROB_ASSIST=1");
    ASSERT_EQ_INT(tm_global_enabled(), 1, "env=1 should enable");
    putenv("PIVOTMIND_PROB_ASSIST");   /* 删除变量，回归默认态 */
    ASSERT_EQ_INT(tm_global_enabled(), 1, "unset should stay enabled");
    TEST_PASS();
}

int main(void) {
    printf("\n=== PivotMind Transition Model Unit Tests ===\n\n");

    test_basic_count_and_prob();
    test_sample_and_top();
    test_chain_logprob();
    test_grow_integrity();
    test_persistence_roundtrip();
    test_load_missing_file_is_ok();
    test_global_switch();

    printf("\n=== Results: %d run, %d passed, %d failed ===\n",
           tests_run, tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
