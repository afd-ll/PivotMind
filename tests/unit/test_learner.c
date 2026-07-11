/**
 * @file test_learner.c
 * @brief Unit tests for autonomic_learner.c — Hebbian online learning
 */

#include "common.h"
#include "autonomic_learner.h"
#include "multi_topology.h"
#include "memory_system.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0, tests_passed = 0, tests_failed = 0;

#define TEST_START(name) printf("Running: %s...", name); tests_run++;
#define TEST_END() tests_passed++; printf(" PASSED\n")
#define TEST_FAIL(msg) tests_failed++; printf(" FAILED: %s\n", msg);
#define ASSERT_TRUE(cond, msg) do { if (!(cond)) { TEST_FAIL(msg); return; } } while (0)
#define ASSERT_NOT_NULL(ptr, msg) ASSERT_TRUE((ptr) != NULL, msg)

/* ── Helpers ── */

static MasterTopology* build_learner_topo(void) {
    MasterTopology* m = master_topology_create(11);
    if (!m) return NULL;
    master_add_sub_topology(m, TOPO_VOCABULARY, "词汇", 2000, 10);

    /* Seed vocabulary: input chars and response chars */
    SubTopology* vt = m->sub_topologies[TOPO_VOCABULARY];
    const char* seeds[] = {"A", "B", "C", "D", "E", "F",
                            "学", "习", "科", "技", "人", "工"};
    for (int i = 0; i < 12; i++) {
        huarong_net_find_or_create_node(vt->net, seeds[i], NULL, 0, vt->node_hash);
    }
    return m;
}

/* ── Tests ── */

void test_autonomic_state_init(void) {
    TEST_START("autonomic_state_init/destroy");
    AutonomicState state;
    autonomic_state_init(&state);
    /* verify no crash, state initialised */
    ASSERT_TRUE(state.initialized, "state not initialized");
    autonomic_state_destroy(&state);
    TEST_END();
}

void test_autonomic_learn_basic(void) {
    TEST_START("autonomic_learn_from_dialog");
    MasterTopology* m = build_learner_topo();
    ASSERT_NOT_NULL(m, "topo failed");

    MemorySystem* mem = memory_system_create(100, 50, 200);
    ASSERT_NOT_NULL(mem, "memory failed");

    AutonomicState state;
    autonomic_state_init(&state);

    /* Learn a simple Q&A pair */
    autonomic_learn_from_dialog(m, "ABC", "DEF", &state, NULL, mem);
    ASSERT_TRUE(state.initialized, "learn caused error");

    /* Check that connections were created (ABC→DEF co-occurrence) */
    int total_edges = 0;
    SubTopology* vt = m->sub_topologies[TOPO_VOCABULARY];
    for (int i = 0; i < vt->net->node_count; i++) {
        ReasoningNode* n = vt->net->nodes[i];
        if (n) total_edges += n->edge_count;
    }
    /* After learning, some edges should exist */
    ASSERT_TRUE(total_edges > 0, "no edges created after learning");

    /* Learn another pair */
    autonomic_learn_from_dialog(m, "科学", "学习", &state, NULL, mem);
    ASSERT_TRUE(state.initialized, "second learn caused error");

    autonomic_state_destroy(&state);
    memory_system_destroy(mem);
    master_topology_destroy(m);
    TEST_END();
}

void test_autonomic_learn_reinforcement(void) {
    TEST_START("autonomic_learn reinforcement");
    MasterTopology* m = build_learner_topo();
    ASSERT_NOT_NULL(m, "topo failed");

    MemorySystem* mem = memory_system_create(100, 50, 200);
    ASSERT_NOT_NULL(mem, "memory failed");

    AutonomicState state;
    autonomic_state_init(&state);

    /* Learn same pair multiple times — edges should strengthen */
    for (int j = 0; j < 3; j++) {
        autonomic_learn_from_dialog(m, "ABC", "DEF", &state, NULL, mem);
    }

    /* Verify edges exist */
    SubTopology* vt = m->sub_topologies[TOPO_VOCABULARY];
    int total_edges = 0;
    float max_weight = 0.0f;
    for (int i = 0; i < vt->net->node_count; i++) {
        ReasoningNode* n = vt->net->nodes[i];
        if (!n) continue;
        total_edges += n->edge_count;
        for (int c = 0; c < n->edge_count; c++) {
            if (n->edges && n->edges[c].weight > max_weight)
                max_weight = n->edges[c].weight;
        }
    }
    ASSERT_TRUE(total_edges > 0, "no edges after reinforcement");
    ASSERT_TRUE(max_weight > 0.01f, "edges have zero weight");

    autonomic_state_destroy(&state);
    memory_system_destroy(mem);
    master_topology_destroy(m);
    TEST_END();
}

int main(void) {
    printf("\n=== PivotMind Autonomic Learner Unit Tests ===\n\n");

    test_autonomic_state_init();
    test_autonomic_learn_basic();
    test_autonomic_learn_reinforcement();

    printf("\n=== Results: %d run, %d passed, %d failed ===\n",
           tests_run, tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
