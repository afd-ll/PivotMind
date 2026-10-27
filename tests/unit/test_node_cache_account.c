/**
 * @file test_node_cache_account.c
 * @brief BG-51 记账对账 — 冻结/解冻的边数必须进账，且口径与脑干日志严格一致。
 *
 * 背景（BG-51，2026-09-26 线上观测 · 2026-10-21 拍板「对话时动态回迁」）：
 *   冻结（node_cache_freeze）会把 node->edge_count 清零、边移出内存 ⇒
 *   `[堆监控] 连接`（= Σedge_count）必然单调下降。这是设计，不是缺陷；
 *   但红线「丢弃必须记账」当时不合规 —— 196 次冻结、约 1769 条边移出内存，账是空的。
 *   ac49020 补了 NodeCache.total_edges_frozen（/status 的 edges_frozen 露出）。
 *
 * 本测试锁住三条判据（防止以后被改坏而无人发现）：
 *   ① 记账累加 == 各次实际移出的边数（对账等式：日志 M 与 edges_frozen 同源）
 *   ② 冻结/解冻计数幂等 —— 对已冻节点再冻、对已热节点再解冻，计数一律不动
 *   ③ 解冻后边数完整恢复（thaw 不丢边，印证「冻的是内存不是数据」）
 *
 * 对账口径说明：src/brainstem.c 的 edges_moved 与 node_cache.c 的
 * total_edges_frozen 都在「freeze 返回 0」之后累加同一个 ne ⇒ 二者恒等。
 * 本测试在测试内复刻 brainstem 的取数逻辑（读 edge_count → freeze → 累加 ne），
 * 断言 Σne 与 nc->total_edges_frozen 的增量相等。
 */

#include "common.h"
#include "node_cache.h"
#include "huarong_topology.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NC_FILE  "build/test_node_cache_account.dat"
#define NC_CAP   64
#define FEAT_DIM 8

static int tests_run = 0, tests_passed = 0, tests_failed = 0;

#define TEST_START(name) printf("Running: %s...", name); tests_run++
#define TEST_END()   do { tests_passed++; printf(" PASSED\n"); } while (0)
#define TEST_FAIL(m) do { tests_failed++; printf(" FAILED: %s\n", m); } while (0)
#define ASSERT_TRUE(cond, msg) do { if (!(cond)) { TEST_FAIL(msg); return; } } while (0)
#define ASSERT_NOT_NULL(p, msg) ASSERT_TRUE((p) != NULL, msg)
#define ASSERT_EQUAL(a, b, msg) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { printf(" FAILED: %s (got %lld, want %lld)\n", msg, _a, _b); \
                    tests_failed++; return; } \
} while (0)

/* ---------- 夹具 ---------- */

static HuarongTopologyNet* mk_net(int n) {
    HuarongTopologyNet* net = huarong_net_create((size_t)NC_CAP);
    if (!net) return NULL;
    float feat[FEAT_DIM];
    char name[32];
    for (int i = 0; i < n; i++) {
        memset(feat, 0, sizeof(feat));
        feat[i % FEAT_DIM] = 1.0f;
        snprintf(name, sizeof(name), "bg51_node_%d", i);
        if (!huarong_net_add_node(net, name, feat, FEAT_DIM)) return NULL;
    }
    return net;
}

/* 给 node 挂 n 条边，target 指向 net 里其它节点（避开自己） */
static int attach_edges(HuarongTopologyNet* net, ReasoningNode* node, int n) {
    if (n <= 0) return 0;
    node->edges = (Edge*)calloc((size_t)n, sizeof(Edge));
    if (!node->edges) return -1;
    node->edge_capacity = n;
    node->edge_count = n;
    int k = 0;
    for (int i = 0; i < net->node_count && k < n; i++) {
        ReasoningNode* t = net->nodes[i];
        if (!t || t == node) continue;
        node->edges[k].target = t;
        node->edges[k].weight = 0.5f;
        node->edges[k].motivational_bias = 0.0f;
        node->edges[k].confidence = 0.5f;
        k++;
    }
    return k == n ? 0 : -1;
}

/* 复刻 src/brainstem.c:646-656 的取数口径：先读 edge_count，freeze 成功才累加 */
static long freeze_like_brainstem(NodeCache* nc, HuarongTopologyNet* net,
                                  ReasoningNode* node) {
    if (!node || node->is_cooled) return 0;
    if (node->edge_count <= 0) return 0;
    long ne = (long)node->edge_count;
    if (node_cache_freeze(nc, net, node) == 0) return ne;
    return 0;
}

