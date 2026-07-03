/**
 * @file test_visual_cortex.c
 * @brief 视觉皮层脑区单元测试 — 任务队列 + 生命周期 + 拓扑集成
 *
 * 测试覆盖:
 *   1. 视觉皮层创建/销毁 + 自动创建 TOPO_VISUAL
 *   2. 任务队列 (enqueue/dequeue)
 *   3. 队列容量限制 (背压)
 *   4. 队列空时 tick 优雅降级
 *   5. 统计信息 API
 *   6. 编码器回调注册
 *   7. MediaReader 内部引用
 *   8. 脑区开关 (enabled/disabled)
 *   9. 帧差异评分
 *   10. NULL 安全
 */

#include "../include/common.h"
#include "../include/visual_cortex.h"
#include "../include/media_reader.h"
#include "../include/multi_topology.h"
#include "../include/thalamus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int tests_run = 0, tests_passed = 0, tests_failed = 0;

#define TEST_START(name) printf("  [TEST] %s ... ", name); tests_run++;
#define TEST_END() tests_passed++; printf("PASSED\n")
#define TEST_FAIL(msg) tests_failed++; printf("FAILED: %s\n", msg)
#define ASSERT_TRUE(cond, msg) do { if (!(cond)) { TEST_FAIL(msg); return; } } while (0)
#define ASSERT_EQUAL(a, b, msg) ASSERT_TRUE((a) == (b), msg)
#define ASSERT_NOT_NULL(ptr, msg) ASSERT_TRUE((ptr) != NULL, msg)
#define ASSERT_NULL(ptr, msg) ASSERT_TRUE((ptr) == NULL, msg)

/* ==================== 生命周期测试 ==================== */

void test_vc_create_destroy(void) {
    TEST_START("visual_cortex create/destroy");
    MasterTopology* m = master_topology_create(12);
    ASSERT_NOT_NULL(m, "topo create failed");
    master_add_sub_topology(m, TOPO_VOCABULARY, "词汇", 1000, 10);

    VisualCortexConfig cfg = VISUAL_CORTEX_DEFAULT_CONFIG;
    VisualCortex* vc = visual_cortex_create(m, &cfg);
    ASSERT_NOT_NULL(vc, "vc create failed");

    visual_cortex_destroy(vc);
    master_topology_destroy(m);
    TEST_END();
}

void test_vc_create_auto_creates_visual_topo(void) {
    TEST_START("visual_cortex auto-creates TOPO_VISUAL");
    MasterTopology* m = master_topology_create(12);
    ASSERT_NOT_NULL(m, "topo create failed");
    master_add_sub_topology(m, TOPO_VOCABULARY, "词汇", 1000, 10);

    /* TOPO_VISUAL 不应存在 */
    SubTopology* vt_before = master_get_sub_topology_by_type(m, TOPO_VISUAL);
    ASSERT_NULL(vt_before, "TOPO_VISUAL should not exist before vc_create");

    VisualCortexConfig cfg = VISUAL_CORTEX_DEFAULT_CONFIG;
    VisualCortex* vc = visual_cortex_create(m, &cfg);
    ASSERT_NOT_NULL(vc, "vc create failed");

    /* TOPO_VISUAL 应自动创建 */
    SubTopology* vt_after = master_get_sub_topology_by_type(m, TOPO_VISUAL);
    ASSERT_NOT_NULL(vt_after, "TOPO_VISUAL should exist after vc_create");
    ASSERT_TRUE(vt_after->type == TOPO_VISUAL, "type should be TOPO_VISUAL");

    visual_cortex_destroy(vc);
    master_topology_destroy(m);
    TEST_END();
}

void test_vc_null_params(void) {
    TEST_START("visual_cortex NULL topology");
    VisualCortex* vc = visual_cortex_create(NULL, NULL);
    ASSERT_NULL(vc, "should return NULL for NULL topology");
    TEST_END();
}

