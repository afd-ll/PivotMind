/**
 * @file test_dialog.c
 * @brief Unit tests for dialog_system.c — input parsing + full system lifecycle
 */

#include "common.h"
#include "dialog_system.h"
#include "multi_topology.h"
#include "memory_system.h"
#include "causal_reasoning.h"
#include "active_learner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0, tests_passed = 0, tests_failed = 0;

#define TEST_START(name) printf("Running: %s...", name); tests_run++;
#define TEST_END() tests_passed++; printf(" PASSED\n")
#define TEST_FAIL(msg) tests_failed++; printf(" FAILED: %s\n", msg);
#define ASSERT_TRUE(cond, msg) do { if (!(cond)) { TEST_FAIL(msg); return; } } while (0)
#define ASSERT_NOT_NULL(ptr, msg) ASSERT_TRUE((ptr) != NULL, msg)

/* ── Helpers: build a minimal but complete topology ── */

static MasterTopology* build_minimal_topology(void) {
    MasterTopology* m = master_topology_create(11);
    if (!m) return NULL;

    /* Create all sub-topologies (required by dialog_system_create) */
    master_add_sub_topology(m, TOPO_VOCABULARY, "词汇", 1000, 10);
    master_add_sub_topology(m, TOPO_SEMANTIC,  "语义", 1000, 9);
    master_add_sub_topology(m, TOPO_EMOTION,   "情绪", 500,  7);
    master_add_sub_topology(m, TOPO_SYNTAX,    "语法", 500,  8);
    master_add_sub_topology(m, TOPO_CONTEXT,   "上下文", 500, 6);
    master_add_sub_topology(m, TOPO_DOMAIN,    "领域", 500,  5);
    master_add_sub_topology(m, TOPO_PRAGMA,    "语用", 500,  5);
    master_add_sub_topology(m, TOPO_CULTURE,   "文化", 500,  4);
    master_add_sub_topology(m, TOPO_CONCEPT,   "概念", 500,  5);
    master_add_sub_topology(m, TOPO_MASTER,    "主拓扑", 500, 10);
    master_add_sub_topology(m, TOPO_TEMPLATE,  "模板", 500,  6);

    /* Seed vocabulary nodes */
    SubTopology* vocab = m->sub_topologies[TOPO_VOCABULARY];
    const char* words[] = {"意", "识", "学", "习", "大", "脑", "科", "学思", "网", "络"};
    for (int i = 0; i < 10; i++) {
        huarong_net_find_or_create_node(vocab->net, words[i], NULL, 0, vocab->node_hash);
    }
    return m;
}

static MasterTopology* build_small_topology(int vocab_nodes) {
    MasterTopology* m = master_topology_create(11);
    if (!m) return NULL;

    /* minimum: only vocab topology for lightweight tests */
    master_add_sub_topology(m, TOPO_VOCABULARY, "词汇", 1000, 10);

    SubTopology* vocab = m->sub_topologies[TOPO_VOCABULARY];
    const char* words_a[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j",
                              "科", "学", "技", "术", "人", "工", "智", "能"};
    int count = vocab_nodes < 18 ? vocab_nodes : 18;
    for (int i = 0; i < count; i++) {
        huarong_net_find_or_create_node(vocab->net, words_a[i], NULL, 0, vocab->node_hash);
    }
    return m;
}

/* ── Tests ── */

void test_dialog_input_create(void) {
    TEST_START("dialog_input_create/destroy");
    DialogInput* input = dialog_input_create(
        "\xe4\xbb\x80\xe4\xb9\x88\xe6\x98\xaf\xe6\x84\x8f\xe8\xaf\x86");
    ASSERT_NOT_NULL(input, "returned NULL");
    ASSERT_NOT_NULL(input->original, "original is NULL");
    ASSERT_TRUE(input->token_count > 0, "token_count > 0");
    ASSERT_NOT_NULL(input->tokens, "tokens is NULL");
    dialog_input_destroy(input);
    TEST_END();
}

void test_dialog_input_empty(void) {
    TEST_START("dialog_input empty");
    DialogInput* input = dialog_input_create("");
    ASSERT_NOT_NULL(input, "empty should create");
    dialog_input_destroy(input);
    TEST_END();
}

void test_dialog_input_null(void) {
    TEST_START("dialog_input NULL");
    DialogInput* input = dialog_input_create(NULL);
    if (input) dialog_input_destroy(input);
    TEST_END();
}

void test_dialog_reasoning_create(void) {
    TEST_START("dialog_reasoning_create/destroy");
    MasterTopology* m = build_small_topology(4);
    ASSERT_NOT_NULL(m, "topo failed");

    DialogInput* input = dialog_input_create("abc");
    ASSERT_NOT_NULL(input, "input failed");

    float intent[11] = {0};
    intent[TOPO_VOCABULARY] = 1.0f;

    DialogReasoning* r = dialog_reasoning_create(input, m, intent);
    ASSERT_NOT_NULL(r, "reasoning create failed");

    dialog_reasoning_destroy(r);
    dialog_input_destroy(input);
    master_topology_destroy(m);
    TEST_END();
}

void test_dialog_system_full_lifecycle(void) {
    TEST_START("dialog_system create/destroy");
    MasterTopology* m = build_minimal_topology();
    ASSERT_NOT_NULL(m, "topo failed");

    MemorySystem* mem = memory_system_create(100, 50, 200);
    ASSERT_NOT_NULL(mem, "memory failed");

    CausalGraph* cg = causal_graph_create(100, 500);
    ASSERT_NOT_NULL(cg, "causal_graph failed");

    ActiveLearner* al = active_learner_create(m, mem);

    DialogSystem* ds = dialog_system_create(m, mem, cg, al);
    ASSERT_NOT_NULL(ds, "dialog_system_create failed");

    /* basic turn — verify no crash */
    DialogReasoning* out = NULL;
    char* reply = dialog_process(ds, "\xe5\xad\xa6\xe4\xb9\xa0", &out);
    if (reply) free(reply);
    if (out) dialog_reasoning_destroy(out);

    dialog_system_destroy(ds);
    if (al) active_learner_destroy(al);
    causal_graph_destroy(cg);
    memory_system_destroy(mem);
    master_topology_destroy(m);
    TEST_END();
}

int main(void) {
    printf("\n=== PivotMind Dialog System Unit Tests ===\n\n");

    test_dialog_input_create();
    test_dialog_input_empty();
    test_dialog_input_null();
    test_dialog_reasoning_create();

    printf("\n=== Results: %d run, %d passed, %d failed ===\n",
           tests_run, tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
