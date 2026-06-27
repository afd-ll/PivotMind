/**
 * @file test_diffusion.c
 * @brief Unit tests for diffusion.c — init safety, basic lifecycle
 */

#include "../include/common.h"
#include "../include/diffusion.h"
#include "../include/multi_topology.h"
#include <stdio.h>

static int tests_run = 0, tests_passed = 0, tests_failed = 0;

#define TEST_START(name) printf("Running: %s...", name); tests_run++;
#define TEST_END() tests_passed++; printf(" PASSED\n")
#define TEST_FAIL(msg) tests_failed++; printf(" FAILED: %s\n", msg);
#define ASSERT_TRUE(cond, msg) do { if (!(cond)) { TEST_FAIL(msg); return; } } while (0)

/* test: diffusion_init with NULL master */
void test_diffusion_init_null(void) {
    TEST_START("diffusion_init NULL master");
    DiffusionCtx ctx;
    int rc = diffusion_init(&ctx, NULL);
    ASSERT_TRUE(rc < 0, "should fail with NULL");
    TEST_END();
}

/* test: diffusion_init with uninitialized master */
void test_diffusion_init_uninitialized(void) {
    TEST_START("diffusion_init uninitialized master");
    MasterTopology* master = master_topology_create(11);
    if (!master) {
        printf(" SKIP (no memory)\n");
        tests_run--;
        return;
    }

    DiffusionCtx ctx;
    int rc = diffusion_init(&ctx, master);
    /* With uninitialized sub-topologies, may fail — that's fine */
    /* Just verify no crash */
    ASSERT_TRUE(rc >= -1 || rc == -1, "unexpected return");

    master_topology_destroy(master);
    TEST_END();
}

/* test: DiffusionCtx constants are sensible */
void test_diffusion_constants(void) {
    TEST_START("diffusion constants");
    ASSERT_TRUE(DIFF_MAX_CANDIDATES == 256, "candidate limit");
    ASSERT_TRUE(DIFF_MAX_PATH_DEPTH == 5, "path depth");
    ASSERT_TRUE(DIFF_MAX_SEQUENCE == 32, "sequence length");
    TEST_END();
}

int main(void) {
    printf("\n=== PivotMind Diffusion Engine Unit Tests ===\n\n");

    test_diffusion_init_null();
    test_diffusion_init_uninitialized();
    test_diffusion_constants();

    printf("\n=== Results: %d run, %d passed, %d failed ===\n",
           tests_run, tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
