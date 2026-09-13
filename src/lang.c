/**
 * @file lang.c
 * @brief 语种判定 SSOT 实现 —— 设计与口径见 include/lang.h
 */

#include "lang.h"

/* ────────────────────── 码点解码（唯一实现） ────────────────────── */

int pm_utf8_decode(const char* s, unsigned int* cp_out) {
    const unsigned char* u = (const unsigned char*)s;
    unsigned char c;
    unsigned int cp;
    int len, i;

    if (cp_out) *cp_out = 0u;
    if (!s) return 1;

    c = u[0];
    if (c == 0u) return 1;                       /* 空串：无效码点，占 1 字节 */

    if (c < 0x80u)                 { cp = c;         len = 1; }
    else if ((c & 0xE0u) == 0xC0u) { cp = c & 0x1Fu; len = 2; }
    else if ((c & 0xF0u) == 0xE0u) { cp = c & 0x0Fu; len = 3; }
    else if ((c & 0xF8u) == 0xF0u) { cp = c & 0x07u; len = 4; }
    else                           { return 1; }  /* 非法首字节（含 0x80-0xBF 续字节） */

    for (i = 1; i < len; i++) {
        if ((u[i] & 0xC0u) != 0x80u) { len = i; break; }   /* 截断：按已解出的部分算 */
        cp = (cp << 6) | (unsigned int)(u[i] & 0x3Fu);
    }
    if (len < 1) len = 1;

    if (cp_out) *cp_out = cp;
    return len;
}

/* ────────────────────────── 语种判定 ────────────────────────── */

PmLang pm_lang_of_cp(unsigned int cp) {
    /* ── 中文：CJK 统一表意（基本区 + 扩展 A） ── */
    if (cp >= 0x4E00u && cp <= 0x9FFFu) return PM_LANG_ZH;
    if (cp >= 0x3400u && cp <= 0x4DBFu) return PM_LANG_ZH;

    /* ── 日文：平假名 / 片假名（含片假名半角在 0xFF66-0xFF9D，归此处更贴切） ── */
    if (cp >= 0x3040u && cp <= 0x30FFu) return PM_LANG_JA;
    if (cp >= 0xFF66u && cp <= 0xFF9Du) return PM_LANG_JA;

    /* ── 韩文：谚文音节 + 谚文字母 ── */
    if (cp >= 0xAC00u && cp <= 0xD7AFu) return PM_LANG_KO;
    if (cp >= 0x1100u && cp <= 0x11FFu) return PM_LANG_KO;

    /* ── 英文：拉丁字母（基本拉丁 + 拉丁扩展 A/B，覆盖 é ü ñ ç 等带音标字母） ── */
    if (cp >= 0x41u && cp <= 0x5Au) return PM_LANG_EN;   /* A-Z */
    if (cp >= 0x61u && cp <= 0x7Au) return PM_LANG_EN;   /* a-z */
    if (cp >= 0x00C0u && cp <= 0x024Fu) return PM_LANG_EN;

    /* ── 其它文字 ── */
    if ((cp >= 0x0370u && cp <= 0x03FFu) ||   /* 希腊 */
        (cp >= 0x0400u && cp <= 0x04FFu) ||   /* 西里尔 */
        (cp >= 0x0590u && cp <= 0x05FFu) ||   /* 希伯来 */
        (cp >= 0x0600u && cp <= 0x06FFu) ||   /* 阿拉伯 */
        (cp >= 0x0900u && cp <= 0x097Fu) ||   /* 天城文 */
        (cp >= 0x1F300u && cp <= 0x1FAFFu))   /* emoji */
        return PM_LANG_OTHER;

    /* 数字、ASCII 标点、空白、控制符、以及其余 Unicode ⇒ 无法归类 */
    return PM_LANG_UNKNOWN;
}

PmLang pm_lang_of(const char* concept) {
    unsigned int cp = 0u;
    if (!concept || !concept[0]) return PM_LANG_UNKNOWN;
    pm_utf8_decode(concept, &cp);
    return pm_lang_of_cp(cp);
}

PmLang pm_lang_of_text(const char* text) {
    int votes[PM_LANG_COUNT];
    int best = PM_LANG_UNKNOWN;
    int i;
    const char* p;

    if (!text) return PM_LANG_UNKNOWN;
    for (i = 0; i < PM_LANG_COUNT; i++) votes[i] = 0;

    for (p = text; *p; ) {
        unsigned int cp = 0u;
        int len = pm_utf8_decode(p, &cp);
        if (len < 1) len = 1;
        if (cp != 0u) votes[pm_lang_of_cp(cp)]++;
        p += len;
    }

    /* 从 ZH 起比（不跟 UNKNOWN 抢），平票取先出现者 */
    for (i = PM_LANG_ZH; i < PM_LANG_COUNT; i++)
        if (votes[i] > votes[best]) best = i;
    return (PmLang)best;
}

int pm_zh_ratio_permille(const char* text) {
    int zh = 0, nonascii = 0;
    const char* p;
    if (!text) return 0;
    for (p = text; *p; ) {
        unsigned int cp = 0u;
        int len = pm_utf8_decode(p, &cp);
        if (len < 1) len = 1;
        if (cp != 0u) {
            if (cp > 0x7Fu) nonascii++;
            if (pm_lang_of_cp(cp) == PM_LANG_ZH) zh++;
        }
        p += len;
    }
    return (nonascii > 0) ? (zh * 1000 / nonascii) : 0;
}

int pm_has_zh(const char* text) {
    const char* p;
    if (!text) return 0;
    for (p = text; *p; ) {
        unsigned int cp = 0u;
        int len = pm_utf8_decode(p, &cp);
        if (len < 1) len = 1;
        if (cp != 0u && pm_lang_of_cp(cp) == PM_LANG_ZH) return 1;
        p += len;
    }
    return 0;
}

/* ────────────── 语义明确的谓词（各对应原散落实现的档位） ────────────── */

int pm_is_nonascii(const char* s) {
    return (s && (((unsigned char)s[0]) & 0x80u) != 0) ? 1 : 0;
}

int pm_is_ascii_text(const char* s) {
    const unsigned char* p;
    if (!s || !s[0]) return 0;
    for (p = (const unsigned char*)s; *p; p++)
        if (*p > 0x7Fu) return 0;
    return 1;
}

int pm_is_zh_char(const char* s) {
    unsigned int cp = 0u;
    if (!s || !s[0]) return 0;
    pm_utf8_decode(s, &cp);
    return (pm_lang_of_cp(cp) == PM_LANG_ZH) ? 1 : 0;
}

/* ────────────────────────── 名称映射 ────────────────────────── */

const char* pm_lang_name(PmLang lang) {
    switch (lang) {
        case PM_LANG_ZH:    return "zh";
        case PM_LANG_EN:    return "en";
        case PM_LANG_JA:    return "ja";
        case PM_LANG_KO:    return "ko";
        case PM_LANG_OTHER: return "other";
        case PM_LANG_UNKNOWN:
        default:            return "unknown";
    }
}
