/**
 * @file test_lang_unit.c
 * @brief 语种判定 SSOT 契约单测 —— 逐条直接断言 include/lang.h 的定稿契约。
 *
 * 为什么这个单测必须存在（别把它改成「宽松一点」）：
 *  ① 语种判定此前散落 9 处、口径分三档（首字节 &0x80 / strlen==3 / 码点基本区），
 *     同一串在不同子系统会被判成不同语种。本 SSOT 的全部价值就是「只有一个答案」，
 *     因此**边界必须被测试锁死** —— 改口径就得同步改本单测（= 有意识的变更）。
 *  ② 用例 5/6 是【回归锁】：CJK 标点「。」与平假名「あ」都是 3 字节 UTF-8，
 *     旧 `strlen(s)==3` 判据把它们当汉字 ⇒ v0.5.35 修掉。这两条一旦变红，
 *     说明有人把「字节数 = 语种」的错误口径又带回来了。
 *  ③ 不用 strcmp 之外的隐式假设：pm_lang_name 的字符串是对外协议，逐字断言。
 */

#include "lang.h"

#include <stdio.h>
#include <string.h>

static int tests_run = 0, tests_passed = 0, tests_failed = 0;

#define T_START(name) do { printf("Running: %s...", name); fflush(stdout); tests_run++; } while (0)
#define T_END()  do { tests_passed++; printf(" PASSED\n"); } while (0)
#define T_FAIL(...) do { tests_failed++; printf(" FAILED: "); printf(__VA_ARGS__); printf("\n"); } while (0)
#define CHECK(cond, ...) do { if (!(cond)) { T_FAIL(__VA_ARGS__); return; } } while (0)

/* ══════════════ 1. pm_utf8_decode：字节数与码点 ══════════════ */

static void test_decode_basic(void) {
    unsigned cp = 0;
    int n;

    T_START("utf8_decode: 1/2/3/4 字节序列");
    n = pm_utf8_decode("A", &cp);
    CHECK(n == 1 && cp == 0x41u, "ASCII: n=%d cp=0x%X（期望 1 / 0x41）", n, cp);

    n = pm_utf8_decode("\xC3\xA9", &cp);              /* é U+00E9 */
    CHECK(n == 2 && cp == 0x00E9u, "2字节 é: n=%d cp=0x%X（期望 2 / 0xE9）", n, cp);

    n = pm_utf8_decode("\xE4\xB8\xAD", &cp);          /* 中 U+4E2D */
    CHECK(n == 3 && cp == 0x4E2Du, "3字节 中: n=%d cp=0x%X（期望 3 / 0x4E2D）", n, cp);

    n = pm_utf8_decode("\xF0\x9F\x98\x80", &cp);      /* 😀 U+1F600 */
    CHECK(n == 4 && cp == 0x1F600u, "4字节 emoji: n=%d cp=0x%X（期望 4 / 0x1F600）", n, cp);

    T_END();
}

static void test_decode_invalid(void) {
    unsigned cp = 0xFFFFu;
    int n;

    T_START("utf8_decode: 非法/截断/空串 ⇒ 不崩溃、cp 归零");
    n = pm_utf8_decode("", &cp);
    CHECK(n == 1 && cp == 0u, "空串: n=%d cp=0x%X（期望 1 / 0）", n, cp);

    cp = 0xFFFFu;
    n = pm_utf8_decode("\x80", &cp);                  /* 裸续字节 */
    CHECK(n == 1 && cp == 0u, "裸续字节: n=%d cp=0x%X（期望 1 / 0）", n, cp);

    cp = 0xFFFFu;
    n = pm_utf8_decode(NULL, &cp);
    CHECK(n == 1 && cp == 0u, "NULL: n=%d cp=0x%X（期望 1 / 0）", n, cp);

    cp = 0xFFFFu;
    n = pm_utf8_decode("\xE4\xB8", &cp);              /* 3 字节序列被截断 */
    CHECK(n >= 1 && n <= 2, "截断序列: n=%d（期望 1..2，不得越过字符串）", n);

    T_END();
}

/* ══════════════ 2. pm_lang_of_cp：区块边界 ══════════════ */

