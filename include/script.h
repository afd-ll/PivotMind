#ifndef SCRIPT_H
#define SCRIPT_H

/* ============================================================
 * @file script.h
 * @brief 文字体系（Script System）抽象层 — 语言皮层按文字体系分层
 *
 * 背景（v0.5.26）：原语言处理以「中文字符集」为默认，英文/拉丁语系被
 * 当成「非中文」用 (unsigned char)x < 0x80 零散判断，散落 30+ 处补丁。
 * 本模块把「文字体系」提升为一等概念：
 *
 *   编码层统一（UTF-8 字节） → 按 Unicode 区段分流 → 形态层分离
 *   → 汇入语言无关的共享语义空间（概念特征向量）
 *
 * 借鉴 DeepSeek Byte-level BPE 的「编码不判语言、语系区分发生在结构层」
 * 思想。英文不是孤立语言，是拉丁字母体系（Latin script）的一员，与
 * 法/德/西/葡等共享同一套字母、空格分词、词缀构词规则。
 * ============================================================ */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 文字体系枚举 ── */
typedef enum {
    SCRIPT_UNKNOWN  = 0,  /* 无法判定（控制符/其他） */
    SCRIPT_LATIN    = 1,  /* 拉丁字母：英/法/德/西/葡/越/土/印尼… */
    SCRIPT_CJK      = 2,  /* 表意文字：中文/日文汉字 */
    SCRIPT_HIRAGANA = 3,  /* 日文平假名 */
    SCRIPT_KATAKANA = 4,  /* 日文片假名 */
    SCRIPT_HANGUL   = 5,  /* 韩文谚文 */
    SCRIPT_CYRILLIC = 6,  /* 西里尔：俄/乌/保/塞… */
    SCRIPT_ARABIC   = 7,  /* 阿拉伯（从右到左，连写） */
    SCRIPT_GREEK    = 8,  /* 希腊字母 */
    SCRIPT_DEVANAGARI = 9,/* 天城文：印地/梵… */
    SCRIPT_DIGIT    = 10, /* 数字（通用，非语系） */
    SCRIPT_PUNCT    = 11, /* 标点/符号（通用，非语系） */
    SCRIPT_COUNT
} ScriptType;

/* ── 文字体系属性 ── */
typedef struct {
    ScriptType script;      /* 所属文字体系 */
    int uses_spaces;        /* 是否靠空格分词（拉丁/西里尔=1，CJK=0） */
    int is_rtl;             /* 是否从右到左（阿拉伯=1） */
    const char* name;       /* 人类可读名 */
} ScriptInfo;

/* 判定一个 UTF-8 字符（首字节指针）所属的文字体系。
 * 输入指向某字符的首字节，函数自动识别 1~4 字节 UTF-8 编码。 */
ScriptType script_detect(const char* p);

/* 获取文字体系属性（供各皮层查询分词/构词规则）。 */
const ScriptInfo* script_info(ScriptType s);

/* 判断该字符是否为「词分隔符」（空白，用于空格分词体系）。
 * 注意：这是通用空白判断，与语系无关。 */
int script_is_whitespace(const char* p);

/* 判断该 UTF-8 字符是否为字母（属于某种可分词文字体系，而非标点/数字）。
 * 用于「连续字母成词」的通用判断，替代散落的 ASCII 二分。 */
int script_is_letter(const char* p);

#ifdef __cplusplus
}
#endif

#endif /* SCRIPT_H */