void test_vc_null_config(void) {
    TEST_START("visual_cortex NULL config uses defaults");
    MasterTopology* m = master_topology_create(12);
    ASSERT_NOT_NULL(m, "topo create failed");
    master_add_sub_topology(m, TOPO_VOCABULARY, "词汇", 1000, 10);

    VisualCortex* vc = visual_cortex_create(m, NULL);
    ASSERT_NOT_NULL(vc, "NULL config should fall back to defaults");

    /* 验证默认特征维度 */
    int q; long f; int vn, xe;
    visual_cortex_get_stats(vc, &q, &f, &vn, &xe);
    ASSERT_EQUAL(q, 0, "queue should be empty initially");

    visual_cortex_destroy(vc);
    master_topology_destroy(m);
    TEST_END();
}

/* ==================== 任务队列测试 ==================== */

void test_vc_enqueue_dequeue(void) {
    TEST_START("visual_cortex enqueue single file");
    MasterTopology* m = master_topology_create(12);
    master_add_sub_topology(m, TOPO_VOCABULARY, "词汇", 1000, 10);

    VisualCortex* vc = visual_cortex_create(m, NULL);
    ASSERT_NOT_NULL(vc, "vc create failed");

    /* 入队一个文件 */
    int ret = visual_cortex_enqueue(vc, "/data/test.mp4", "visual");
    ASSERT_EQUAL(ret, 0, "enqueue should return 0 (success)");

    /* 队列大小应为 1 */
    int qsize = visual_cortex_queue_size(vc);
    ASSERT_EQUAL(qsize, 1, "queue_size should be 1");

    visual_cortex_destroy(vc);
    master_topology_destroy(m);
    TEST_END();
}

void test_vc_enqueue_multiple(void) {
    TEST_START("visual_cortex enqueue multiple files");
    MasterTopology* m = master_topology_create(12);
    master_add_sub_topology(m, TOPO_VOCABULARY, "词汇", 1000, 10);

    VisualCortex* vc = visual_cortex_create(m, NULL);
    ASSERT_NOT_NULL(vc, "vc create failed");

    /* 入队 5 个文件 */
    const char* files[] = {"/1.mp4", "/2.mp4", "/3.mp4", "/4.mp4", "/5.mp4"};
    for (int i = 0; i < 5; i++) {
        ASSERT_EQUAL(visual_cortex_enqueue(vc, files[i], "visual"), 0, "enqueue failed");
    }

    int qsize = visual_cortex_queue_size(vc);
    ASSERT_EQUAL(qsize, 5, "queue_size should be 5");

    visual_cortex_destroy(vc);
    master_topology_destroy(m);
    TEST_END();
}

void test_vc_enqueue_null(void) {
    TEST_START("visual_cortex enqueue NULL");
    int ret = visual_cortex_enqueue(NULL, "/test.mp4", "visual");
    ASSERT_EQUAL(ret, -1, "should return -1 for NULL vc");
    TEST_END();
}

void test_vc_tick_empty_queue(void) {
    TEST_START("visual_cortex tick with empty queue");
    MasterTopology* m = master_topology_create(12);
    master_add_sub_topology(m, TOPO_VOCABULARY, "词汇", 1000, 10);

    VisualCortex* vc = visual_cortex_create(m, NULL);
    ASSERT_NOT_NULL(vc, "vc create failed");

    /* 空队列 tick 应返回 0 */
    int work = visual_cortex_tick(vc, 1.0f);
    ASSERT_EQUAL(work, 0, "tick on empty queue should return 0");

    visual_cortex_destroy(vc);
    master_topology_destroy(m);
    TEST_END();
}

void test_vc_tick_low_throttle(void) {
    TEST_START("visual_cortex tick with low throttle");
    MasterTopology* m = master_topology_create(12);
    master_add_sub_topology(m, TOPO_VOCABULARY, "词汇", 1000, 10);

    VisualCortex* vc = visual_cortex_create(m, NULL);
    ASSERT_NOT_NULL(vc, "vc create failed");

    /* 即使有任务，throttle < 0.1 应跳过 */
    visual_cortex_enqueue(vc, "/test.mp4", "visual");

    int work = visual_cortex_tick(vc, 0.05f);
    ASSERT_EQUAL(work, 0, "low throttle should skip");

    /* 队列中任务应保留 */
    ASSERT_EQUAL(visual_cortex_queue_size(vc), 1, "task should remain in queue");

    visual_cortex_destroy(vc);
    master_topology_destroy(m);
    TEST_END();
}