static void test_lang_of_cp(void) {
    T_START("lang_of_cp: 中文区边界（含扩展 A）");
    CHECK(pm_lang_of_cp(0x4E00u) == PM_LANG_ZH, "U+4E00 应为 ZH");
    CHECK(pm_lang_of_cp(0x9FFFu) == PM_LANG_ZH, "U+9FFF 应为 ZH");
    CHECK(pm_lang_of_cp(0x3400u) == PM_LANG_ZH, "U+3400（扩展 A 首）应为 ZH");
    CHECK(pm_lang_of_cp(0x4DBFu) == PM_LANG_ZH, "U+4DBF（扩展 A 末）应为 ZH");
    CHECK(pm_lang_of_cp(0x4E00u - 1u) != PM_LANG_ZH, "U+4DFF 不应是 ZH");
    CHECK(pm_lang_of_cp(0x9FFFu + 1u) != PM_LANG_ZH, "U+A000 不应是 ZH");
    T_END();

    T_START("lang_of_cp: 日/韩/英/其它");
    CHECK(pm_lang_of_cp(0x3042u) == PM_LANG_JA, "U+3042 あ 应为 JA");
    CHECK(pm_lang_of_cp(0x30A2u) == PM_LANG_JA, "U+30A2 ア 应为 JA");
    CHECK(pm_lang_of_cp(0xAC00u) == PM_LANG_KO, "U+AC00 가 应为 KO");
    CHECK(pm_lang_of_cp(0x41u)   == PM_LANG_EN, "U+0041 'A' 应为 EN");
    CHECK(pm_lang_of_cp(0x61u)   == PM_LANG_EN, "U+0061 'a' 应为 EN");
    CHECK(pm_lang_of_cp(0x00E9u) == PM_LANG_EN, "U+00E9 é 应为 EN（拉丁扩展）");
    CHECK(pm_lang_of_cp(0x0416u) == PM_LANG_OTHER, "U+0416 Ж 应为 OTHER");
    CHECK(pm_lang_of_cp(0x1F600u) == PM_LANG_OTHER, "U+1F600 😀 应为 OTHER");
    T_END();

    T_START("lang_of_cp: 数字/标点/空白 ⇒ UNKNOWN");
    CHECK(pm_lang_of_cp(0x30u) == PM_LANG_UNKNOWN, "'0' 应为 UNKNOWN");
    CHECK(pm_lang_of_cp(0x20u) == PM_LANG_UNKNOWN, "空格 应为 UNKNOWN");
    CHECK(pm_lang_of_cp(0x2Eu) == PM_LANG_UNKNOWN, "'.' 应为 UNKNOWN");
    CHECK(pm_lang_of_cp(0u)    == PM_LANG_UNKNOWN, "码点 0 应为 UNKNOWN");
    T_END();
}

/* ══════════════ 3. 回归锁：3 字节 ≠ 汉字 ══════════════ */
/* 旧口径 `strlen(s)==3` 会把下列 3 字节字符当汉字 —— 这是 v0.5.35 修掉的真错判。 */

static void test_regression_three_byte(void) {
    T_START("回归锁: CJK 标点「。」(U+3002) 不得判为中文");
    CHECK(pm_lang_of("\xE3\x80\x82") != PM_LANG_ZH,
          "「。」(3 字节) 被判成中文 ⇒ 旧的 strlen==3 口径又回来了");
    CHECK(pm_is_zh_char("\xE3\x80\x82") == 0, "pm_is_zh_char(「。」) 应为 0");
    T_END();

    T_START("回归锁: 平假名「あ」(U+3042) 不得判为中文");
    CHECK(pm_lang_of("\xE3\x81\x82") != PM_LANG_ZH,
          "「あ」(3 字节) 被判成中文 ⇒ 旧的 strlen==3 口径又回来了");
    CHECK(pm_lang_of("\xE3\x81\x82") == PM_LANG_JA, "「あ」应为 JA");
    CHECK(pm_is_zh_char("\xE3\x81\x82") == 0, "pm_is_zh_char(「あ」) 应为 0");
    T_END();

    T_START("回归锁: 谚文「가」(U+AC00) 不得判为中文");
    CHECK(pm_lang_of("\xEA\xB0\x80") == PM_LANG_KO, "「가」应为 KO");
    CHECK(pm_is_zh_char("\xEA\xB0\x80") == 0, "pm_is_zh_char(「가」) 应为 0");
    T_END();
}

/* ══════════════ 4. pm_lang_of：首码点 ══════════════ */

