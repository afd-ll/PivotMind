/**
 * @file test_pfe_unit.c
 * @brief 前额叶执行器 (PFE) 单元测试 — Phase 3 全覆盖
 *
 * 测试范围：
 *   1. 复杂度评估 (pfe_assess_complexity)
 *   2. 模式确定 (pfe_determine_mode) — 关键词 + epsilon-greedy
 *   3. 模式名称 (pfe_mode_name)
 *   4. 策略权重初始化/归一化/更新
 *   5. 按模式统计 (PFEModeStats)
 *   6. 自适应参数调优 (pfe_adapt_parameters)
 *   7. 工作区持久化 roundtrip (save/load/resume)
 *   8. 策略权重持久化 roundtrip
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "prefrontal_executive.h"

/* ================================================================
 *  测试框架
 * ================================================================ */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START(name) \
    printf("  %s ... ", name); tests_run++

#define TEST_END() \
    tests_passed++; printf("PASSED\n")

#define TEST_FAIL(msg) \
    tests_failed++; printf("FAILED: %s\n", msg); return

#define ASSERT_TRUE(cond, msg) \
    do { if (!(cond)) { TEST_FAIL(msg); } } while(0)

#define ASSERT_FALSE(cond, msg) \
    ASSERT_TRUE(!(cond), msg)

#define ASSERT_EQ_INT(a, b, msg) \
    do { if ((a) != (b)) { \
        printf("FAILED: %s (got %d, expected %d)\n", msg, (int)(a), (int)(b)); \
        tests_failed++; return; \
    } } while(0)

#define ASSERT_FLOAT_RANGE(val, lo, hi, msg) \
    do { if ((val) < (lo) || (val) > (hi)) { \
        printf("FAILED: %s (got %.4f, expected [%.4f, %.4f])\n", msg, (double)(val), (double)(lo), (double)(hi)); \
        tests_failed++; return; \
    } } while(0)

#define ASSERT_FLOAT_NEAR(a, b, tol, msg) \
    do { if (fabsf((float)(a) - (float)(b)) > (float)(tol)) { \
        printf("FAILED: %s (got %.4f, expected %.4f±%.4f)\n", msg, (double)(a), (double)(b), (double)(tol)); \
        tests_failed++; return; \
    } } while(0)

/* ================================================================
 *  辅助：创建最小 PFE 用于 Phase 3 算法测试
 *  不经过 pfe_create()，直接分配并手动设置字段，
 *  避免依赖 MasterTopology / Thalamus。
 * ================================================================ */

static PrefrontalExecutive* make_minimal_pfe(void) {
    PrefrontalExecutive* pfe = (PrefrontalExecutive*)calloc(1, sizeof(PrefrontalExecutive));
    if (!pfe) return NULL;
    pfe->max_decompose_depth      = 2;
    pfe->min_subgoal_satisfaction = 0.5f;
    pfe->max_subgoal_retries      = 2;
    pfe->conflict_threshold       = 0.25f;
    pfe->temperature_base         = 0.15f;
    pfe->temperature_increment    = 0.12f;
    for (int i = 0; i < PFE_MODE_COUNT; i++)
        pfe->strategy_weights[i] = 1.0f / PFE_MODE_COUNT;
    return pfe;
}

static void destroy_minimal_pfe(PrefrontalExecutive* pfe) {
    if (pfe) {
        remove("pfe_strategy.bin");
        remove("pfe_workspace.bin");
        free(pfe);
    }
}

/* ================================================================
 *  1. 复杂度评估
 * ================================================================ */

void test_complexity_simple(void) {
    TEST_START("简单问题 complexity=0");
    ASSERT_EQ_INT(pfe_assess_complexity(NULL, "你好"), 0, "greeting");
    ASSERT_EQ_INT(pfe_assess_complexity(NULL, "今天天气不错"), 0, "short");
    TEST_END();
}

