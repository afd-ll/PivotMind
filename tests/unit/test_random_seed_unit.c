/**
 * @file test_random_seed_unit.c
 * @brief 固定随机种子契约单测（v0.5.37：接线 init_random_seed / PIVOTMIND_SEED）。
 *
 * 为什么必须存在：
 *  `PIVOTMIND_SEED` 在 v0.5.26 的 CHANGELOG 里就作为成品交付了，但
 *  `init_random_from_env()` / `init_random_seed()` 全仓**零调用点** —— 是「装了
 *  开关没通电」的空开关；而且 `init_random()` 首次运行会用 time/pid 覆盖已设种子。
 *  本单测锁死三件事，任何一件回归都要变红：
 *   ① init_random() 不得覆盖已显式设置的种子；
 *   ② 同一 seed 两次初始化 ⇒ rand() 序列逐值一致；
 *   ③ PIVOTMIND_SEED 已设置且非空时生效、未设/空串时不改变种子状态。
 *
 * ⚠️ 用例顺序不可随意调换：`init_random()` 内部是一次性 static（首次调用才可能
 *    重新 srand），所以「守护测试」T1 必须是**进程内第一次**调用 init_random()，
 *    否则 static 已置位、拿不到能变红的证据。
 */

#include "common.h"

#include <stdio.h>
#include <stdlib.h>

static int tests_run = 0, tests_passed = 0, tests_failed = 0;

#define T_START(name) do { printf("Running: %s...", name); fflush(stdout); tests_run++; } while (0)
#define T_END()  do { tests_passed++; printf(" PASSED\n"); } while (0)
#define T_FAIL(...) do { tests_failed++; printf(" FAILED: "); printf(__VA_ARGS__); printf("\n"); } while (0)
#define CHECK(cond, ...) do { if (!(cond)) { T_FAIL(__VA_ARGS__); return; } } while (0)

/* ══════════════ T0. 默认标志 ══════════════ */

static void test_default_flag_is_false(void) {
    T_START("默认 pm_random_seed_explicit == false（未设种 ⇒ 保持旧的 time/pid 播种）");
    CHECK(pm_random_seed_explicit == false,
          "进程启动时标志应为 false，实为 true ⇒ 未设种路径的行为不再与旧实现一致");
    T_END();
}

/* ══════════════ T1. 守护（必须是本进程首次 init_random 调用）══════════════ */

static void test_init_random_does_not_override(void) {
    unsigned int a1, b1, c1, a2, b2, c2;

    T_START("★守护：init_random() 不得用 time/pid 覆盖已显式设置的种子");
    init_random_seed(0x5EED1234u);
    a1 = (unsigned)rand();
    /* 本进程第一次 init_random()：若 guard 失效，这里会 srand(time^pid) */
    init_random();
    b1 = (unsigned)rand();
    c1 = (unsigned)rand();

    init_random_seed(0x5EED1234u);   /* 重放同一种子 */
    a2 = (unsigned)rand();
    b2 = (unsigned)rand();
    c2 = (unsigned)rand();

    CHECK(a1 == a2 && b1 == b2 && c1 == c2,
          "init_random() 覆盖了显式种子：seed 后 3 值 (%u,%u,%u) vs 重放 (%u,%u,%u)",
          a1, b1, c1, a2, b2, c2);
    T_END();
}

/* ══════════════ T2. 同种子同序列 ══════════════ */

static void test_same_seed_same_sequence(void) {
    unsigned int s1[3], s2[3];
    int i, same = 1;

    T_START("同种子同序列 + 不同种子不同序列");
    init_random_seed(12345u);
    for (i = 0; i < 3; i++) s1[i] = (unsigned)rand();

    init_random_seed(12345u);
    for (i = 0; i < 3; i++) s2[i] = (unsigned)rand();
    for (i = 0; i < 3; i++) if (s1[i] != s2[i]) same = 0;
    CHECK(same, "同一 seed=12345 两次初始化序列不一致：(%u,%u,%u) vs (%u,%u,%u)",
          s1[0], s1[1], s1[2], s2[0], s2[1], s2[2]);

    init_random_seed(54321u);
    same = 1;
    for (i = 0; i < 3; i++) if ((unsigned)rand() != s1[i]) { same = 0; break; }
    CHECK(!same, "不同 seed（54321 vs 12345）产生了逐值相同的序列 ⇒ 种子未生效");
    T_END();
}