static void test_lang_of(void) {
    T_START("lang_of: 首码点判定 + 空/NULL");
    CHECK(pm_lang_of("\xE4\xB8\xAD") == PM_LANG_ZH, "「中」应为 ZH");
    CHECK(pm_lang_of("hello") == PM_LANG_EN, "「hello」应为 EN");
    CHECK(pm_lang_of("3") == PM_LANG_UNKNOWN, "「3」应为 UNKNOWN");
    CHECK(pm_lang_of("") == PM_LANG_UNKNOWN, "空串应为 UNKNOWN");
    CHECK(pm_lang_of(NULL) == PM_LANG_UNKNOWN, "NULL 应为 UNKNOWN");
    CHECK(pm_lang_of("\xE4\xB8\xAD""abc") == PM_LANG_ZH, "「中abc」按首码点应为 ZH");
    T_END();
}

/* ══════════════ 5. pm_lang_of_text：主导语种 ══════════════ */

static void test_lang_of_text(void) {
    T_START("lang_of_text: 主导语种按码点投票");
    CHECK(pm_lang_of_text("\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C") == PM_LANG_ZH,
          "「你好世界」应为 ZH");
    CHECK(pm_lang_of_text("hello world") == PM_LANG_EN, "「hello world」应为 EN");
    CHECK(pm_lang_of_text("\xE4\xBD\xA0\xE5\xA5\xBD hello world foo") == PM_LANG_EN,
          "「你好 hello world foo」拉丁票多 ⇒ EN");
    CHECK(pm_lang_of_text("hi \xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C") == PM_LANG_ZH,
          "「hi 你好世界」中文票多 ⇒ ZH");
    CHECK(pm_lang_of_text("12345") == PM_LANG_UNKNOWN, "纯数字 ⇒ UNKNOWN");
    CHECK(pm_lang_of_text("") == PM_LANG_UNKNOWN, "空串 ⇒ UNKNOWN");
    CHECK(pm_lang_of_text(NULL) == PM_LANG_UNKNOWN, "NULL ⇒ UNKNOWN");
    T_END();
}

/* ══════════════ 6. 比例与存在性 ══════════════ */

static void test_ratio_and_has(void) {
    T_START("zh_ratio_permille / pm_has_zh");
    CHECK(pm_zh_ratio_permille("\xE4\xB8\xAD\xE6\x96\x87") == 1000, "「中文」应为 1000");
    CHECK(pm_zh_ratio_permille("hello") == 0, "纯拉丁应为 0（无非 ASCII）");
    CHECK(pm_zh_ratio_permille(NULL) == 0, "NULL 应为 0");
    CHECK(pm_has_zh("hello \xE4\xBD\xA0\xE5\xA5\xBD") == 1, "「hello 你好」含中文 ⇒ 1");
    CHECK(pm_has_zh("hello") == 0, "「hello」不含中文 ⇒ 0");
    CHECK(pm_has_zh("") == 0, "空串 ⇒ 0");
    CHECK(pm_has_zh(NULL) == 0, "NULL ⇒ 0");
    T_END();
}

/* ══════════════ 7. 谓词 ══════════════ */

static void test_predicates(void) {
    T_START("pm_is_nonascii / pm_is_ascii_text / pm_is_zh_char");
    CHECK(pm_is_nonascii("\xE4\xB8\xAD") == 1, "「中」⇒ 1");
    CHECK(pm_is_nonascii("a") == 0, "「a」⇒ 0");
    CHECK(pm_is_nonascii("\xC3\xA9") == 1, "「é」⇒ 1（非 ASCII 不含语种语义）");
    CHECK(pm_is_nonascii("") == 0, "空串 ⇒ 0");
    CHECK(pm_is_nonascii(NULL) == 0, "NULL ⇒ 0");

    CHECK(pm_is_ascii_text("hello123") == 1, "「hello123」⇒ 1");
    CHECK(pm_is_ascii_text("\xC3\xA9") == 0, "「é」⇒ 0");
    CHECK(pm_is_ascii_text("\xE4\xB8\xAD") == 0, "「中」⇒ 0");
    CHECK(pm_is_ascii_text("") == 0, "空串 ⇒ 0（与旧 is_ascii_word 一致）");
    CHECK(pm_is_ascii_text(NULL) == 0, "NULL ⇒ 0");

    CHECK(pm_is_zh_char("\xE4\xB8\xAD") == 1, "「中」⇒ 1");
    CHECK(pm_is_zh_char("\xE4\xB8\xAD""abc") == 1, "「中abc」按首码点 ⇒ 1");
    CHECK(pm_is_zh_char("\xE3\x80\x82") == 0, "「。」⇒ 0（回归锁，见用例 3）");
    CHECK(pm_is_zh_char("") == 0, "空串 ⇒ 0");
    CHECK(pm_is_zh_char(NULL) == 0, "NULL ⇒ 0");
    T_END();
}

