/* ============================================================
 * @file script.c
 * @brief 文字体系（Script System）抽象层 — 实现
 *
 * 按 Unicode 区段判定 UTF-8 字符所属的文字体系，替代散落的 ASCII 二分
 * 判断。这是语言皮层按文字体系分层的基石。
 * ============================================================ */

#include "script.h"
#include <string.h>

/* 静态属性表，与 ScriptType 枚举一一对应 */
static const ScriptInfo SCRIPT_TABLE[SCRIPT_COUNT] = {
    { SCRIPT_UNKNOWN,    0, 0, "unknown"     },
    { SCRIPT_LATIN,      1, 0, "latin"       },
    { SCRIPT_CJK,        0, 0, "cjk"         },
    { SCRIPT_HIRAGANA,   0, 0, "hiragana"    },
    { SCRIPT_KATAKANA,   0, 0, "katakana"    },
    { SCRIPT_HANGUL,     0, 0, "hangul"      },
    { SCRIPT_CYRILLIC,   1, 0, "cyrillic"    },
    { SCRIPT_ARABIC,     1, 1, "arabic"      },
    { SCRIPT_GREEK,      1, 0, "greek"       },
    { SCRIPT_DEVANAGARI, 0, 0, "devanagari"  },
    { SCRIPT_DIGIT,      0, 0, "digit"       },
    { SCRIPT_PUNCT,      0, 0, "punct"       },
};

const ScriptInfo* script_info(ScriptType s) {
    if (s < 0 || s >= SCRIPT_COUNT) return &SCRIPT_TABLE[0];
    return &SCRIPT_TABLE[s];
}

/* 解析 UTF-8 字符为 Unicode 码点。返回码点（>=0），失败返回 -1。
 * 输入 p 指向某字符首字节，自动消费 1~4 字节。 */
static int utf8_codepoint(const char* p) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x80) return c;                     /* ASCII */
    if ((c & 0xE0) == 0xC0) {                   /* 2 字节 */
        if (p[1] == '\0') return -1;
        return ((c & 0x1F) << 6) | ((unsigned char)p[1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0) {                   /* 3 字节 */
        if (p[1] == '\0' || p[2] == '\0') return -1;
        return ((c & 0x0F) << 12) | (((unsigned char)p[1] & 0x3F) << 6)
               | ((unsigned char)p[2] & 0x3F);
    }
    if ((c & 0xF8) == 0xF0) {                   /* 4 字节 */
        if (p[1] == '\0' || p[2] == '\0' || p[3] == '\0') return -1;
        return ((c & 0x07) << 18) | (((unsigned char)p[1] & 0x3F) << 12)
               | (((unsigned char)p[2] & 0x3F) << 6)
               | ((unsigned char)p[3] & 0x3F);
    }
    return -1;
}

ScriptType script_detect(const char* p) {
    if (!p || *p == '\0') return SCRIPT_UNKNOWN;
    int cp = utf8_codepoint(p);
    if (cp < 0) return SCRIPT_UNKNOWN;

    /* 数字 */
    if (cp >= '0' && cp <= '9') return SCRIPT_DIGIT;

    /* ASCII 标点 / 空白 / 控制符 */
    if (cp < 0x80) {
        /* ASCII 字母 [A-Za-z] 归拉丁 */
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) return SCRIPT_LATIN;
        return SCRIPT_PUNCT;
    }

    /* 拉丁扩展：Latin-1 Supplement、Latin Extended-A/B、拉丁补充 */
    if ((cp >= 0x00C0 && cp <= 0x024F) ||     /* Latin-1 + Extended-A */
        (cp >= 0x1E00 && cp <= 0x1EFF))       /* Latin Extended Additional */
        return SCRIPT_LATIN;

    /* 希腊 */
    if ((cp >= 0x0370 && cp <= 0x03FF) || (cp >= 0x1F00 && cp <= 0x1FFF))
        return SCRIPT_GREEK;

    /* 西里尔 */
    if ((cp >= 0x0400 && cp <= 0x052F)) return SCRIPT_CYRILLIC;

    /* 阿拉伯 */
    if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F))
        return SCRIPT_ARABIC;

    /* 天城文 */
    if (cp >= 0x0900 && cp <= 0x097F) return SCRIPT_DEVANAGARI;

    /* 日文平假名 / 片假名 */
    if (cp >= 0x3040 && cp <= 0x309F) return SCRIPT_HIRAGANA;
    if (cp >= 0x30A0 && cp <= 0x30FF) return SCRIPT_KATAKANA;

    /* CJK 统一表意文字（含扩展） */
    if ((cp >= 0x4E00 && cp <= 0x9FFF) ||     /* 基本区 */
        (cp >= 0x3400 && cp <= 0x4DBF) ||     /* 扩展 A */
        (cp >= 0x20000 && cp <= 0x2A6DF))     /* 扩展 B */
        return SCRIPT_CJK;

    /* 韩文谚文 */
    if ((cp >= 0xAC00 && cp <= 0xD7AF) ||     /* 音节 */
        (cp >= 0x1100 && cp <= 0x11FF) ||     /* Jamo */
        (cp >= 0x3130 && cp <= 0x318F))       /* 兼容 Jamo */
        return SCRIPT_HANGUL;

    /* 中日韩标点 / 全角 */
    if ((cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFF00 && cp <= 0xFFEF))
        return SCRIPT_PUNCT;

    return SCRIPT_UNKNOWN;
}

int script_is_whitespace(const char* p) {
    if (!p) return 0;
    unsigned char c = (unsigned char)*p;
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0B || c == 0x0C);
}

int script_is_letter(const char* p) {
    ScriptType s = script_detect(p);
    /* 文字体系里，真正承载语义的是「字母/表意字」类，数字和标点不算 */
    switch (s) {
        case SCRIPT_LATIN:
        case SCRIPT_CJK:
        case SCRIPT_HIRAGANA:
        case SCRIPT_KATAKANA:
        case SCRIPT_HANGUL:
        case SCRIPT_CYRILLIC:
        case SCRIPT_ARABIC:
        case SCRIPT_GREEK:
        case SCRIPT_DEVANAGARI:
            return 1;
        default:
            return 0;
    }
}
