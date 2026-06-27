/**
 * @file test_causal.c
 * @brief Unit tests for causal_reasoning.c — graph lifecycle, edges
 */

#include "../include/common.h"
#include "../include/causal_reasoning.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0, tests_passed = 0, tests_failed = 0;

#define TEST_START(name) printf("Running: %s...", name); tests_run++;
#define TEST_END() tests_passed++; printf(" PASSED\n")
#define TEST_FAIL(msg) tests_failed++; printf(" FAILED: %s\n", msg);
#define ASSERT_TRUE(cond, msg) do { if (!(cond)) { TEST_FAIL(msg); return; } } while (0)
#define ASSERT_NOT_NULL(ptr, msg) ASSERT_TRUE((ptr) != NULL, msg)
#define ASSERT_NULL(ptr, msg) ASSERT_TRUE((ptr) == NULL, msg)

void test_causal_graph_create(void) {
    TEST_START("causal_graph_create/destroy");
    CausalGraph* cg = causal_graph_create(100, 500);
    ASSERT_NOT_NULL(cg, "create failed");
    ASSERT_TRUE(cg->node_count == 100, "node_count wrong");
    ASSERT_TRUE(cg->edge_capacity == 500, "edge_capacity wrong");
    causal_graph_destroy(cg);
    TEST_END();
}

void test_causal_graph_add_edge(void) {
    TEST_START("causal_graph add_edge");
    CausalGraph* cg = causal_graph_create(10, 20);
    ASSERT_NOT_NULL(cg, "create failed");

    int rc = add_causal_edge(cg, 0, 1, CAUSAL_DIRECT, 0.8f);
    ASSERT_TRUE(rc >= 0, "add_edge failed");

    bool exists = causal_edge_exists(cg, 0, 1);
    ASSERT_TRUE(exists, "edge should exist after add");

    CausalEdge* edge = get_causal_edge(cg, 0, 1);
    ASSERT_NOT_NULL(edge, "get_edge returned NULL");
    if (edge) {
        ASSERT_TRUE(edge->cause_node_id == 0, "cause_node_id wrong");
        ASSERT_TRUE(edge->effect_node_id == 1, "effect_node_id wrong");
    }

    causal_graph_destroy(cg);
    TEST_END();
}

void test_causal_graph_remove_edge(void) {
    TEST_START("causal_graph remove_edge");
    CausalGraph* cg = causal_graph_create(10, 20);
    ASSERT_NOT_NULL(cg, "create failed");

    add_causal_edge(cg, 0, 1, CAUSAL_DIRECT, 0.8f);
    ASSERT_TRUE(causal_edge_exists(cg, 0, 1), "edge not added");

    int rc = remove_causal_edge(cg, 0, 1);
    ASSERT_TRUE(rc >= 0, "remove_edge failed");
    ASSERT_TRUE(!causal_edge_exists(cg, 0, 1), "edge should not exist after remove");

    causal_graph_destroy(cg);
    TEST_END();
}

void test_causal_graph_nonexistent(void) {
    TEST_START("causal_graph nonexistent edge");
    CausalGraph* cg = causal_graph_create(10, 20);
    ASSERT_NOT_NULL(cg, "create failed");

    ASSERT_TRUE(!causal_edge_exists(cg, 5, 9), "nonexistent reported as existing");
    CausalEdge* e = get_causal_edge(cg, 5, 9);
    ASSERT_NULL(e, "get nonexistent should return NULL");

    causal_graph_destroy(cg);
    TEST_END();
}

void test_causal_graph_sparse(void) {
    TEST_START("causal_graph sparse chain");
    CausalGraph* cg = causal_graph_create(20, 100);
    ASSERT_NOT_NULL(cg, "create failed");

    for (int i = 0; i < 3; i++)
        add_causal_edge(cg, i, i + 1, CAUSAL_DIRECT, 0.7f);

    ASSERT_TRUE(causal_edge_exists(cg, 0, 1), "chain 0→1 missing");
    ASSERT_TRUE(causal_edge_exists(cg, 1, 2), "chain 1→2 missing");
    ASSERT_TRUE(causal_edge_exists(cg, 2, 3), "chain 2→3 missing");

    causal_graph_destroy(cg);
    TEST_END();
}

int main(void) {
    printf("\n=== PivotMind Causal Reasoning Unit Tests ===\n\n");
    test_causal_graph_create();
    test_causal_graph_add_edge();
    test_causal_graph_remove_edge();
    test_causal_graph_nonexistent();
    test_causal_graph_sparse();
    printf("\n=== Results: %d run, %d passed, %d failed ===\n",
           tests_run, tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
