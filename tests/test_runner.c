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

    /* 单元测试 */
    const char* tests[] = {
        "build/bin/test_tensor",
        "build/bin/test_model",
        "build/bin/test_metrics",
        "build/bin/test_trainer",
        "build/bin/test_chinese",
        "build/bin/test_io",
        "build/bin/test_cognitive_controller",
        "build/bin/test_cognitive_full",
        "build/bin/test_web_fetch",
        "build/bin/test_integration",
    };
    int count = sizeof(tests) / sizeof(tests[0]);

    for (int i = 0; i < count; i++) {
        total++;
        if (run_test(tests[i], tests[i]) == 0)
            passed++;
    }

    /* ASCII 框线宽度固定 38 字符，汇总行靠右填充 */
    printf("\n╔══════════════════════════════════════╗\n");
    printf("║  汇总: %d/%d 通过                     ║\n", passed, total);
    printf("╚══════════════════════════════════════╝\n");

    return (passed == total) ? 0 : 1;
}
