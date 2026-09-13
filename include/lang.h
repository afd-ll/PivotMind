#ifndef PM_LANG_H
#define PM_LANG_H

/**
 * @file lang.h
 * @brief 语种判定 SSOT —— 多语种分离的【唯一定义源】
 *
 * ============================ 为什么有这个文件 ============================
 * v0.6「多语种分离」动工前的盘点发现：语种判定在本仓散落 9 处，且口径分成三档
 * 互不相同，各有错判：
 *
 *   ① 宽档：`(unsigned char)c & 0x80`         —— 任何非 ASCII 都算「中文」
 *            （utf8_tokenizer.c / diffusion.c 的 NODE_IS_CJK / gateway_handlers.c）
 *            错：法文 é、德文 ü、日文假名、韩文谚文、emoji、CJK 标点全被算成中文。
 *   ② 中档：`strlen(s) == 3 && 首字节 E0-EF`  —— 「3 字节就算汉字」
 *            （autonomic_learner.c / compound_promote.c）
 *            错：CJK 标点（。是 E3 80 82）、平假名/片假名（E3 81 82）、谚文
 *                都是 3 字节，被误当汉字。
 *   ③ 严档：3 字节且码点落在 0x4E00..0x9FFF  —— 只认基本区
 *            （chinese.c）
 *
 * 三档并存 ⇒ 同一个词在不同子系统里会被判成不同语种。**分离语种之前，
 * 必须先让「语种是什么」只有一个答案。** 本文件就是那个答案。
 *
 * 设计口径：
 *   - **码点级，不是字节级**：一律先 pm_utf8_decode 解出码点，再按 Unicode 区块归属。
 *   - 与显示宽度 SSOT（src/ui.c 的 ui_disp_width）同源思路：一个维度一个真值源。
 *   - Makefile 的 `CORE_SRC = $(wildcard src/*.c)` 会自动纳入 src/lang.c，无需改 Makefile。
 *
 * 用法约定：
 *   - **新代码一律用本文件的 API，不要再写裸判据**（`& 0x80` / `strlen==3` / 裸码点比较）。
 *   - 需要「非 ASCII」这种字节级粗判时用 pm_is_nonascii()，不要内联位运算。
 * ==========================================================================
 */

#include <stddef.h>

/** 语种标签 —— 多语种分离的基础维度 */
typedef enum {
    PM_LANG_UNKNOWN = 0,   /* 无法归类：空串、纯数字、纯标点、纯符号 */
    PM_LANG_ZH      = 1,   /* 中文：CJK 统一表意（基本区 4E00-9FFF + 扩展 A 3400-4DBF） */
    PM_LANG_EN      = 2,   /* 英文：拉丁字母（基本拉丁 + 拉丁扩展 A/B） */
    PM_LANG_JA      = 3,   /* 日文：平假名 / 片假名 */
    PM_LANG_KO      = 4,   /* 韩文：谚文音节 / 谚文字母 */
    PM_LANG_OTHER   = 5    /* 其它文字：希腊/西里尔/希伯来/阿拉伯/天城文/emoji */
} PmLang;

/** 语种标签总数（= PM_LANG_OTHER + 1），供「按语种开数组」用。 */
#define PM_LANG_COUNT 6

/** 语种标签的稳定短名："zh" / "en" / "ja" / "ko" / "other" / "unknown"。
 *  用于日志与持久化 —— **不要拿枚举数值当外部协议**。 */
const char* pm_lang_name(PmLang lang);

/* ────────────────────── 码点解码（全仓唯一实现） ────────────────────── */

/**
 * 从 s 解出首个码点，返回其占用字节数（>=1），码点写入 *cp_out。
 * 非法/不完整序列 ⇒ 返回 1 且 *cp_out = 0（调用方按「无效码点」处理）。
 * 与 src/ui.c 的 ui_utf8_decode 同款语义（ui.c 那份是 static，待后续复用本函数）。
 */
int pm_utf8_decode(const char* s, unsigned int* cp_out);

/* ────────────────────────── 语种判定 ────────────────────────── */

/** 单个码点 → 语种。数字/标点/控制符 ⇒ PM_LANG_UNKNOWN。 */
PmLang pm_lang_of_cp(unsigned int cp);

/** 取【首码点】判定（这就是旧 NODE_IS_CJK / is_ascii_token 等「看首字节」的正确版本）。 */
PmLang pm_lang_of(const char* concept);

/** 整串文本的【主导语种】：按码点投票，得票最高者胜，平票取先出现者。
 *  纯数字/纯标点 ⇒ PM_LANG_UNKNOWN；空串/NULL ⇒ PM_LANG_UNKNOWN。 */
PmLang pm_lang_of_text(const char* text);

/** 文本里中文码点占「非 ASCII 码点」的千分比（0..1000）。
 *  没有非 ASCII 码点 ⇒ 0。用于「中文输入不该回英文」这类阈值判断。 */
int pm_zh_ratio_permille(const char* text);

/** 文本里是否【存在】中文码点（注意：≠ 主导语种。「hello 你好」⇒ 1）。 */
int pm_has_zh(const char* text);

/* ────────────── 语义明确的谓词（各对应原散落实现的档位） ────────────── */

/** 首字节带高位（`& 0x80`）—— 承接旧 is_chinese()/NODE_IS_CJK 的「非 ASCII」粗判。
 *  ⚠ 这是字节级粗判，**不含语种语义**；能用 pm_lang_of() 就别用它。 */
int pm_is_nonascii(const char* s);

/** 首码点落在 CJK 表意区 —— 承接旧 `strlen==3` 汉字判据，但排除 CJK 标点/假名/谚文。 */
int pm_is_zh_char(const char* s);

/** 全部字节 < 0x80（空串 ⇒ 0）—— 承接旧 is_ascii_word() / is_ascii_token()。 */
int pm_is_ascii_text(const char* s);

#endif /* PM_LANG_H */