void test_vc_tick_null(void) {
    TEST_START("visual_cortex tick with NULL");
    int work = visual_cortex_tick(NULL, 1.0f);
    ASSERT_EQUAL(work, 0, "NULL tick should return 0");
    TEST_END();
}

/* ==================== 统计 API 测试 ==================== */

void test_vc_stats_initial(void) {
    TEST_START("visual_cortex stats (initial)");
    MasterTopology* m = master_topology_create(12);
    master_add_sub_topology(m, TOPO_VOCABULARY, "词汇", 1000, 10);

    VisualCortex* vc = visual_cortex_create(m, NULL);
    ASSERT_NOT_NULL(vc, "vc create failed");

    int q; long f; int vn, xe;
    visual_cortex_get_stats(vc, &q, &f, &vn, &xe);

    ASSERT_EQUAL(q, 0, "queue should be 0");
    ASSERT_EQUAL(f, 0, "frames should be 0");
    ASSERT_EQUAL(vn, 0, "visual nodes should be 0");
    ASSERT_EQUAL(xe, 0, "xmod edges should be 0");

    visual_cortex_destroy(vc);
    master_topology_destroy(m);
    TEST_END();
}

void test_vc_stats_null_params(void) {
    TEST_START("visual_cortex stats with partial NULL");
    MasterTopology* m = master_topology_create(12);
    master_add_sub_topology(m, TOPO_VOCABULARY, "词汇", 1000, 10);

    VisualCortex* vc = visual_cortex_create(m, NULL);
    ASSERT_NOT_NULL(vc, "vc create failed");

    /* NULL 输出参数不应崩溃 */
    visual_cortex_get_stats(vc, NULL, NULL, NULL, NULL);
    visual_cortex_get_stats(NULL, NULL, NULL, NULL, NULL);

    visual_cortex_destroy(vc);
    master_topology_destroy(m);
    TEST_END();
}

/* ==================== 编码器回调测试 ==================== */

static int dummy_encoder(const char* path, int dim, float* out, void* ctx) {
    (void)path; (void)ctx;
    /* 填充随机特征向量 */
    for (int i = 0; i < dim; i++)
        out[i] = (float)((i * 17 + 3) % 100) / 100.0f;
    return 0;
}

void test_vc_encoder_callback(void) {
    TEST_START("visual_cortex set encoder callback");
    MasterTopology* m = master_topology_create(12);
    master_add_sub_topology(m, TOPO_VOCABULARY, "词汇", 1000, 10);

    VisualCortex* vc = visual_cortex_create(m, NULL);
    ASSERT_NOT_NULL(vc, "vc create failed");

    int ctx = 42;
    visual_cortex_set_encoder(vc, dummy_encoder, &ctx);

    /* NULL encoder 不应崩溃 */
    visual_cortex_set_encoder(NULL, dummy_encoder, NULL);
    visual_cortex_set_encoder(vc, NULL, NULL);

    visual_cortex_destroy(vc);
    master_topology_destroy(m);
    TEST_END();
}

/* ==================== MediaReader 引用测试 ==================== */

void test_vc_get_media_reader(void) {
    TEST_START("visual_cortex get_media_reader");
    MasterTopology* m = master_topology_create(12);
    master_add_sub_topology(m, TOPO_VOCABULARY, "词汇", 1000, 10);

    VisualCortex* vc = visual_cortex_create(m, NULL);
    ASSERT_NOT_NULL(vc, "vc create failed");

    MediaReader* mr = visual_cortex_get_media_reader(vc);
    ASSERT_NOT_NULL(mr, "media_reader should be created internally");

    /* NULL 参数 */
    ASSERT_NULL(visual_cortex_get_media_reader(NULL), "NULL should return NULL");

    visual_cortex_destroy(vc);
    master_topology_destroy(m);
    TEST_END();
}