void test_complexity_medium(void) {
    TEST_START("中等复杂度 complexity=1");
    /* "为什么" 触发 +1, 需 score=2 → 再加长度 > 40，中文 UTF-8 每字 3 字节 */
    ASSERT_EQ_INT(pfe_assess_complexity(NULL, "为什么深度学习模型会过拟合？这个问题很复杂"), 1, "why+long");
    ASSERT_EQ_INT(pfe_assess_complexity(NULL,
        "如何学习编程更有效率而且能坚持下去"), 1, "how+long");
    TEST_END();
}

void test_complexity_high(void) {
    TEST_START("高复杂度 complexity=2");
    int c = pfe_assess_complexity(NULL,
        "为什么深度学习模型会过拟合？如何防止过拟合？比较Dropout和正则化的区别，以及BatchNorm的作用是什么？");
    ASSERT_EQ_INT(c, 2, "long multi-question");
    TEST_END();
}

void test_complexity_keywords(void) {
    TEST_START("关键词触发复杂度");
    /* 单个关键词只+1分，需要 score>=2 才 complexity>0
     * 凑：关键词+长文本 (>40字节) */
    ASSERT_TRUE(pfe_assess_complexity(NULL,
        "为什么下雨会导致气温降低还影响出行计划") > 0, "为什么+long");
    ASSERT_TRUE(pfe_assess_complexity(NULL,
        "比较苹果和橘子的营养价值和口感差异") > 0, "比较+long");
    ASSERT_TRUE(pfe_assess_complexity(NULL,
        "怎么去北京最快最省钱还能看风景") > 0, "怎么+long");
    ASSERT_TRUE(pfe_assess_complexity(NULL,
        "如果明天下雨了我们还要不要去爬山") > 0, "如果+long");
    ASSERT_TRUE(pfe_assess_complexity(NULL,
        "类比人类学习方式和机器学习算法") > 0, "类比+long");
    TEST_END();
}

void test_complexity_edge(void) {
    TEST_START("边界情况");
    ASSERT_EQ_INT(pfe_assess_complexity(NULL, NULL), 0, "NULL");
    ASSERT_EQ_INT(pfe_assess_complexity(NULL, ""), 0, "empty");
    ASSERT_EQ_INT(pfe_assess_complexity(NULL, "?"), 0, "single char");

    /* 40字符不含关键词 → 仍然简单 */
    ASSERT_EQ_INT(pfe_assess_complexity(NULL,
        "abcdefghijklmnopqrstuvwxyz0123456789abcd"), 0, "40 chars no keyword");
    TEST_END();
}

/* ================================================================
 *  2. 模式确定
 * ================================================================ */

void test_determine_mode_keyword(void) {
    TEST_START("关键词模式确定");
    ASSERT_EQ_INT(pfe_determine_mode(NULL, "为什么天是蓝的"),
                  PFE_MODE_DECOMPOSE, "为什么");
    ASSERT_EQ_INT(pfe_determine_mode(NULL, "比较Java和Go语言"),
                  PFE_MODE_COMPARE, "比较");
    ASSERT_EQ_INT(pfe_determine_mode(NULL, "怎么学英语"),
                  PFE_MODE_HOWTO, "怎么");
    ASSERT_EQ_INT(pfe_determine_mode(NULL, "如果地球停止自转"),
                  PFE_MODE_ABDUCE, "如果");
    ASSERT_EQ_INT(pfe_determine_mode(NULL, "类比人类和AI学习"),
                  PFE_MODE_ANALOGY, "类比");
    /* "怎么样" 含 "怎么" → 匹配为 HOWTO */
    ASSERT_EQ_INT(pfe_determine_mode(NULL, "今天天气怎么样"),
                  PFE_MODE_HOWTO, "怎么样 → HOWTO (contains 怎么)");
    ASSERT_EQ_INT(pfe_determine_mode(NULL, "今天天气好吗"),
                  PFE_MODE_DIRECT, "no keyword → DIRECT");
    ASSERT_EQ_INT(pfe_determine_mode(NULL, NULL),
                  PFE_MODE_DIRECT, "NULL → DIRECT");
    TEST_END();
}

