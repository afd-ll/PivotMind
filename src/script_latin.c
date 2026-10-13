/* ============================================================
 * @file script_latin.c
 * @brief 拉丁语系皮层 — 实现
 *
 * 承载拉丁字母体系的词边界 + 形态学处理。取代 v0.5.25 english.c。
 * ============================================================ */

#include "script_latin.h"
#include "script.h"
#include <string.h>

/* URL/编码分隔字符 */
static int is_url_sep(char c) {
    switch (c) {
        case '/': case '\\': case ':': case '?': case '=':
        case '&': case '%': case '#': case '@': case '+':
            return 1;
        default:
            return 0;
    }
}

static int is_word_char(char c) {
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    if (c == '-' || c == '_' || c == '\'') return 1;
    return 0;
}

/* 循环串检测（FBFBFB、PIPIPI 等） */
static int is_cyclic_junk(const char* word, int len) {
    if (len <= 3) return 0;
    char seen[256];
    memset(seen, 0, sizeof(seen));
    int distinct = 0;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)word[i];
        if (!seen[c]) { seen[c] = 1; distinct++; }
    }
    if (len >= 8 && distinct <= 3) return 1;
    if (len >= 5 && distinct == 1) return 1;
    return 0;
}

int latin_word_valid(const char* word, int len) {
    if (!word || len <= 0) return 0;
    if (len > LATIN_WORD_MAX_LEN) return 0;

    for (int i = 0; i < len; i++) {
        char c = word[i];
        if (is_url_sep(c)) return 0;
        if (!is_word_char(c)) return 0;
    }
    if (is_cyclic_junk(word, len)) return 0;

    /* 纯数字串（>2）拒收 */
    int all_digit = 1;
    for (int i = 0; i < len; i++) {
        if (!(word[i] >= '0' && word[i] <= '9')) { all_digit = 0; break; }
    }
    if (all_digit && len > 2) return 0;

    return 1;
}

int latin_extract_word(const char* p, int* out_len) {
    if (!p || !out_len) return 0;
    *out_len = 0;

    const char* start = p;
    int len = 0;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        /* 非拉丁（中文/其他语系）停止 */
        if (c >= 0x80 && script_detect(p) != SCRIPT_LATIN) break;
        if (c >= 0x80 && script_detect(p) == SCRIPT_LATIN) {
            /* 拉丁扩展字符（é、ñ 等）：占 2 字节，计入长度 */
            int blen = ((c & 0xE0) == 0xC0) ? 2 : 1;
            len += blen;
            p += blen;
            continue;
        }
        if (script_is_whitespace(p)) break;
        if (c == ',' || c == '.' || c == ';' || c == ':' || c == '!' ||
            c == '?' || c == '(' || c == ')' || c == '[' || c == ']' ||
            c == '{' || c == '}' || c == '<' || c == '>' || c == '"') break;
        len++;
        p++;
    }

    if (len == 0) return 0;
    if (latin_word_valid(start, len)) {
        *out_len = len;
        return 1;
    }
    *out_len = len;
    return 0;
}

/* 轻量词干化：剥离常见拉丁语系形态后缀。
 * 只处理英文高频后缀（复数/时态/比较级），保守起见仅在词足够长时剥离。 */
int latin_stem(const char* word, int len, char* out, int out_sz) {
    if (!word || !out || len <= 0) return 0;
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, word, len);
    out[len] = '\0';

    int n = len;

    /* 复数/第三人称：ies → y（cities→city） */
    if (n > 4 && out[n-3]=='i' && out[n-2]=='e' && out[n-1]=='s') {
        out[n-3] = 'y'; out[n-2] = '\0'; return n - 2;
    }
    /* 复数/所有格：s（cats→cat），但避免 ss 结尾误删 */
    if (n > 4 && out[n-1]=='s' && out[n-2]!='s' && out[n-2]!='u') {
        out[n-1] = '\0'; return n - 1;
    }
    /* es 复数：boxes→box */
    if (n > 4 && out[n-2]=='e' && out[n-1]=='s') {
        out[n-2] = '\0'; return n - 2;
    }
    /* 进行时：ing（walking→walk；running→run 简化为 runn） */
    if (n > 5 && out[n-3]=='i' && out[n-2]=='n' && out[n-1]=='g') {
        out[n-3] = '\0'; return n - 3;
    }
    /* 过去式：ed（walked→walk） */
    if (n > 4 && out[n-2]=='e' && out[n-1]=='d') {
        out[n-2] = '\0'; return n - 2;
    }
    /* 比较级：er（taller→tall）、est（tallest→tall） */
    if (n > 5 && out[n-3]=='e' && out[n-2]=='s' && out[n-1]=='t') {
        out[n-3] = '\0'; return n - 3;
    }
    if (n > 4 && out[n-2]=='e' && out[n-1]=='r') {
        out[n-2] = '\0'; return n - 2;
    }

    return n; /* 无后缀可剥离 */
}
