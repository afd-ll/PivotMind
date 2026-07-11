/* test_pure.c — 纯函数单元测试
 *
 * 测试零环境依赖的公开函数：
 *   1. pos_connector_map      — 中文 POS 对 → 连接词映射
 *   2. english_connector_map  — 英文 POS 对 → 连接词映射
 *   3. compute_causal_confidence — 四因子加权公式
 *   4. causal_confidence_create   — 分配 + 初始化
 *   5. get_confidence_level       — 阈值分类
 *   6. decay_causal_confidence    — 衰减公式
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#include "cognitive_controller.h"   /* POSTag enum */
#include "multi_topology.h"          /* pos_connector_map, english_connector_map */
#include "causal_reasoning.h"        /* CausalConfidence, compute_causal_confidence, etc. */

#define MAX_OBSERVATIONS 1000  /* 与 causal_reasoning.c 一致 */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  %-50s", name); \
} while(0)

#define PASS() do { \
    tests_passed++; \
    printf("PASS\n"); \
} while(0)

#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
} while(0)

/* ================================================================
 * Test 1: pos_connector_map
 * ================================================================ */
static void test_pos_connector_map(void) {
    TEST("pos_connector_map: ADJ+NOUN → 的");
    assert(strcmp(pos_connector_map(POS_ADJ, POS_NOUN), "的") == 0);
    PASS();

    TEST("pos_connector_map: NOUN+NOUN → 的");
    assert(strcmp(pos_connector_map(POS_NOUN, POS_NOUN), "的") == 0);
    PASS();

    TEST("pos_connector_map: ADV+VERB → 地");
    assert(strcmp(pos_connector_map(POS_ADV, POS_VERB), "地") == 0);
    PASS();

    TEST("pos_connector_map: VERB+NOUN → '' (动宾)");
    assert(strcmp(pos_connector_map(POS_VERB, POS_NOUN), "") == 0);
    PASS();

    TEST("pos_connector_map: NOUN+VERB → '' (主谓)");
    assert(strcmp(pos_connector_map(POS_NOUN, POS_VERB), "") == 0);
    PASS();

    TEST("pos_connector_map: NOUN+ADJ → 是");
    assert(strcmp(pos_connector_map(POS_NOUN, POS_ADJ), "是") == 0);
    PASS();

    TEST("pos_connector_map: VERB+ADV → 得");
    assert(strcmp(pos_connector_map(POS_VERB, POS_ADV), "得") == 0);
    PASS();

    TEST("pos_connector_map: VERB+VERB → 和");
    assert(strcmp(pos_connector_map(POS_VERB, POS_VERB), "和") == 0);
    PASS();

    TEST("pos_connector_map: NUM+NOUN → 个");
    assert(strcmp(pos_connector_map(POS_NUM, POS_NOUN), "个") == 0);
    PASS();

    TEST("pos_connector_map: 未定义对 → '' (fallback)");
    assert(strcmp(pos_connector_map(POS_UNKNOWN, POS_UNKNOWN), "") == 0);
    assert(strcmp(pos_connector_map(POS_PRON, POS_PREP), "") == 0);
    PASS();
}

/* ================================================================
 * Test 2: english_connector_map
 * ================================================================ */
static void test_english_connector_map(void) {
    TEST("en_connector: ADJ+NOUN → ' '");
    assert(strcmp(english_connector_map(POS_ADJ, POS_NOUN), " ") == 0);
    PASS();

    TEST("en_connector: NOUN+NOUN → ' of '");
    assert(strcmp(english_connector_map(POS_NOUN, POS_NOUN), " of ") == 0);
    PASS();

    TEST("en_connector: NOUN+ADJ → ' is '");
    assert(strcmp(english_connector_map(POS_NOUN, POS_ADJ), " is ") == 0);
    PASS();

    TEST("en_connector: 未定义对 → ' ' (fallback)");
    assert(strcmp(english_connector_map(POS_UNKNOWN, POS_UNKNOWN), " ") == 0);
    PASS();
}

/* ================================================================
 * Test 3: compute_causal_confidence
 * ================================================================ */
