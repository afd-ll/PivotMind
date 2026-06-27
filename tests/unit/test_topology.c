/**
 * @file test_topology.c
 * @brief Unit tests for multi_topology.c — topology_walk_greedy, node ops
 */

#include "../include/common.h"
#include "../include/multi_topology.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0, tests_passed = 0, tests_failed = 0;

#define TEST_START(name) printf("Running: %s...", name); tests_run++;
#define TEST_END() tests_passed++; printf(" PASSED\n")
#define TEST_FAIL(msg) tests_failed++; printf(" FAILED: %s\n", msg);
#define ASSERT_TRUE(cond, msg) do { if (!(cond)) { TEST_FAIL(msg); return; } } while (0)
#define ASSERT_NOT_NULL(ptr, msg) ASSERT_TRUE((ptr) != NULL, msg)
#define ASSERT_EQUAL(a, b, msg) ASSERT_TRUE((a) == (b), msg)

/* ── Helpers ── */

static MasterTopology* build_walk_topo(int node_count) {
    MasterTopology* m = master_topology_create(11);
    if (!m) return NULL;
    master_add_sub_topology(m, TOPO_VOCABULARY, "词汇", 2000, 10);

    SubTopology* vt = m->sub_topologies[TOPO_VOCABULARY];
    /* Build a linear chain: 0-1-2-3-...-(n-1) */
    for (int i = 0; i < node_count; i++) {
        char name[32];
        snprintf(name, sizeof(name), "n%d", i);
        huarong_net_find_or_create_node(vt->net, name, NULL, 0, vt->node_hash);
    }
    for (int i = 0; i < node_count - 1; i++) {
        huarong_net_add_connection(vt->net, i, i + 1, 0.8f);
    }
    return m;
}

/* ── Tests ── */

void test_topology_walk_greedy_chain(void) {
    TEST_START("topology_walk_greedy chain");
    MasterTopology* m = build_walk_topo(10);
    ASSERT_NOT_NULL(m, "topo failed");
    SubTopology* vt = m->sub_topologies[TOPO_VOCABULARY];

    int path[32];
    float scores[32];
    unsigned char visited[2000] = {0};
    float intent = 1.0f;

    int len = topology_walk_greedy(vt, 0, path, scores, 5, visited, intent, m, NULL, NULL);
    ASSERT_TRUE(len > 0, "walk returned 0");
    ASSERT_TRUE(len <= 5, "walk exceeded max_len");

    /* Linear chain: should follow 0→1→2→... */
    ASSERT_EQUAL(path[0], 0, "first node should be start");
    for (int i = 1; i < len; i++) {
        ASSERT_TRUE(path[i] > path[i-1], "chain order broken");
    }

    master_topology_destroy(m);
    TEST_END();
}

void test_topology_walk_empty_graph(void) {
    TEST_START("topology_walk_greedy empty");
    MasterTopology* m = master_topology_create(11);
    if (!m) { TEST_FAIL("create"); return; }
    master_add_sub_topology(m, TOPO_VOCABULARY, "词汇", 500, 10);

    /* Single isolated node — no edges */
    SubTopology* vt = m->sub_topologies[TOPO_VOCABULARY];
    huarong_net_find_or_create_node(vt->net, "solo", NULL, 0, vt->node_hash);

    int path[32];
    float scores[32];
    unsigned char visited[500] = {0};
    int len = topology_walk_greedy(vt, 0, path, scores, 5, visited, 0.5f, m, NULL, NULL);
    ASSERT_TRUE(len >= 0 && len <= 1, "empty graph walk unexpected");

    master_topology_destroy(m);
    TEST_END();
}

void test_node_find_or_create(void) {
    TEST_START("huarong_net_find_or_create");
    MasterTopology* m = master_topology_create(11);
    if (!m) { TEST_FAIL("create"); return; }
    master_add_sub_topology(m, TOPO_VOCABULARY, "词汇", 500, 10);
    SubTopology* vt = m->sub_topologies[TOPO_VOCABULARY];

    /* Create */
    ReasoningNode* n = huarong_net_find_or_create_node(vt->net, "test", NULL, 0, vt->node_hash);
    ASSERT_NOT_NULL(n, "create failed");
    ASSERT_EQUAL(n->node_id, 0, "first node id != 0");

    /* Find same */
    ReasoningNode* n2 = huarong_net_find_or_create_node(vt->net, "test", NULL, 0, vt->node_hash);
    ASSERT_NOT_NULL(n2, "find failed");
    ASSERT_EQUAL(n2->node_id, 0, "find returned different id");

    master_topology_destroy(m);
    TEST_END();
}

int main(void) {
    printf("\n=== PivotMind Topology Unit Tests ===\n\n");

    test_node_find_or_create();
    test_topology_walk_empty_graph();
    test_topology_walk_greedy_chain();

    printf("\n=== Results: %d run, %d passed, %d failed ===\n",
           tests_run, tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
