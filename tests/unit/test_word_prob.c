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
#include "word_ctx.h"

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

    /* 中文多字词：token 是单字，词必须由字对合成出计数 —— 不合成则恒 0（曾踩过） */
    ASSERT_TRUE(wp_count("中一") >= 1, "中文多字词应由字对合成出计数");

    /* ⚠ CX-40 契约：词级熵【不许】用首字/末字冒充。
     * 没有 word_ctx 实测时就是「未知」= 0.0；旧实现这里会返回「中」的左熵（>0）。
     * 这一条是回归护栏：谁再把首字熵接回来，这里立刻红。 */
    ASSERT_NEAR(wp_entropy("中一", 0), 0.0, 1e-9, "无实测时多字词左熵应为 0（不再是首字的左熵）");
    ASSERT_NEAR(wp_entropy("中一", 1), 0.0, 1e-9, "无实测时多字词右熵应为 0（不再是末字的右熵）");
    ASSERT_TRUE(wp_is_wordlike("中一", 0.5, 1) == 0, "没有词级实测的多字词不应被判为合法词");

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

    /* ---------- ②-bis 词整体左右邻（CX-40） ---------- */
    /* 造一份最小语料，把 CX-40 的两个铁证摆进来：
     *   · 「公吵」只出现在「老公吵架」里 ⇒ 左邻恒为「老」、右邻恒为「架」、两侧边界 0%
     *     ⇒ 切片词，在语料里从不独立出现。
     *   · 「公」自己用得很广（老/办/石/水/火 五种左邻）⇒ 字级左熵高。
     *     旧实现拿「公」的左熵冒充「公吵」的左熵 ⇒ 切片词拿假高分。
     *   · 「苹果」左邻 5 种、右邻 7 种，还能出现在句首 ⇒ 真词。
     * 这一组的意义：判据本身是准的，坏的是实现 —— 所以钉在这里。 */
    {
        static const char* corpus[] = {
            "老公吵架", "老公吵架", "老公吵架",
            "办公，石公，水公，火公",
            "吃苹果很甜", "买苹果贵", "看苹果大",
            "苹果好吃", "苹果便宜", "红苹果甜", "大苹果香",
        };
        const int nline = (int)(sizeof(corpus) / sizeof(corpus[0]));
        for (int i = 0; i < nline; i++) cp_observe_tokens(corpus[i]);
        wp_invalidate();

        ASSERT_TRUE(wp_entropy("公", 0) > 1.5, "「公」的字级左熵应高（5 种左邻）");

        static const char* cands[] = { "公吵", "苹果" };
        wctx_reset();     /* 从干净的结果表起步（本进程内可能已有别的构建） */
        ASSERT_TRUE(wctx_begin(cands, 2) == 2, "word_ctx 应接受 2 个候选");
        for (int i = 0; i < nline; i++) wctx_scan_text(corpus[i]);
        wctx_finish();

        WctxStat frag;
        ASSERT_TRUE(wctx_query("公吵", &frag) == 1, "「公吵」应被实测到");
        ASSERT_TRUE(frag.count >= 3, "「公吵」应出现 3 次");
        ASSERT_NEAR(frag.hl, 0.0, 1e-9, "「公吵」词整体左熵应为 0（左邻只有「老」）");
        ASSERT_NEAR(frag.hr, 0.0, 1e-9, "「公吵」词整体右熵应为 0（右邻只有「架」）");
        ASSERT_TRUE(frag.lbound == 0 && frag.rbound == 0, "「公吵」两侧都不该出现边界");

        WctxStat real;
        ASSERT_TRUE(wctx_query("苹果", &real) == 1, "「苹果」应被实测到");
        /* 边界【计入】分布（与 tools/word_ctx.py 探针一致）：
         * 左邻 = {吃,买,看,红,大}×1 + <边界>×2 ⇒ H = 2.5216 bit
         * 右邻 = {很,贵,大,好,便,甜,香}×1  ⇒ H = log2(7) = 2.8074 bit */
        ASSERT_NEAR(real.hl, 2.5216, 0.01, "「苹果」词整体左熵应为 2.5216 bit（含边界类）");
        ASSERT_NEAR(real.hr, log2(7.0), 0.01, "「苹果」词整体右熵应为 log2(7)");
        ASSERT_TRUE(real.lbound == 2, "「苹果」应在段首出现 2 次");
        ASSERT_TRUE(real.rbound == 0, "「苹果」不应在段尾出现");

        /* 晋升闸门谓词（qa_retrain 用的就是这条）：只看词整体 min熵 ≥ me。
         * 曾经还有「两侧都有边界即放行」的豁免，2026-10-27 实测后撤掉：
         * 它放过了高频切片（用太 0.432 / 的原 0.687），而它当初想救的
         * 「吵架」在全量语料上 min熵 3.197，本来就不需要救。 */
        int frag_ok = (frag.hl < frag.hr ? frag.hl : frag.hr) >= 2.0;
        int real_ok = (real.hl < real.hr ? real.hl : real.hr) >= 2.0;
        ASSERT_TRUE(frag_ok == 0, "切片词「公吵」应被晋升闸门拒绝");
        ASSERT_TRUE(real_ok == 1, "真词「苹果」应通过晋升闸门");

        /* 读侧也必须走实测：给词级 0.0，而不是「公」的字级 2.3 */
        ASSERT_NEAR(wp_entropy("公吵", 0), 0.0, 1e-9, "读侧应给出词整体左熵 0，不是首字的字级熵");
        ASSERT_NEAR(wp_min_entropy("公吵"), 0.0, 1e-9, "读侧 min 熵应为 0");
        ASSERT_TRUE(wp_is_wordlike("公吵", 2.0, 3) == 0, "「公吵」不应被判为合法词");
        ASSERT_TRUE(wp_is_wordlike("苹果", 2.0, 3) == 1, "「苹果」应被判为合法词");
    }

    /* ---------- ④ 写侧结构闸门（CX-27） ---------- */
    {
        int why = -1;
        ASSERT_TRUE(wp_word_gate("天气", &why) == 1, "正常词应过闸");
        ASSERT_TRUE(why == 0, "正常词 reason 应为 0");
        ASSERT_TRUE(wp_word_gate("中", &why) == 1, "单字应过闸");

        ASSERT_TRUE(wp_word_gate("= =", &why) == 0, "纯符号应被拒");
        ASSERT_TRUE(why == 2, "纯符号 reason 应为 2");
        ASSERT_TRUE(wp_word_gate("。", &why) == 0, "纯标点应被拒");

        ASSERT_TRUE(wp_word_gate("一二三四五六七", &why) == 0, "7 个汉字应被拒（过合并）");
        ASSERT_TRUE(why == 3, "超长词 reason 应为 3");

        /* 非法 UTF-8：孤立的续字节 */
        const char bad[] = {(char)0xE5, (char)0xB1, (char)0x80, (char)0x80, 0};
        ASSERT_TRUE(wp_word_gate(bad, &why) == 0, "非法 UTF-8 应被拒");
        ASSERT_TRUE(why == 1, "非法 UTF-8 reason 应为 1");

        /* 句子级判据：不套 6 字上限 */
        ASSERT_TRUE(wp_text_has_content("今天天气不错，我们去爬山吧") == 1, "句子应判有内容");
        ASSERT_TRUE(wp_text_has_content("= =") == 0, "纯符号应判无内容");
        ASSERT_TRUE(wp_text_has_content("") == 0, "空串应判无内容");

        /* 计数应累计（拒收 4 次：纯符号 / 纯标点 / 超长 / 非法 UTF-8） */
        unsigned long long gate[5] = {0};
        wp_gate_stats(gate);
        ASSERT_TRUE(gate[1] >= 1 && gate[2] >= 2 && gate[3] >= 1, "闸门分项计数应累计");
    }

    /* ---------- ⑤ 读数一致性 ---------- */
    int tok = 0, pairs = 0;
    unsigned long long total = 0;
    wp_stats(&tok, &pairs, &total);
    ASSERT_TRUE(tok > 0, "wp_tokens 应 > 0");
    ASSERT_TRUE(pairs > 0, "wp_pairs 应 > 0");
    ASSERT_TRUE(total > 0, "wp_total 应 > 0");

    printf("\n[word_prob] %d 通过 / %d 失败 (沙箱 %s)\n", g_pass, g_fail, dir);
    return g_fail ? 1 : 0;
}