/* ---------- ① 记账累加 == 实际移出边数 ---------- */

static void test_accounting_equals_edges_moved(void) {
    TEST_START("edge accounting == edges moved (BG-51 对账等式)");

    remove(NC_FILE);
    HuarongTopologyNet* net = mk_net(8);   /* 需 > 最大单节点边数(5)，且要避开自己 */
    ASSERT_NOT_NULL(net, "net create failed");

    NodeCache* nc = node_cache_create(NC_FILE, NC_CAP);
    ASSERT_NOT_NULL(nc, "node_cache_create failed");

    ASSERT_EQUAL(nc->total_edges_frozen, 0, "fresh cache should start at 0 edges");
    ASSERT_EQUAL(nc->total_freezes, 0, "fresh cache should start at 0 freezes");

    /* 三个节点挂不同边数：5 / 3 / 1（合计 9） */
    const int plan[3] = {5, 3, 1};
    long expected_edges = 0, logged_edges = 0;
    for (int i = 0; i < 3; i++) {
        ReasoningNode* nd = net->nodes[i];
        ASSERT_EQUAL(attach_edges(net, nd, plan[i]), 0, "attach edges failed");
        expected_edges += plan[i];
        logged_edges += freeze_like_brainstem(nc, net, nd);
    }

    /* 对账等式：脑干日志累加的 M 必须等于 edges_frozen 的增量 */
    ASSERT_EQUAL(logged_edges, expected_edges, "brainstem-side M should equal sum(edge_count)");
    ASSERT_EQUAL(nc->total_edges_frozen, expected_edges,
                 "total_edges_frozen must equal total edges moved out");
    ASSERT_EQUAL(nc->total_freezes, 3, "total_freezes should count 3 nodes");

    /* 冻完边清零、标记冷却（这就是「连接数单调下降」的机制本身） */
    for (int i = 0; i < 3; i++) {
        ReasoningNode* nd = net->nodes[i];
        ASSERT_EQUAL(nd->edge_count, 0, "edge_count must be zeroed after freeze");
        ASSERT_EQUAL(nd->is_cooled, 1, "node must be marked cooled");
        ASSERT_TRUE(nd->edges == NULL, "edge array must be released");
    }

    node_cache_destroy(nc);
    huarong_net_destroy(net);
    TEST_END();
}

/* ---------- ② 幂等：重复冻结/解冻不动计数 ---------- */

static void test_idempotent_counters(void) {
    TEST_START("freeze/thaw counters are idempotent");

    remove(NC_FILE);
    HuarongTopologyNet* net = mk_net(6);   /* 需 > 4 条边 */
    ASSERT_NOT_NULL(net, "net create failed");
    NodeCache* nc = node_cache_create(NC_FILE, NC_CAP);
    ASSERT_NOT_NULL(nc, "node_cache_create failed");

    ReasoningNode* a = net->nodes[0];
    ASSERT_EQUAL(attach_edges(net, a, 4), 0, "attach failed");

    ASSERT_EQUAL(freeze_like_brainstem(nc, net, a), 4, "first freeze moves 4 edges");
    ASSERT_EQUAL(nc->total_edges_frozen, 4, "edges_frozen=4");
    ASSERT_EQUAL(nc->total_freezes, 1, "freezes=1");

    /* 再冻一次：直接调底层 freeze（绕过 brainstem 的 is_cooled 预判），必须返回 -1 */
    ASSERT_EQUAL(node_cache_freeze(nc, net, a), -1, "re-freeze should be rejected");
    ASSERT_EQUAL(nc->total_edges_frozen, 4, "re-freeze must not add to the ledger");
    ASSERT_EQUAL(nc->total_freezes, 1, "re-freeze must not bump freezes");

    /* 解冻一次 */
    ASSERT_EQUAL(node_cache_thaw(nc, net, a, 0), 0, "thaw failed");
    ASSERT_EQUAL(nc->total_thaws, 1, "thaws=1");
    ASSERT_EQUAL(a->edge_count, 4, "thaw must restore all 4 edges");
    ASSERT_EQUAL(a->is_cooled, 0, "node must be hot after thaw");

    /* 再解冻一次：已热 ⇒ 必须拒绝且不计数 */
    ASSERT_EQUAL(node_cache_thaw(nc, net, a, 0), -1, "re-thaw should be rejected");
    ASSERT_EQUAL(nc->total_thaws, 1, "re-thaw must not bump thaws");

    node_cache_destroy(nc);
    huarong_net_destroy(net);
    TEST_END();
}