/* ══════════════ 8. 名称映射（对外协议） ══════════════ */

static void test_names(void) {
    const char* names[PM_LANG_COUNT];
    int i, j;

    T_START("pm_lang_name: 6 个标签逐字断言且互不相同");
    names[0] = pm_lang_name(PM_LANG_UNKNOWN);
    names[1] = pm_lang_name(PM_LANG_ZH);
    names[2] = pm_lang_name(PM_LANG_EN);
    names[3] = pm_lang_name(PM_LANG_JA);
    names[4] = pm_lang_name(PM_LANG_KO);
    names[5] = pm_lang_name(PM_LANG_OTHER);

    CHECK(strcmp(names[PM_LANG_UNKNOWN], "unknown") == 0, "UNKNOWN 名称应为 unknown（实为 %s）", names[0]);
    CHECK(strcmp(names[PM_LANG_ZH], "zh") == 0, "ZH 名称应为 zh（实为 %s）", names[1]);
    CHECK(strcmp(names[PM_LANG_EN], "en") == 0, "EN 名称应为 en（实为 %s）", names[2]);
    CHECK(strcmp(names[PM_LANG_JA], "ja") == 0, "JA 名称应为 ja（实为 %s）", names[3]);
    CHECK(strcmp(names[PM_LANG_KO], "ko") == 0, "KO 名称应为 ko（实为 %s）", names[4]);
    CHECK(strcmp(names[PM_LANG_OTHER], "other") == 0, "OTHER 名称应为 other（实为 %s）", names[5]);

    for (i = 0; i < PM_LANG_COUNT; i++)
        for (j = i + 1; j < PM_LANG_COUNT; j++)
            CHECK(strcmp(names[i], names[j]) != 0,
                  "标签重复：%d 与 %d 都是 %s", i, j, names[i]);

    CHECK(PM_LANG_COUNT == 6, "PM_LANG_COUNT 应为 6（实为 %d）", PM_LANG_COUNT);
    T_END();
}

/* ══════════════════════════════ main ══════════════════════════════ */

/* ══════════════ 9. 单字符谓词（v0.5.38 正梁口径） ══════════════ */
/* 这两支谓词是输出层「非表意字符不得进入输出」的唯一判据源：
 *   - pm_is_single_char       ：是不是「一个字符」（按码点，不按字节数）
 *   - pm_is_single_nonzh_char ：是不是「一个非表意字符」（旧歪判据歪打正着压制的那类）
 * 回归锁：一旦有人把「3 字节 = 汉字 / 3 字节 = 单字」的口径又带回来，本组必然变红。 */

