/**
 * @file test_concept_output.c
 * @brief 用户可见输出判据契约单测 —— 逐条锁死 concept_is_outputtable() 的口径。
 *
 * 为什么这个单测必须存在（别把它改成「宽松一点」）：
 *  引擎的「语义拓扑匿名节点」命名形如 sem_<x>_<n>（产出方 src/semantic_growth.c），
 *  是内部标识符。历史上「这个节点/概念能不能输出」的实现有 3 份、口径不一，
 *  其中 concept_is_printable() 明确放行 '_' ⇒ sem_xxx 会直达用户回复（真 bug）。
 *  本单测是【回归锁】：
 *   ① sem_ 前缀必须被 outputtable 拒绝（用例 1）；
 *   ② 正常词必须放行，且 'semantic'（sem 但非 sem_）不得被误杀（用例 2）；
 *   ③ outputtable 必须继承 printable 的全部拒绝口径（用例 3）。
 */

#include "cognitive_controller.h"

#include <stdio.h>

static int tests_run = 0, tests_passed = 0, tests_failed = 0;

#define T_START(name) do { printf("Running: %s...", name); fflush(stdout); tests_run++; } while (0)
#define T_END()  do { tests_passed++; printf(" PASSED\n"); } while (0)
#define T_FAIL(...) do { tests_failed++; printf(" FAILED: "); printf(__VA_ARGS__); printf("\n"); } while (0)
#define CHECK(cond, ...) do { if (!(cond)) { T_FAIL(__VA_ARGS__); return; } } while (0)

/* ══════════════ 1. 回归锁：内部匿名节点必须被拒 ══════════════ */

static void test_rejects_internal_nodes(void) {
    T_START("outputtable: 拒 sem_ 前缀内部节点（回归锁）");
    CHECK(concept_is_outputtable("sem_0_1") == 0, "「sem_0_1」应被拒（内部匿名节点）");
    CHECK(concept_is_outputtable("sem_word_17") == 0, "「sem_word_17」应被拒");
    CHECK(concept_is_outputtable("sem_") == 0, "「sem_」应被拒");
    /* 生产格式见 src/semantic_growth.c:179 snprintf("sem_%s_%d", seed, mcnt) */
    CHECK(concept_is_outputtable("sem_\xE8\x8B\xB9\xE6\x9E\x9C_3") == 0,
          "「sem_苹果_3」（真实命名格式）应被拒");
    CHECK(concept_is_outputtable("semantic_growth_3") == 1,
          "「semantic_growth_3」不是 sem_ 前缀，不得误杀（判据是 sem_ 而非 sem）");
    T_END();

    T_START("回归锁: 旧判据 concept_is_printable 确实放行 sem_（泄漏根因）");
    CHECK(concept_is_printable("sem_0_1") == 1,
          "concept_is_printable 放行 sem_ ⇒ 这正是泄漏根因；若此处变 0，说明口径被改动");
    T_END();
}

/* ══════════════ 2. 正常词必须放行（防误杀） ══════════════ */

static void test_accepts_normal(void) {
    T_START("outputtable: 放行正常中文/英文词");
    CHECK(concept_is_outputtable("\xE4\xB8\xAD\xE6\x96\x87") == 1, "「中文」应放行");
    CHECK(concept_is_outputtable("apple") == 1, "「apple」应放行");
    CHECK(concept_is_outputtable("hello-world") == 1, "「hello-world」应放行（'-' 合法）");
    CHECK(concept_is_outputtable("a_b") == 1, "「a_b」应放行（'_' 合法且非行首 sem_）");
    T_END();

    T_START("防误杀: 'semantic'（sem 但非 sem_）必须放行");
    CHECK(concept_is_outputtable("semantic") == 1,
          "「semantic」不是内部节点名，必须放行 —— 判据是 sem_ 前缀而非 sem");
    CHECK(concept_is_outputtable("semantic word") == 0, "含空格 ⇒ 仍被 printable 拒");
    T_END();
}

/* ══════════════ 3. 与 printable 的一致性：噪声仍被拒 ══════════════ */

static void test_inherits_printable(void) {
    T_START("outputtable: 继承 printable 的全部拒绝口径");
    CHECK(concept_is_outputtable("") == 0, "空串 ⇒ 0");
    CHECK(concept_is_outputtable(NULL) == 0, "NULL ⇒ 0");
    CHECK(concept_is_outputtable("hello world") == 0, "含空格 ⇒ 0");
    CHECK(concept_is_outputtable("a@b") == 0, "含 '@' ⇒ 0");
    CHECK(concept_is_outputtable("a|b") == 0, "含 '|' ⇒ 0");
    T_END();

    T_START("一致性: 凡 outputtable 通过者必通过 printable");
    {
        static const char* cases[] = {
            "\xE4\xB8\xAD\xE6\x96\x87", "apple", "hello-world", "a_b", "semantic"
        };
        int i;
        for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
            CHECK(concept_is_outputtable(cases[i]) == 1, "用例 %d 应放行", i);
            CHECK(concept_is_printable(cases[i]) == 1, "用例 %d 应与 printable 一致", i);
        }
    }
    T_END();
}

/* ══════════════════════════════ main ══════════════════════════════ */

int main(void) {
    printf("\n=== 用户可见输出判据契约单测 (concept_is_outputtable) ===\n\n");

    test_rejects_internal_nodes();
    test_accepts_normal();
    test_inherits_printable();

    printf("\n");
    printf("  用例: %d 运行, %d 通过, %d 失败\n", tests_run, tests_passed, tests_failed);
    printf("\n=== 完成 ===\n");
    return (tests_failed == 0) ? 0 : 1;
}
