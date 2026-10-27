/**
 * test_word_prob.c — 词概率/左右熵/PMI 契约单测（第 33 支）
 *
 * 为什么需要：v0.6.12 起「生成」和「词涌现」共用同一套统计口径
 * （word_prob 从 charpair 现算）。这套统计一旦被改坏，
 * 生成侧只是"悄悄变差"，词涌现只是"悄悄少造词"——都不会报错。
 * 因此把三条判据钉死：
 *   ① 计数/概率口径：count 与 P(w) 随观测增加，英文 token 与中文同表
 *   ② 左右熵：搭配多样的单位熵高，只在固定上下文出现的碎片熵为 0
 *   ③ 生成增益：单字恒 1.0（不奖不罚），多字落在 [0.45,1.25]
 *
 * 沙箱：本测试通过 PIVOTMIND_HOME 指向临时目录，**绝不碰线上 charpairs.bin**。
 */

#include "word_prob.h"
#include "charpair.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail = 0;
static int g_pass = 0;

#define ASSERT_TRUE(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

#define ASSERT_NEAR(a, b, eps, msg) do { \
    double _d = (double)(a) - (double)(b); \
    if (_d < 0) _d = -_d; \
    if (_d <= (eps)) { g_pass++; } \
    else { g_fail++; printf("FAIL: %s 期望 %.4f 实际 %.4f (%s:%d)\n", msg, (double)(b), (double)(a), __FILE__, __LINE__); } \
} while (0)

int main(void) {
    /* 沙箱：必须在任何 pm_* 调用之前设置（pm_home 只解析一次并缓存） */
    char tmpl[] = "/tmp/pm_wordprob_XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) { printf("FAIL: mkdtemp\n"); return 1; }
    setenv("PIVOTMIND_HOME", dir, 1);
    unsetenv("PIVOTMIND_PROB_ASSIST");

    /* ---------- ① 计数 / 概率口径 ---------- */
    /* "甲中一 乙中二 丙中三 丁中四"：空格断链 ⇒ 中 的左右邻居各 4 种各 1 次
     * 另外再喂英文，验证中英同表。 */
    cp_observe_tokens("甲中一 乙中二 丙中三 丁中四");
    wp_invalidate();

    unsigned long long c0 = wp_count("中");
    ASSERT_TRUE(c0 > 0, "中 应被计入");
    ASSERT_TRUE(wp_p("中") > 0.0, "P(中) 应大于 0");
    ASSERT_TRUE(wp_count("不存在词") == 0, "未观测 token 计数应为 0");
    ASSERT_TRUE(wp_p("不存在词") == 0.0, "未观测 token 概率应为 0");

    /* ---------- ② 左右熵：多样 vs 碎片 ---------- */
    double mn_real = wp_min_entropy("中");     /* 左邻 4 种×1、右邻 4 种×1 ⇒ 各 2 bit */
    ASSERT_NEAR(mn_real, 2.0, 0.01, "中 的 min 左右熵应为 2.0 bit");

    /* 中文多字词：token 是单字，词必须由字对合成 —— 不合成则恒 0（曾踩过） */
    ASSERT_TRUE(wp_count("中一") >= 1, "中文多字词应由字对合成出计数");
    ASSERT_TRUE(wp_entropy("中一", 0) > 0.0, "多字词左熵应取首字的左熵");
    ASSERT_NEAR(wp_gain("中一"), 0.70, 0.01, "只见过 1 次的多字词增益应为 0.70");

    /* 英文碎片：同一个 token 反复自组合 ⇒ 左右邻居唯一 ⇒ 熵 0 */
    cp_observe_tokens("abab abab abab");
    wp_invalidate();
    double mn_frag = wp_min_entropy("abab");
    ASSERT_TRUE(mn_frag < 0.01, "abab 的 min 左右熵应为 0（固定搭配碎片）");
    ASSERT_TRUE(wp_is_wordlike("abab", 2.0, 1) == 0, "碎片 abab 不应被判为合法词");

    /* 英文正常搭配：machine learning 重复 + 左右邻多样 */
    cp_observe_tokens("a machine learning b machine learning c machine learning");
    wp_invalidate();
    ASSERT_TRUE(wp_count("machine") > 0, "英文 token machine 应被计入");
    ASSERT_TRUE(wp_count("learning") > 0, "英文 token learning 应被计入");
    ASSERT_TRUE(wp_pmi("machine", "learning") > 0.0, "machine learning 的 PMI 应为正");

    /* ---------- ③ 生成增益 ---------- */
    ASSERT_NEAR(wp_gain("中"), 1.0, 1e-6, "单字增益应恒为 1.0（不奖不罚）");
    ASSERT_TRUE(wp_gain("machine") >= 0.45f - 1e-6f && wp_gain("machine") <= 1.25f + 1e-6f,
                "多字增益应落在 [0.45,1.25]");
    ASSERT_TRUE(wp_gain("未观测的怪词") >= 0.45f - 1e-6f, "未知多字词增益下界 0.45");

    /* ---------- ④ 读数一致性 ---------- */
    int tok = 0, pairs = 0;
    unsigned long long total = 0;
    wp_stats(&tok, &pairs, &total);
    ASSERT_TRUE(tok > 0, "wp_tokens 应 > 0");
    ASSERT_TRUE(pairs > 0, "wp_pairs 应 > 0");
    ASSERT_TRUE(total > 0, "wp_total 应 > 0");

    printf("\n[word_prob] %d 通过 / %d 失败 (沙箱 %s)\n", g_pass, g_fail, dir);
    return g_fail ? 1 : 0;
}