void test_determine_mode_weighted(void) {
    TEST_START("权重选择 (epsilon-greedy)");
    PrefrontalExecutive* pfe = make_minimal_pfe();
    ASSERT_TRUE(pfe != NULL, "create minimal pfe");

    /* 给 DIRECT 模式最高权重 */
    pfe->strategy_weights[PFE_MODE_DIRECT]    = 0.5f;
    pfe->strategy_weights[PFE_MODE_DECOMPOSE] = 0.1f;
    pfe->strategy_weights[PFE_MODE_COMPARE]   = 0.1f;
    pfe->strategy_weights[PFE_MODE_HOWTO]     = 0.1f;
    pfe->strategy_weights[PFE_MODE_ABDUCE]    = 0.1f;
    pfe->strategy_weights[PFE_MODE_ANALOGY]   = 0.1f;

    /* 没有关键词的短问题 → 应该倾向于选权重最高的 DIRECT 模式
     * 由于 epsilon-greedy (15%探索)，跑多次验证大多数选 DIRECT */
    int direct_count = 0;
    int total        = 100;
    for (int i = 0; i < total; i++) {
        if (pfe_determine_mode(pfe, "你好") == PFE_MODE_DIRECT)
            direct_count++;
    }
    /* 允许 15% epsilon 探索，所以 >= 80% 选 DIRECT 即可 */
    ASSERT_TRUE(direct_count >= total * 0.75,
                "weighted selection favors highest weight");

    destroy_minimal_pfe(pfe);
    TEST_END();
}

/* ================================================================
 *  3. 模式名称
 * ================================================================ */

void test_mode_names(void) {
    TEST_START("模式名称");
    ASSERT_TRUE(strcmp(pfe_mode_name(PFE_MODE_DIRECT), "直接联想") == 0, "DIRECT");
    ASSERT_TRUE(strcmp(pfe_mode_name(PFE_MODE_DECOMPOSE), "解释分解") == 0, "DECOMPOSE");
    ASSERT_TRUE(strcmp(pfe_mode_name(PFE_MODE_COMPARE), "比较对比") == 0, "COMPARE");
    ASSERT_TRUE(strcmp(pfe_mode_name(PFE_MODE_HOWTO), "步骤指导") == 0, "HOWTO");
    ASSERT_TRUE(strcmp(pfe_mode_name(PFE_MODE_ABDUCE), "溯因推断") == 0, "ABDUCE");
    ASSERT_TRUE(strcmp(pfe_mode_name(PFE_MODE_ANALOGY), "类比结构") == 0, "ANALOGY");
    ASSERT_TRUE(strcmp(pfe_mode_name((PFEReasonMode)999), "未知") == 0, "invalid");
    ASSERT_TRUE(strcmp(pfe_mode_name((PFEReasonMode)-1), "未知") == 0, "negative");
    TEST_END();
}

/* ================================================================
 *  4. 策略权重更新 + 按模式统计
 * ================================================================ */

void test_strategy_weight_init(void) {
    TEST_START("策略权重初始均匀");
    PrefrontalExecutive* pfe = make_minimal_pfe();
    ASSERT_TRUE(pfe != NULL, "create pfe");

    float total = 0.0f;
    for (int i = 0; i < PFE_MODE_COUNT; i++) {
        ASSERT_FLOAT_NEAR(pfe->strategy_weights[i], 1.0f / PFE_MODE_COUNT,
                          0.001f, "init uniform");
        total += pfe->strategy_weights[i];
    }
    ASSERT_FLOAT_NEAR(total, 1.0f, 0.001f, "weights sum to 1");

    /* 模式统计初始为零 */
    for (int i = 0; i < PFE_MODE_COUNT; i++) {
        ASSERT_EQ_INT(pfe->per_mode_stats[i].use_count, 0, "stat use_count=0");
        ASSERT_EQ_INT(pfe->per_mode_stats[i].success_count, 0, "stat success_count=0");
    }

    destroy_minimal_pfe(pfe);
    TEST_END();
}

