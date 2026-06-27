/**
 * @file test_forgetting.c
 * @brief Unit tests for catastrophic_forgetting.c — EWC config and structures
 */

#include "../include/common.h"
#include "../include/catastrophic_forgetting.h"
#include <stdio.h>
#include <stdlib.h>

static int tests_run = 0, tests_passed = 0, tests_failed = 0;

#define TEST_START(name) printf("Running: %s...", name); tests_run++;
#define TEST_END() tests_passed++; printf(" PASSED\n")
#define TEST_FAIL(msg) tests_failed++; printf(" FAILED: %s\n", msg);
#define ASSERT_TRUE(cond, msg) do { if (!(cond)) { TEST_FAIL(msg); return; } } while (0)
#define ASSERT_NOT_NULL(ptr, msg) ASSERT_TRUE((ptr) != NULL, msg)
#define ASSERT_EQUAL(a, b, msg) ASSERT_TRUE((a) == (b), msg)

void test_ewc_config_create(void) {
    TEST_START("ewc_config_create/destroy");
    EWCConfig* cfg = ewc_config_create();
    ASSERT_NOT_NULL(cfg, "create failed");
    ASSERT_TRUE(cfg->lambda > 0.0f, "lambda should be positive");
    ASSERT_TRUE(cfg->fisher_update_interval > 0, "update interval should be > 0");
    free(cfg);
    TEST_END();
}

void test_ewc_config_defaults(void) {
    TEST_START("ewc_config defaults");
    EWCConfig* cfg = ewc_config_create();
    ASSERT_NOT_NULL(cfg, "create failed");

    /* Default EWC config should have reasonable values */
    ASSERT_TRUE(cfg->lambda > 0.0f, "lambda should be positive");
    ASSERT_TRUE(cfg->fisher_update_interval >= 1, "update interval too small");
    ASSERT_TRUE(cfg->gamma >= 0.0f && cfg->gamma <= 1.0f, "gamma out of range");

    free(cfg);
    TEST_END();
}

void test_ewc_config_custom(void) {
    TEST_START("ewc_config custom");
    EWCConfig* cfg = ewc_config_create();
    ASSERT_NOT_NULL(cfg, "create failed");

    /* Modify and verify */
    cfg->lambda = 10.0f;
    cfg->fisher_update_interval = 50;
    cfg->online_ewc = true;
    cfg->gamma = 0.9f;

    ASSERT_EQUAL(cfg->lambda, 10.0f, "lambda not set");
    ASSERT_EQUAL(cfg->fisher_update_interval, 50, "interval not set");
    ASSERT_TRUE(cfg->online_ewc, "online_ewc not set");
    ASSERT_TRUE(cfg->gamma > 0.89f && cfg->gamma < 0.91f, "gamma not set");

    free(cfg);
    TEST_END();
}

int main(void) {
    printf("\n=== PivotMind Catastrophic Forgetting Unit Tests ===\n\n");

    test_ewc_config_create();
    test_ewc_config_defaults();
    test_ewc_config_custom();

    printf("\n=== Results: %d run, %d passed, %d failed ===\n",
           tests_run, tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
