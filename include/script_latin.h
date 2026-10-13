#ifndef SCRIPT_LATIN_H
#define SCRIPT_LATIN_H

/* ============================================================
 * @file script_latin.h
 * @brief 拉丁语系皮层（Latin script cortex）— 词边界 + 形态学
 *
 * 拉丁字母体系覆盖英/法/德/西/葡/越/土/印尼等数百种语言，共享：
 *   - 空格分词（uses_spaces=1）
 *   - 词缀构词（复数 -s、时态 -ed、前缀 pre-/re- 等）
 * 本模块承载拉丁语系的「形态层」，与 CJK 表意皮层（按字切分）分离。
 *
 * 取代 v0.5.25 的 english.c（英文词校验），升级为语系级处理。
 * ============================================================ */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 拉丁词最长字节数（超出判定为 URL/哈希/垃圾） */
#define LATIN_WORD_MAX_LEN 24

/* 拉丁词校验：1 = 合法拉丁词，0 = 垃圾（URL/编码/循环串/超长/非法字符） */
int latin_word_valid(const char* word, int len);

/* 从文本当前位置提取下一个拉丁词。
 * 返回 1 且 *out_len>0 表示合法词；返回 0 表示该位置非合法词
 * （*out_len 告知调用方应跳过多长）。 */
int latin_extract_word(const char* p, int* out_len);

/* 词干化（stemming，轻量后缀剥离）：
 * 把 walking→walk、running→run、cats→cat、boxes→box。
 * 用于让同一词根的形态变体共享语义节点，而非各自独立。
 * 输出写入 out（调用方保证足够空间，建议 >= 原词长）。
 * 返回剥离后的长度。 */
int latin_stem(const char* word, int len, char* out, int out_sz);

#ifdef __cplusplus
}
#endif

#endif /* SCRIPT_LATIN_H */