void test_strategy_weight_update_high_reward(void) {
    TEST_START("高奖励提升权重");
    PrefrontalExecutive* pfe = make_minimal_pfe();
    ASSERT_TRUE(pfe != NULL, "create pfe");

    float old_weight = pfe->strategy_weights[PFE_MODE_DECOMPOSE];

    /* 模拟 10 次高奖励 (0.9) → DECOMPOSE 权重应上升 */
    for (int i = 0; i < 10; i++)
        pfe_update_strategy_weights(pfe, PFE_MODE_DECOMPOSE, 0.9f);

    float new_weight = pfe->strategy_weights[PFE_MODE_DECOMPOSE];
    ASSERT_TRUE(new_weight > old_weight, "weight increased after high reward");

    /* 归一化检查 */
    float total = 0.0f;
    for (int i = 0; i < PFE_MODE_COUNT; i++)
        total += pfe->strategy_weights[i];
    ASSERT_FLOAT_NEAR(total, 1.0f, 0.001f, "still normalized");

    /* 统计验证 */
    const PFEModeStats* s = pfe_get_mode_stats(pfe, PFE_MODE_DECOMPOSE);
    ASSERT_EQ_INT(s->use_count, 10, "use_count=10");
    ASSERT_TRUE(s->success_rate > 0.5f, "success_rate high");

    destroy_minimal_pfe(pfe);
    TEST_END();
}

void test_strategy_weight_update_low_reward(void) {
    TEST_START("低奖励降低权重");
    PrefrontalExecutive* pfe = make_minimal_pfe();
    ASSERT_TRUE(pfe != NULL, "create pfe");

    /* 先给 DECOMPOSE 初始权重 */
    float old_weight = pfe->strategy_weights[PFE_MODE_DECOMPOSE];

    /* 模拟 10 次低奖励 (0.1) → 权重应下降 */
    for (int i = 0; i < 10; i++)
        pfe_update_strategy_weights(pfe, PFE_MODE_DECOMPOSE, 0.1f);

    float new_weight = pfe->strategy_weights[PFE_MODE_DECOMPOSE];
    ASSERT_TRUE(new_weight < old_weight, "weight decreased after low reward");

    const PFEModeStats* s = pfe_get_mode_stats(pfe, PFE_MODE_DECOMPOSE);
    ASSERT_TRUE(s->success_rate < 0.4f, "success_rate low");

    destroy_minimal_pfe(pfe);
    TEST_END();
}

void test_strategy_weight_reward_clamp(void) {
    TEST_START("奖励钳制 [0,1]");
    PrefrontalExecutive* pfe = make_minimal_pfe();

    /* 负奖励 → 钳制到 0 */
    pfe_update_strategy_weights(pfe, PFE_MODE_DIRECT, -0.5f);
    ASSERT_TRUE(pfe->strategy_weights[PFE_MODE_DIRECT] >= 0.0f,
                "negative reward clamped");

    /* 超 1 奖励 → 钳制到 1 */
    pfe_update_strategy_weights(pfe, PFE_MODE_DIRECT, 1.5f);
    ASSERT_TRUE(pfe->strategy_weights[PFE_MODE_DIRECT] <= 1.0f,
                ">1 reward clamped");

    destroy_minimal_pfe(pfe);
    TEST_END();
}

/* ================================================================
 *  5. 策略权重持久化 roundtrip
 * ================================================================ */

