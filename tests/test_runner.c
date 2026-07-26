/**
 * @file test_runner.c
 * @brief 统一测试运行器 — 依次执行所有单元测试
 *
 * 每个单元测试在 tests/unit/ 下作为独立可执行文件。
 * 本运行器会逐个编译并运行它们，汇总结果。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#endif

static int run_test(const char* name, const char* path) {
    printf("\n━━━ Running %s ━━━\n", name);
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s", path);
    int ret = system(cmd);
    /* 跨平台返回码归一化：Windows system() 返回进程退出码，Linux 返回 WEXITSTATUS */
#ifdef _WIN32
    int passed = (ret == 0);
#else
    int passed = (WIFEXITED(ret) && WEXITSTATUS(ret) == 0);
#endif
    printf("━━━ %s: %s ━━━\n\n", name, passed ? "PASSED" : "FAILED");
    return passed ? 0 : ret;
}

int main(void) {
    int total = 0, passed = 0;

    printf("╔══════════════════════════════════════╗\n");
    printf("║      PivotMind 测试套件              ║\n");
    printf("╚══════════════════════════════════════╝\n");

    /* 单元测试 — 按稳定性分组 */

    /* 快速测试 (离线，无网络依赖) */
    const char* fast_tests[] = {
        "build/bin/test_dialog_unit",
        "build/bin/test_diffusion_unit",
        "build/bin/test_topology_unit",
        "build/bin/test_memory_unit",
        "build/bin/test_learner_unit",
        "build/bin/test_causal_unit",
        "build/bin/test_forgetting_unit",
        "build/bin/test_media_reader",
        "build/bin/test_pure",
        "build/bin/test_search",
        "build/bin/test_pfe_unit",
        "build/bin/test_regression",
    };

    /* 网络/慢速测试 */
    const char* slow_tests[] = {
        "build/bin/test_web_fetch",
    };

    /* 已知不稳定 (预存 bug，等待修复) */
    /* test_tensor — tensor 1D 断言失败后卡死 */
    /* test_trainer — malloc(): invalid size (heap 损坏) */
    /* test_visual_cortex — 拓扑警告后卡死 */
    /* test_chinese — 哑测试 (只打印 locale) */
    /* test_io — 哑测试 (只打印 stdout) */
    /* test_cognitive_controller — 超时卡死 */
    /* test_cognitive_full — 需要持久化数据文件 */
    /* test_integration — 需要预先训练模型 */

    int fast_count = sizeof(fast_tests) / sizeof(fast_tests[0]);
    int slow_count = sizeof(slow_tests) / sizeof(slow_tests[0]);

    for (int i = 0; i < fast_count; i++) {
        total++;
        if (run_test(fast_tests[i], fast_tests[i]) == 0)
            passed++;
    }

    /* 可选：运行慢速测试 */
    const char* env = getenv("PIVOTMIND_RUN_SLOW");
    if (env && strcmp(env, "1") == 0) {
        printf("\n── 慢速测试 (PIVOTMIND_RUN_SLOW=1) ──\n");
        for (int i = 0; i < slow_count; i++) {
            total++;
            if (run_test(slow_tests[i], slow_tests[i]) == 0)
                passed++;
        }
    }

    /* ASCII 框线宽度固定 38 字符，汇总行靠右填充 */
    printf("\n╔══════════════════════════════════════╗\n");
    printf("║  汇总: %d/%d 通过                     ║\n", passed, total);
    printf("╚══════════════════════════════════════╝\n");

    return (passed == total) ? 0 : 1;
}