static void test_compute_causal_confidence(void) {
    TEST("causal_confidence: NULL → 0.0");
    assert(compute_causal_confidence(NULL) == 0.0f);
    PASS();

    TEST("causal_confidence: 零观察, 零场景 → 依赖 base_score");
    CausalConfidence cc = {0};
    cc.base_score = 0.8f;
    /* 无观察无场景: validation=0, diversity=0.5, stability=0.5 */
    float result = compute_causal_confidence(&cc);
    /* base*0.4 + 0*0.2 + 0.5*0.2 + 0.5*0.2 = 0.32 + 0 + 0.1 + 0.1 = 0.52 */
    float expected = 0.8f * 0.4f + 0.0f * 0.2f + 0.5f * 0.2f + 0.5f * 0.2f;
    assert(fabsf(result - expected) < 0.001f);
    PASS();

    TEST("causal_confidence: 观察次数增加 → 置信度提升");
    CausalConfidence low = {.base_score = 0.5f, .observation_count = 0};
    CausalConfidence high = {.base_score = 0.5f, .observation_count = 100,
                              .total_scenarios = 1, .valid_scenarios = 1,
                              .total_tests = 1, .consistent_count = 1};
    float low_conf = compute_causal_confidence(&low);
    float high_conf = compute_causal_confidence(&high);
    assert(high_conf > low_conf);
    PASS();

    TEST("causal_confidence: base_score=1, 全满 → ~1.0");
    CausalConfidence full = {
        .base_score = 1.0f,
        .observation_count = 1000,
        .total_scenarios = 10, .valid_scenarios = 10,
        .total_tests = 100, .consistent_count = 100
    };
    float full_conf = compute_causal_confidence(&full);
    assert(full_conf > 0.9f);
    PASS();
}

/* ================================================================
 * Test 4: causal_confidence_create
 * ================================================================ */
static void test_causal_confidence_create(void) {
    TEST("causal_confidence_create: base_score=0.5");
    CausalConfidence* cc = causal_confidence_create(0.5f);
    assert(cc != NULL);
    assert(fabsf(cc->base_score - 0.5f) < 0.001f);
    assert(cc->observation_count == 1);   /* 初始化为1防除零 */
    assert(cc->total_scenarios == 1);
    assert(cc->valid_scenarios == 1);
    free(cc);
    PASS();

    TEST("causal_confidence_create: base_score=0.0 (边界)");
    CausalConfidence* cc2 = causal_confidence_create(0.0f);
    assert(cc2 != NULL);
    assert(cc2->base_score == 0.0f);
    free(cc2);
    PASS();

    TEST("causal_confidence_create: base_score=1.0 (边界)");
    CausalConfidence* cc3 = causal_confidence_create(1.0f);
    assert(cc3 != NULL);
    assert(cc3->base_score == 1.0f);
    free(cc3);
    PASS();
}

/* ================================================================
 * Test 5: get_confidence_level
 * ================================================================ */
static void test_get_confidence_level(void) {
    TEST("get_confidence_level: 0.0 → CONTEXT");
    assert(get_confidence_level(0.0f) == CAUSAL_CONF_CONTEXT);
    assert(get_confidence_level(0.2f) == CAUSAL_CONF_CONTEXT);
    PASS();

    TEST("get_confidence_level: 0.4 → SHORT_TERM");
    assert(get_confidence_level(0.4f) == CAUSAL_CONF_SHORT_TERM);
    assert(get_confidence_level(0.55f) == CAUSAL_CONF_SHORT_TERM);
    PASS();

    TEST("get_confidence_level: 0.7 → PERMANENT");
    assert(get_confidence_level(0.7f) == CAUSAL_CONF_PERMANENT);
    assert(get_confidence_level(0.78f) == CAUSAL_CONF_PERMANENT);
    PASS();

    TEST("get_confidence_level: 0.85 → CORE");
    assert(get_confidence_level(0.85f) == CAUSAL_CONF_CORE);
    assert(get_confidence_level(1.0f) == CAUSAL_CONF_CORE);
    PASS();
}

/* ================================================================
 * Test 6: decay_causal_confidence
 * ================================================================ */
static void test_decay_causal_confidence(void) {
    TEST("decay: normal decay reduces confidence");
    CausalConfidence* cc = causal_confidence_create(0.8f);
    cc->observation_count = 100;
    cc->total_scenarios = 10; cc->valid_scenarios = 10;
    cc->total_tests = 100; cc->consistent_count = 100;
    float before = compute_causal_confidence(cc);
    float decayed = decay_causal_confidence(cc);
    assert(decayed < before);
    assert(decayed > 0.0f);
    free(cc);
    PASS();

    TEST("decay: NULL → 0.0");
    assert(decay_causal_confidence(NULL) == 0.0f);
    PASS();
}

/* ================================================================
 * Main
 * ================================================================ */
int main(void) {
    printf("=== PivotMind pure function tests ===\n\n");

    test_pos_connector_map();
    test_english_connector_map();
    test_compute_causal_confidence();
    test_causal_confidence_create();
    test_get_confidence_level();
    test_decay_causal_confidence();

    printf("\n=== %d/%d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