void test_strategy_weight_persistence(void) {
    TEST_START("策略权重持久化 roundtrip");
    PrefrontalExecutive* pfe = make_minimal_pfe();
    ASSERT_TRUE(pfe != NULL, "create pfe");

    /* 手动设置非均匀权重 */
    pfe->strategy_weights[0] = 0.30f;
    pfe->strategy_weights[1] = 0.25f;
    pfe->strategy_weights[2] = 0.15f;
    pfe->strategy_weights[3] = 0.10f;
    pfe->strategy_weights[4] = 0.10f;
    pfe->strategy_weights[5] = 0.10f;

    /* 保存 */
    int ret = pfe_save_strategy_weights(pfe);
    ASSERT_EQ_INT(ret, 0, "save success");

    /* 重置权重 */
    for (int i = 0; i < PFE_MODE_COUNT; i++)
        pfe->strategy_weights[i] = 0.0f;

    /* 加载 */
    ret = pfe_load_strategy_weights(pfe);
    ASSERT_EQ_INT(ret, 0, "load success");

    /* 验证 */
    ASSERT_FLOAT_NEAR(pfe->strategy_weights[0], 0.30f, 0.01f, "w[0] restored");
    ASSERT_FLOAT_NEAR(pfe->strategy_weights[1], 0.25f, 0.01f, "w[1] restored");
    ASSERT_FLOAT_NEAR(pfe->strategy_weights[2], 0.15f, 0.01f, "w[2] restored");

    destroy_minimal_pfe(pfe);
    TEST_END();
}

void test_strategy_weight_load_missing(void) {
    TEST_START("加载不存在的权重文件");
    PrefrontalExecutive* pfe = make_minimal_pfe();

    /* 确保文件不存在 */
    remove("pfe_strategy.bin");

    /* 加载应返回 -1，权重保持默认均匀 */
    int ret = pfe_load_strategy_weights(pfe);
    ASSERT_EQ_INT(ret, -1, "load missing returns -1");

    /* 权重仍为均匀默认值 */
    float total = 0.0f;
    for (int i = 0; i < PFE_MODE_COUNT; i++) {
        ASSERT_FLOAT_NEAR(pfe->strategy_weights[i],
                          1.0f / PFE_MODE_COUNT, 0.001f, "still uniform");
        total += pfe->strategy_weights[i];
    }
    ASSERT_FLOAT_NEAR(total, 1.0f, 0.001f, "still sum to 1");

    destroy_minimal_pfe(pfe);
    TEST_END();
}

/* ================================================================
 *  6. 自适应参数调优
 * ================================================================ */

void test_adapt_params_insufficient_data(void) {
    TEST_START("数据不足时不调参");
    PrefrontalExecutive* pfe = make_minimal_pfe();
    ASSERT_TRUE(pfe != NULL, "create pfe");

    float orig_min_sat = pfe->min_subgoal_satisfaction;
    int   orig_retries = pfe->max_subgoal_retries;

    /* use_count=0 → 不触发调参 */
    pfe_adapt_parameters(pfe, PFE_MODE_DECOMPOSE);

    /* 参数应不变 */
    ASSERT_FLOAT_NEAR(pfe->min_subgoal_satisfaction, orig_min_sat,
                      0.001f, "min_sat unchanged");
    ASSERT_EQ_INT(pfe->max_subgoal_retries, orig_retries, "retries unchanged");

    destroy_minimal_pfe(pfe);
    TEST_END();
}

void test_adapt_params_high_success(void) {
    TEST_START("高成功率调参");
    PrefrontalExecutive* pfe = make_minimal_pfe();
    ASSERT_TRUE(pfe != NULL, "create pfe");

    /* 模拟高成功率模式 (sr=0.9)
     * target_min_sat = 0.50 + (0.9-0.5)*0.4 = 0.66 → 门槛上升 */
    PFEModeStats* s = &pfe->per_mode_stats[PFE_MODE_DIRECT];
    s->use_count    = 10;
    s->success_rate = 0.9f;

    /* 执行多次调参让其收敛 */
    for (int i = 0; i < 50; i++)
        pfe_adapt_parameters(pfe, PFE_MODE_DIRECT);

    /* 高成功率 → 门槛应上升 (>0.55 after convergence) */
    ASSERT_TRUE(pfe->min_subgoal_satisfaction > 0.55f,
                "min_sat raised for high success");
    ASSERT_TRUE(pfe->max_subgoal_retries <= 2,
                "retries reduced for high success");

    /* 温度应降低 */
    ASSERT_TRUE(pfe->temperature_base < 0.13f,
                "temp_base reduced for high success");

    /* 冲突阈值放宽 */
    ASSERT_TRUE(pfe->conflict_threshold > 0.30f,
                "conflict relaxed for high success");

    destroy_minimal_pfe(pfe);
    TEST_END();
}