/* ══════════════ T3. PIVOTMIND_SEED 生效 ══════════════ */

static void test_env_seed_is_effective(void) {
    unsigned int a, b, a2, b2, c, d, c2, d2;

    T_START("PIVOTMIND_SEED 已设置 ⇒ 生效且不被 init_random() 覆盖");
    setenv("PIVOTMIND_SEED", "20260913", 1);
    init_random_from_env();
    a = (unsigned)rand();
    init_random();                    /* 已设种 ⇒ 不得覆盖 */
    b = (unsigned)rand();

    init_random_seed(20260913u);
    a2 = (unsigned)rand();
    b2 = (unsigned)rand();
    CHECK(a == a2 && b == b2,
          "PIVOTMIND_SEED=20260913 未生效或 init_random() 覆盖了它：(%u,%u) vs 直接 srand 的 (%u,%u)",
          a, b, a2, b2);

    /* 再验一次：置种后调用 init_random() 仍不得改变流 */
    init_random_from_env();
    c = (unsigned)rand();
    init_random();
    d = (unsigned)rand();
    init_random_seed(20260913u);
    c2 = (unsigned)rand();
    d2 = (unsigned)rand();
    CHECK(c == c2 && d == d2, "env 置种后 init_random() 覆盖了种子：(%u,%u) vs (%u,%u)",
          c, d, c2, d2);
    T_END();
}

/* ══════════════ T4. PIVOTMIND_SEED 未设 / 空串 ══════════════ */

static void test_env_seed_absent_is_noop(void) {
    unsigned int s1, s2, t1, t2, e1, e2, f1, f2;

    T_START("PIVOTMIND_SEED 未设置 ⇒ init_random_from_env() 不改变种子");
    init_random_seed(777u);
    s1 = (unsigned)rand();
    unsetenv("PIVOTMIND_SEED");
    init_random_from_env();           /* 未设 ⇒ 不得 srand */
    s2 = (unsigned)rand();

    init_random_seed(777u);
    t1 = (unsigned)rand();
    t2 = (unsigned)rand();
    CHECK(s1 == t1 && s2 == t2,
          "未设 PIVOTMIND_SEED 时种子被改变：(%u,%u) vs (%u,%u)", s1, s2, t1, t2);
    T_END();

    T_START("PIVOTMIND_SEED 为空串 ⇒ init_random_from_env() 不改变种子");
    init_random_seed(888u);
    e1 = (unsigned)rand();
    setenv("PIVOTMIND_SEED", "", 1);
    init_random_from_env();           /* 空串 ⇒ 不得 srand */
    e2 = (unsigned)rand();

    init_random_seed(888u);
    f1 = (unsigned)rand();
    f2 = (unsigned)rand();
    CHECK(e1 == f1 && e2 == f2,
          "空串 PIVOTMIND_SEED 时种子被改变：(%u,%u) vs (%u,%u)", e1, e2, f1, f2);
    unsetenv("PIVOTMIND_SEED");
    T_END();
}

/* ══════════════════════════════ main ══════════════════════════════ */

int main(void) {
    printf("\n=== 固定随机种子契约单测 (include/common.h) ===\n\n");

    test_default_flag_is_false();          /* 必须最先（不改状态） */
    test_init_random_does_not_override();  /* 必须第二：本进程首次 init_random() */
    test_same_seed_same_sequence();
    test_env_seed_is_effective();
    test_env_seed_absent_is_noop();

    printf("\n");
    printf("  用例: %d 运行, %d 通过, %d 失败\n", tests_run, tests_passed, tests_failed);
    printf("\n=== 完成 ===\n");
    return (tests_failed == 0) ? 0 : 1;
}