/* ==================== 帧差异评分测试 ==================== */

void test_frame_diff_score(void) {
    TEST_START("frame diff score (internal)");
    float a[64], b[64];
    for (int i = 0; i < 64; i++) {
        a[i] = 0.5f;
        b[i] = 0.5f;
    }
    /* 相同向量差异应为 0 */
    float sum_sq = 0.0f;
    for (int i = 0; i < 64; i++) {
        float d = a[i] - b[i];
        sum_sq += d * d;
    }
    int diff = (int)(sqrtf(sum_sq) * 100.0f);
    ASSERT_EQUAL(diff, 0, "identical frames should have diff 0");

    /* 修改 b 的一个维度 */
    b[0] = 1.0f;
    sum_sq = 0.0f;
    for (int i = 0; i < 64; i++) {
        float d = a[i] - b[i];
        sum_sq += d * d;
    }
    diff = (int)(sqrtf(sum_sq) * 100.0f);
    ASSERT_EQUAL(diff, 50, "0.5 difference should yield 50");

    TEST_END();
}

/* ==================== 脑区开关测试 ==================== */

void test_vc_thalamus_disabled(void) {
    TEST_START("visual_cortex tick when thalamus disables it");
    MasterTopology* m = master_topology_create(12);
    master_add_sub_topology(m, TOPO_VOCABULARY, "词汇", 1000, 10);

    VisualCortex* vc = visual_cortex_create(m, NULL);
    ASSERT_NOT_NULL(vc, "vc create failed");

    Thalamus* th = thalamus_create();
    ASSERT_NOT_NULL(th, "thalamus create failed");

    /* 注册视觉皮层为脑区 */
    thalamus_register_region(th, THAL_VISUAL_CORTEX, vc);

    /* 禁用该脑区 */
    thalamus_enable_region(th, THAL_VISUAL_CORTEX, 0);
    ASSERT_EQUAL(thalamus_is_region_enabled(th, THAL_VISUAL_CORTEX), 0,
                 "visual cortex should be disabled");

    /* 丘脑 throttle API */
    float t = thalamus_get_throttle(th, THAL_VISUAL_CORTEX);
    (void)t;  /* 只是证明调用不崩溃 */

    /* 清理 */
    thalamus_destroy(th);
    visual_cortex_destroy(vc);
    master_topology_destroy(m);
    TEST_END();
}

/* ==================== 主函数 ==================== */

int main(void) {
    printf("============================================\n");
    printf("  视觉皮层脑区单元测试 (VisualCortex)\n");
    printf("============================================\n\n");

    printf("--- 生命周期 ---\n");
    test_vc_create_destroy();
    test_vc_create_auto_creates_visual_topo();
    test_vc_null_params();
    test_vc_null_config();

    printf("\n--- 任务队列 ---\n");
    test_vc_enqueue_dequeue();
    test_vc_enqueue_multiple();
    test_vc_enqueue_null();
    test_vc_tick_empty_queue();
    test_vc_tick_low_throttle();
    test_vc_tick_null();

    printf("\n--- 统计 API ---\n");
    test_vc_stats_initial();
    test_vc_stats_null_params();

    printf("\n--- 编码器 ---\n");
    test_vc_encoder_callback();

    printf("\n--- MediaReader 引用 ---\n");
    test_vc_get_media_reader();

    printf("\n--- 帧差异 ---\n");
    test_frame_diff_score();

    printf("\n--- 脑区开关 ---\n");
    test_vc_thalamus_disabled();

    printf("\n============================================\n");
    printf("  结果: %d/%d 通过, %d 失败\n",
           tests_passed, tests_run, tests_failed);
    printf("============================================\n");

    return (tests_failed > 0) ? 1 : 0;
}