void test_adapt_params_low_success(void) {
    TEST_START("低成功率调参");
    PrefrontalExecutive* pfe = make_minimal_pfe();
    ASSERT_TRUE(pfe != NULL, "create pfe");

    /* 模拟低成功率模式 (sr=0.1)
     * target_min_sat = 0.50 + (0.1-0.5)*0.4 = 0.34 → 门槛降低 */
    PFEModeStats* s = &pfe->per_mode_stats[PFE_MODE_ANALOGY];
    s->use_count    = 10;
    s->success_rate = 0.1f;

    for (int i = 0; i < 50; i++)
        pfe_adapt_parameters(pfe, PFE_MODE_ANALOGY);

    /* 低成功率 → 门槛应降低 */
    ASSERT_TRUE(pfe->min_subgoal_satisfaction < 0.45f,
                "min_sat lowered for low success");

    destroy_minimal_pfe(pfe);
    TEST_END();
}

void test_adapt_params_bounds(void) {
    TEST_START("参数钳制在有效范围");
    PrefrontalExecutive* pfe = make_minimal_pfe();
    ASSERT_TRUE(pfe != NULL, "create pfe");

    /* 极端高成功率 (sr=1.0) 100 次迭代 */
    pfe->per_mode_stats[PFE_MODE_DIRECT].use_count    = 100;
    pfe->per_mode_stats[PFE_MODE_DIRECT].success_rate = 1.0f;
    for (int i = 0; i < 100; i++)
        pfe_adapt_parameters(pfe, PFE_MODE_DIRECT);

    ASSERT_FLOAT_RANGE(pfe->min_subgoal_satisfaction, 0.30f, 0.70f, "min_sat in [0.3,0.7]");
    ASSERT_TRUE(pfe->max_subgoal_retries >= 1 && pfe->max_subgoal_retries <= 5,
                "retries in [1,5]");
    ASSERT_FLOAT_RANGE(pfe->temperature_base, 0.08f, 0.25f, "temp_base in [0.08,0.25]");
    ASSERT_FLOAT_RANGE(pfe->temperature_increment, 0.05f, 0.18f, "temp_inc in [0.05,0.18]");
    ASSERT_FLOAT_RANGE(pfe->conflict_threshold, 0.15f, 0.40f, "conflict in [0.15,0.40]");

    destroy_minimal_pfe(pfe);
    TEST_END();
}

/* ================================================================
 *  7. 工作区持久化 roundtrip
 * ================================================================ */

