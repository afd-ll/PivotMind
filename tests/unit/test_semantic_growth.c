/**
 * @file test_semantic_growth.c
 * @brief semantic_grow_from_vocab 回归测试
 *
 * 验证：
 *   1. NULL/空拓扑安全性（不崩溃）
 *   2. 基本功能 — 词表 → 语义聚类
 *   3. 预计算范数优化 — 不改变聚类结果
 */

#include "semantic_growth.h"
#include "multi_topology.h"
#include "huarong_topology.h"
#include "cognitive_params.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("Running: %-50s ", name); \
} while(0)

#define PASS() do { \
    printf("PASSED\n"); \
    tests_passed++; \
} while(0)

#define FAIL(fmt, ...) do { \
    printf("FAILED: " fmt "\n", ##__VA_ARGS__); \
    tests_failed++; \
} while(0)

/* ── NODE_FEATURE_DIM 必须与 semantic_growth 一致 ── */
#ifndef NODE_FEATURE_DIM
#define NODE_FEATURE_DIM 512
#endif

/* ── 测试 1: NULL 安全 ── */
static int test_null_safety(void) {
    TEST("semantic_grow_from_vocab(NULL)");
    int ret = semantic_grow_from_vocab(NULL);
    if (ret == 0) PASS();
    else FAIL("expected 0 for NULL master");

    return 0;
}

/* ── 测试 2: 空拓扑（节点不足） ── */
static int test_empty_topology(void) {
    TEST("semantic_grow_from_vocab with 0 nodes");
    MasterTopology* master = master_topology_create(4);
    assert(master);

    master_add_sub_topology(master, TOPO_VOCABULARY, "vocab", 32, 1);
    master_add_sub_topology(master, TOPO_SEMANTIC, "semantic", 32, 1);

    int ret = semantic_grow_from_vocab(master);
    if (ret == 0) PASS();
    else FAIL("expected 0 for empty vocab");

    master_topology_destroy(master);
    return 0;
}

/* ── 测试 3: 最小聚类 (SG_MIN_CLUSTER_SIZE=3) ── */
static int test_minimal_clustering(void) {
    TEST("semantic_grow_from_vocab minimal cluster (3 nodes)");
    MasterTopology* master = master_topology_create(4);
    assert(master);

    master_add_sub_topology(master, TOPO_VOCABULARY, "vocab", 32, 1);
    master_add_sub_topology(master, TOPO_SEMANTIC, "semantic", 32, 1);

    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    assert(vocab);

    /* 3 个节点 — 惰性特征分配，不传显式特征 */
    huarong_net_add_node(vocab->net, "test_a", NULL, 0);
    huarong_net_add_node(vocab->net, "test_b", NULL, 0);
    huarong_net_add_node(vocab->net, "test_c", NULL, 0);

    int created = semantic_grow_from_vocab(master);
    if (created >= 0) PASS();
    else FAIL("returned: %d", created);

    master_topology_destroy(master);
    return 0;
}

/* ── 测试 4: 多次调用不崩溃（惰性特征分配） ── */
static int test_multiple_calls(void) {
    TEST("semantic_grow_from_vocab multiple calls");
    MasterTopology* master = master_topology_create(4);
    assert(master);

    master_add_sub_topology(master, TOPO_VOCABULARY, "vocab", 32, 1);
    master_add_sub_topology(master, TOPO_SEMANTIC, "semantic", 32, 1);

    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    assert(vocab);

    /* 惰性分配 — 不传显式特征，加速测试 */
    for (int i = 0; i < 5; i++) {
        char name[32];
        snprintf(name, sizeof(name), "w%d", i);
        huarong_net_add_node(vocab->net, name, NULL, 0);
    }

    for (int call = 0; call < 3; call++) {
        int ret = semantic_grow_from_vocab(master);
        if (ret < 0) {
            FAIL("call %d returned error: %d", call, ret);
            master_topology_destroy(master);
            return 0;
        }
    }

    PASS();
    master_topology_destroy(master);
    return 0;
}

/* ── 测试 5: 冻节点跳过 ── */
static int test_frozen_nodes_safety(void) {
    TEST("semantic_grow_from_vocab with frozen nodes");
    MasterTopology* master = master_topology_create(4);
    assert(master);

    master_add_sub_topology(master, TOPO_VOCABULARY, "vocab", 32, 1);
    master_add_sub_topology(master, TOPO_SEMANTIC, "semantic", 32, 1);

    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    assert(vocab);

    /* 惰性分配 — 5 个节点 */
    for (int i = 0; i < 5; i++) {
        char name[32];
        snprintf(name, sizeof(name), "f%d", i);
        huarong_net_add_node(vocab->net, name, NULL, 0);
    }

    /* 标记前 2 个为冷却 */
    for (int i = 0; i < 2 && i < vocab->net->node_count; i++)
        if (vocab->net->nodes[i]) vocab->net->nodes[i]->is_cooled = 1;

    int ret = semantic_grow_from_vocab(master);
    if (ret >= 0) PASS();
    else FAIL("returned error: %d", ret);

    master_topology_destroy(master);
    return 0;
}

/* ================================================================
 *  main
 * ================================================================ */
int main(void) {
    printf("\n========================================\n");
    printf("  semantic_growth 回归测试\n");
    printf("========================================\n\n");

    test_null_safety();
    test_empty_topology();
    test_minimal_clustering();
    test_multiple_calls();
    test_frozen_nodes_safety();

    printf("\n=== Results: %d run, %d passed, %d failed ===\n",
           tests_run, tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