/* ---------- ③ 冻→解 往返后边数与边权完整 ---------- */

static void test_roundtrip_preserves_edges(void) {
    TEST_START("freeze->thaw roundtrip preserves edges");

    remove(NC_FILE);
    HuarongTopologyNet* net = mk_net(4);
    ASSERT_NOT_NULL(net, "net create failed");
    NodeCache* nc = node_cache_create(NC_FILE, NC_CAP);
    ASSERT_NOT_NULL(nc, "node_cache_create failed");

    ReasoningNode* a = net->nodes[0];
    ASSERT_EQUAL(attach_edges(net, a, 2), 0, "attach failed");
    float w0 = a->edges[0].weight, w1 = a->edges[1].weight;


    ASSERT_EQUAL(freeze_like_brainstem(nc, net, a), 2, "freeze moves 2 edges");
    ASSERT_EQUAL(nc->total_edges_frozen, 2, "edges_frozen=2 after freeze");

    ASSERT_EQUAL(node_cache_thaw(nc, net, a, 0), 0, "thaw failed");
    ASSERT_EQUAL(a->edge_count, 2, "edge_count restored");
    ASSERT_TRUE(a->edges != NULL, "edge array re-allocated");
    ASSERT_TRUE(a->edges[0].target != NULL && a->edges[1].target != NULL,
                "edge targets re-resolved");
    ASSERT_TRUE(a->edges[0].weight > w0 - 1e-6f && a->edges[0].weight < w0 + 1e-6f,
                "edge weight preserved");
    ASSERT_TRUE(a->edges[1].weight > w1 - 1e-6f && a->edges[1].weight < w1 + 1e-6f,
                "edge weight preserved (2)");

    /* 累计账不会因解冻而回退 —— 它记的是「历史上移出过多少」，不是当前值 */
    ASSERT_EQUAL(nc->total_edges_frozen, 2, "cumulative ledger must not decrease on thaw");

    node_cache_destroy(nc);
    huarong_net_destroy(net);
    TEST_END();
}

/* [BG-64] 冻结必须记下「冻了哪些节点」—— 原来只记个数，无法追溯、无法定向验证解冻 */
static void test_freeze_records_node_ids(void) {
    TEST_START("BG-64: freeze records node ids (ring buffer)");

    remove(NC_FILE);
    HuarongTopologyNet* net = mk_net(8);
    ASSERT_NOT_NULL(net, "net create failed");
    NodeCache* nc = node_cache_create(NC_FILE, NC_CAP);
    ASSERT_NOT_NULL(nc, "node_cache_create failed");

    int picked[5] = { 0 };
    for (int i = 0; i < 5; i++) {
        ReasoningNode* nd = net->nodes[i];
        ASSERT_NOT_NULL(nd, "node missing");
        picked[i] = nd->node_id;
        ASSERT_TRUE(attach_edges(net, nd, 2) >= 0, "attach edges failed");
        ASSERT_EQUAL(node_cache_freeze(nc, net, nd), 0, "freeze should succeed");
    }

    /* 最新在最前：取回来的顺序应是 picked[4..0] */
    int out[64];
    int n = node_cache_recent_frozen(nc, out, 64);
    ASSERT_EQUAL(n, 5, "should record 5 frozen node ids");
    for (int i = 0; i < 5; i++) {
        ASSERT_EQUAL(out[i], picked[4 - i], "ring buffer must be newest-first");
    }

    /* 未使用槽位初始化为 -1：总数不应超过实际冻结数 */
    ASSERT_TRUE(n <= 5, "unused slots must not be reported");

    node_cache_destroy(nc);
    huarong_net_destroy(net);
    TEST_END();
}

int main(void) {
    printf("\n=== BG-51 记账对账（node_cache freeze/thaw）===\n\n");

    /* 顺序故意保持「多节点在前、少节点在后」：BG-63 未修前，后跑的用例会复用到
     * 上一轮已冻结的节点内存，is_cooled 脏成 1 而失败 —— 这个顺序本身是回归哨兵。 */
    test_accounting_equals_edges_moved();
    test_idempotent_counters();
    test_roundtrip_preserves_edges();
    test_freeze_records_node_ids();

    printf("\n--- %d tests, %d passed, %d failed ---\n",
           tests_run, tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