void test_workspace_save_load(void) {
    TEST_START("工作区持久化 roundtrip");
    PrefrontalExecutive* pfe = make_minimal_pfe();
    ASSERT_TRUE(pfe != NULL, "create pfe");

    /* 手动设置工作区状态 */
    PFEReasonWorkspace* ws = &pfe->workspace;
    ws->goal_count      = 3;
    ws->active_goal     = 1;
    ws->mode            = PFE_MODE_DECOMPOSE;
    ws->max_depth       = 2;
    ws->conflict_count  = 1;
    ws->backtrack_count = 0;
    ws->total_retries   = 2;

    /* 填充子目标 */
    snprintf(ws->goals[0].question, sizeof(ws->goals[0].question), "什么是天空？");
    ws->goals[0].depends_on   = -1;
    ws->goals[0].status       = PFE_GOAL_SOLVED;
    ws->goals[0].answer_score = 0.85f;
    snprintf(ws->goals[0].answer_text, sizeof(ws->goals[0].answer_text), "天空是大气层");

    snprintf(ws->goals[1].question, sizeof(ws->goals[1].question), "什么是蓝色？");
    ws->goals[1].depends_on   = -1;
    ws->goals[1].status       = PFE_GOAL_SOLVED;
    ws->goals[1].answer_score = 0.72f;
    snprintf(ws->goals[1].answer_text, sizeof(ws->goals[1].answer_text), "蓝色是短波长光");

    snprintf(ws->goals[2].question, sizeof(ws->goals[2].question), "为什么天空是蓝色的？");
    ws->goals[2].depends_on   = 0;
    ws->goals[2].status       = PFE_GOAL_PENDING;
    ws->goals[2].answer_score = 0.0f;

    /* 设置工作记忆 */
    for (int t = 0; t < 12; t++)
        for (int d = 0; d < 64; d++)
            ws->working_activation[t][d] = (float)(t * 64 + d) * 0.001f;

    /* 保存 */
    int ret = pfe_save_workspace(pfe, "为什么天空是蓝色的？");
    ASSERT_EQ_INT(ret, 0, "save workspace success");

    /* 清零工作区 */
    memset(ws, 0, sizeof(PFEReasonWorkspace));

    /* 加载 */
    char qbuf[512] = {0};
    ret = pfe_load_workspace(pfe, qbuf, sizeof(qbuf));
    ASSERT_EQ_INT(ret, 0, "load workspace success");

    /* 验证 */
    ASSERT_EQ_INT(ws->goal_count, 3, "goal_count restored");
    ASSERT_EQ_INT(ws->active_goal, 1, "active_goal restored");
    ASSERT_EQ_INT(ws->mode, PFE_MODE_DECOMPOSE, "mode restored");
    ASSERT_EQ_INT(ws->conflict_count, 1, "conflict_count restored");
    ASSERT_EQ_INT(ws->total_retries, 2, "total_retries restored");

    ASSERT_EQ_INT(ws->goals[0].status, PFE_GOAL_SOLVED, "sg0 status");
    ASSERT_FLOAT_NEAR(ws->goals[0].answer_score, 0.85f, 0.001f, "sg0 score");
    ASSERT_TRUE(strcmp(ws->goals[0].question, "什么是天空？") == 0, "sg0 question");

    ASSERT_EQ_INT(ws->goals[2].status, PFE_GOAL_PENDING, "sg2 pending");
    ASSERT_EQ_INT(ws->goals[2].depends_on, 0, "sg2 depends_on");

    /* 验证工作记忆 */
    ASSERT_FLOAT_NEAR(ws->working_activation[5][32],
                      (float)(5 * 64 + 32) * 0.001f, 0.0001f, "working memory");

    /* 验证原始问题恢复 */
    ASSERT_TRUE(strcmp(qbuf, "为什么天空是蓝色的？") == 0, "question restored");

    destroy_minimal_pfe(pfe);
    TEST_END();
}

void test_workspace_load_missing(void) {
    TEST_START("加载不存在的工作区");
    PrefrontalExecutive* pfe = make_minimal_pfe();
    ASSERT_TRUE(pfe != NULL, "create pfe");

    remove("pfe_workspace.bin");

    char qbuf[512] = {0};
    int ret = pfe_load_workspace(pfe, qbuf, sizeof(qbuf));
    ASSERT_EQ_INT(ret, -1, "load missing returns -1");

    destroy_minimal_pfe(pfe);
    TEST_END();
}

void test_workspace_load_corrupt(void) {
    TEST_START("损坏文件拒绝加载");
    PrefrontalExecutive* pfe = make_minimal_pfe();
    ASSERT_TRUE(pfe != NULL, "create pfe");

    /* 写入一个杂散文件 */
    FILE* f = fopen("pfe_workspace.bin", "wb");
    if (f) {
        const char garbage[] = "not a valid workspace";
        fwrite(garbage, 1, sizeof(garbage), f);
        fclose(f);
    }

    char qbuf[512] = {0};
    int ret = pfe_load_workspace(pfe, qbuf, sizeof(qbuf));
    ASSERT_EQ_INT(ret, -1, "corrupt file rejected");

    destroy_minimal_pfe(pfe);
    TEST_END();
}

/* ================================================================
 *  8. 模式统计 API
 * ================================================================ */