static void test_single_char_predicates(void) {
    T_START("pm_is_single_char: 恰一个码点（按码点判，非字节数）");
    CHECK(pm_is_single_char("\xE4\xB8\xAD") == 1, "「中」⇒ 1");
    CHECK(pm_is_single_char("\xE3\x80\x82") == 1, "「。」⇒ 1（3 字节但仍是单字符）");
    CHECK(pm_is_single_char("\xC3\xA9") == 1, "「é」(2字节) ⇒ 1");
    CHECK(pm_is_single_char("\xF0\x9F\x98\x80") == 1, "「😀」(4字节) ⇒ 1");
    CHECK(pm_is_single_char("a") == 1, "「a」⇒ 1");
    CHECK(pm_is_single_char("\xE4\xB8\xAD\xE6\x96\x87") == 0, "「中文」(2 个码点) ⇒ 0");
    CHECK(pm_is_single_char("\xE4\xB8\xAD""a") == 0, "「中a」⇒ 0");
    CHECK(pm_is_single_char("") == 0, "空串 ⇒ 0");
    CHECK(pm_is_single_char(NULL) == 0, "NULL ⇒ 0");
    CHECK(pm_is_single_char("\x80") == 0, "裸续字节（非法码点）⇒ 0");
    T_END();

    T_START("pm_is_single_nonzh_char: 单个非表意字符（三字节区，输出层正梁）");
    CHECK(pm_is_single_nonzh_char("\xE3\x80\x82") == 1, "「。」(CJK 标点/UNKNOWN) ⇒ 1");
    CHECK(pm_is_single_nonzh_char("\xE3\x80\x8C") == 1, "「「」⇒ 1");
    CHECK(pm_is_single_nonzh_char("\xE3\x80\x8D") == 1, "「」」⇒ 1");
    CHECK(pm_is_single_nonzh_char("\xEF\xBC\x8C") == 1, "「，」(全角/UNKNOWN) ⇒ 1");
    CHECK(pm_is_single_nonzh_char("\xE3\x81\x82") == 1, "「あ」(假名/JA) ⇒ 1");
    CHECK(pm_is_single_nonzh_char("\xEF\xBD\xB1") == 1, "「ｱ」(半角片假名/JA) ⇒ 1");
    CHECK(pm_is_single_nonzh_char("\xEA\xB0\x80") == 1, "「가」(谚文/KO) ⇒ 1");
    CHECK(pm_is_single_nonzh_char("\xF0\x9F\x98\x80") == 0, "「😀」(4字节) ⇒ 0（覆盖面外）");
    CHECK(pm_is_single_nonzh_char("\xC2\xB7") == 0, "「·」(2字节) ⇒ 0（覆盖面外）");
    CHECK(pm_is_single_nonzh_char("\xE4\xB8\xAD") == 0, "「中」是汉字 ⇒ 0（归 pm_is_zh_char）");
    CHECK(pm_is_single_nonzh_char("\xC3\xA9") == 0, "「é」(2字节) ⇒ 0（覆盖面外）");
    CHECK(pm_is_single_nonzh_char("a") == 0, "「a」ASCII 单字符 ⇒ 0（不在本列）");
    CHECK(pm_is_single_nonzh_char("3") == 0, "「3」⇒ 0");
    CHECK(pm_is_single_nonzh_char("hello") == 0, "「hello」多字符 ⇒ 0");
    CHECK(pm_is_single_nonzh_char("\xE4\xB8\xAD\xE6\x96\x87") == 0, "「中文」⇒ 0");
    CHECK(pm_is_single_nonzh_char("") == 0, "空串 ⇒ 0");
    CHECK(pm_is_single_nonzh_char(NULL) == 0, "NULL ⇒ 0");
    T_END();

    T_START("覆盖面锁：正梁 == 旧判据（首字节>=0x80 且 strlen==3）的非汉字部分");
    /* 对每个三字节非汉字，正梁必须命中；对每个非三字节单字符，必须不命中。
     * 这条一旦变红，说明正梁与旧判据的足迹不再一致 ⇒ C 步改口径必然劣化。 */
    CHECK(pm_is_single_nonzh_char("\xE4\xB8\xAD") == 0, "三字节「中」是汉字 ⇒ 正梁不接（归 pm_is_zh_char）");
    CHECK(pm_is_single_nonzh_char("\xE3\x80\x82") == 1, "三字节「。」非汉字 ⇒ 正梁接管");
    CHECK(pm_is_single_nonzh_char("\xC3\xA9") == 0, "二字节「é」⇒ 正梁不接（足迹外）");
    CHECK(pm_is_single_nonzh_char("\xF0\x9F\x98\x80") == 0, "四字节「😀」⇒ 正梁不接（足迹外）");
    T_END();

    T_START("正梁 ∪ 单汉字 = 单个多字节字符（与旧 strlen==3 口径对照）");
    /* 旧判据 `首字节>=0x80 && strlen==3` 压制的 3 字节字符，两种归类合并后应完全覆盖：
     * 汉字走 pm_is_zh_char（C 步），非汉字走 pm_is_single_nonzh_char（B 步正梁）。 */
    CHECK(pm_is_single_nonzh_char("\xE3\x80\x82") || pm_is_zh_char("\xE3\x80\x82"),
          "「。」必须被两者之一命中（否则改口径后必然漏出）");
    CHECK(pm_is_single_nonzh_char("\xE3\x81\x82") || pm_is_zh_char("\xE3\x81\x82"),
          "「あ」必须被两者之一命中");
    CHECK(pm_is_single_nonzh_char("\xE4\xB8\xAD") || pm_is_zh_char("\xE4\xB8\xAD"),
          "「中」必须被两者之一命中");
    T_END();
}


int main(void) {
    printf("\n=== 语种判定 SSOT 契约单测 (include/lang.h) ===\n\n");

    test_decode_basic();
    test_decode_invalid();
    test_lang_of_cp();
    test_regression_three_byte();
    test_lang_of();
    test_lang_of_text();
    test_ratio_and_has();
    test_predicates();
    test_single_char_predicates();
    test_names();

    printf("\n");
    printf("  用例: %d 运行, %d 通过, %d 失败\n", tests_run, tests_passed, tests_failed);
    printf("\n=== 完成 ===\n");
    return (tests_failed == 0) ? 0 : 1;
}