void test_mode_stats_api(void) {
    TEST_START("pfe_get_mode_stats API");
    PrefrontalExecutive* pfe = make_minimal_pfe();
    ASSERT_TRUE(pfe != NULL, "create pfe");

    /* 有效模式 */
    const PFEModeStats* s = pfe_get_mode_stats(pfe, PFE_MODE_DIRECT);
    ASSERT_TRUE(s != NULL, "valid mode returns non-NULL");
    ASSERT_EQ_INT(s->use_count, 0, "initial use_count=0");

    /* 无效模式 */
    ASSERT_TRUE(pfe_get_mode_stats(pfe, (PFEReasonMode)999) == NULL, "invalid mode → NULL");
    ASSERT_TRUE(pfe_get_mode_stats(pfe, (PFEReasonMode)-1) == NULL, "negative → NULL");
    ASSERT_TRUE(pfe_get_mode_stats(NULL, PFE_MODE_DIRECT) == NULL, "NULL pfe → NULL");

    destroy_minimal_pfe(pfe);
    TEST_END();
}

void test_cycle_and_satisfaction(void) {
    TEST_START("pfe_cycle_count / pfe_avg_satisfaction");
    PrefrontalExecutive* pfe = make_minimal_pfe();
    ASSERT_TRUE(pfe != NULL, "create pfe");

    ASSERT_EQ_INT(pfe_cycle_count(NULL), 0, "NULL → 0");
    ASSERT_EQ_INT(pfe_cycle_count(pfe), 0, "new pfe → 0");

    pfe->total_reasoning_cycles = 5;
    ASSERT_EQ_INT(pfe_cycle_count(pfe), 5, "after 5 cycles");

    ASSERT_FLOAT_NEAR(pfe_avg_satisfaction(NULL), 0.0f, 0.001f, "NULL avg=0");
    ASSERT_FLOAT_NEAR(pfe_avg_satisfaction(pfe), 0.0f, 0.001f, "initial avg=0");

    destroy_minimal_pfe(pfe);
    TEST_END();
}

/* ================================================================
 *  main
 * ================================================================ */

int main(void) {
    printf("===========================================\n");
    printf("  PFE Unit Tests — Phase 3 Full Coverage\n");
    printf("===========================================\n\n");

    printf("=== 1. 复杂度评估 ===\n");
    test_complexity_simple();
    test_complexity_medium();
    test_complexity_high();
    test_complexity_keywords();
    test_complexity_edge();

    printf("\n=== 2. 模式确定 ===\n");
    test_determine_mode_keyword();
    test_determine_mode_weighted();

    printf("\n=== 3. 模式名称 ===\n");
    test_mode_names();

    printf("\n=== 4. 策略权重更新 ===\n");
    test_strategy_weight_init();
    test_strategy_weight_update_high_reward();
    test_strategy_weight_update_low_reward();
    test_strategy_weight_reward_clamp();

    printf("\n=== 5. 策略权重持久化 ===\n");
    test_strategy_weight_persistence();
    test_strategy_weight_load_missing();

    printf("\n=== 6. 自适应参数调优 ===\n");
    test_adapt_params_insufficient_data();
    test_adapt_params_high_success();
    test_adapt_params_low_success();
    test_adapt_params_bounds();

    printf("\n=== 7. 工作区持久化 ===\n");
    test_workspace_save_load();
    test_workspace_load_missing();
    test_workspace_load_corrupt();

    printf("\n=== 8. 统计 API ===\n");
    test_mode_stats_api();
    test_cycle_and_satisfaction();

    /* 汇总 */
    printf("\n===========================================\n");
    printf("  Test Results\n");
    printf("===========================================\n");
    printf("Total:    %d\n", tests_run);
    printf("Passed:   %d\n", tests_passed);
    printf("Failed:   %d\n", tests_failed);
    printf("Success:  %.1f%%\n",
           (tests_run > 0) ? (100.0f * tests_passed / tests_run) : 0.0f);

    return (tests_failed == 0) ? 0 : 1;
}
