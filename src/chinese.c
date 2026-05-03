#include "chinese.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

// 初始化分词器
Tokenizer* tokenizer_create(int max_tokens) {
    Tokenizer* tokenizer = (Tokenizer*)malloc(sizeof(Tokenizer));
    if (!tokenizer) return NULL;

    tokenizer->max_tokens = max_tokens;
    tokenizer->tokens = (Token*)malloc(sizeof(Token) * max_tokens);
    tokenizer->token_count = 0;

    if (!tokenizer->tokens) {
        free(tokenizer);
        return NULL;
    }

    return tokenizer;
}

// 销毁分词器
void tokenizer_destroy(Tokenizer* tokenizer) {
    if (!tokenizer) return;

    for (int i = 0; i < tokenizer->token_count; i++) {
        if (tokenizer->tokens[i].text) {
            free(tokenizer->tokens[i].text);
        }
    }

    free(tokenizer->tokens);
    free(tokenizer);
}

// 获取字符类型
CharType get_char_type(const char* str) {
    if (!str || *str == '\0') return CH_INVALID;

    unsigned char c = (unsigned char)*str;

    if (c < 0x80) {
        return CH_UTF8_1BYTE;
    } else if ((c & 0xE0) == 0xC0) {
        return CH_UTF8_2BYTE;
    } else if ((c & 0xF0) == 0xE0) {
        return CH_UTF8_3BYTE;
    } else if ((c & 0xF8) == 0xF0) {
        return CH_UTF8_4BYTE;
    }

    return CH_INVALID;
}

// 获取字符长度(字节数)
int get_char_bytes(const char* str) {
    if (!str || *str == '\0') return 0;

    unsigned char c = (unsigned char)*str;

    if (c < 0x80) {
        return 1;
    } else if ((c & 0xE0) == 0xC0) {
        return 2;
    } else if ((c & 0xF0) == 0xE0) {
        return 3;
    } else if ((c & 0xF8) == 0xF0) {
        return 4;
    }

    return 1; // 默认1字节
}

// 判断是否为中文字符
int is_chinese_char(const char* str) {
    if (!str) return 0;

    CharType type = get_char_type(str);

    // 3字节UTF-8通常表示中文字符
    if (type == CH_UTF8_3BYTE) {
        // 检查是否在中文Unicode范围内
        unsigned char c1 = (unsigned char)str[0];
        unsigned char c2 = (unsigned char)str[1];
        unsigned char c3 = (unsigned char)str[2];

        // 转换为Unicode码点
        int codepoint = ((c1 & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);

        // 基本汉字范围: 0x4E00 - 0x9FFF
        if (codepoint >= 0x4E00 && codepoint <= 0x9FFF) {
            return 1;
        }
    }

    return 0;
}

// 判断是否为标点符号
int is_punctuation(const char* str) {
    if (!str || *str == '\0') return 0;

    CharType type = get_char_type(str);

    if (type == CH_UTF8_1BYTE) {
        // ASCII标点
        char c = *str;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return 1;
        if (c == ',' || c == '.' || c == '?' || c == '!') return 1;
        if (c == ';' || c == ':' || c == '\'' || c == '"') return 1;
        if (c == '(' || c == ')' || c == '[' || c == ']') return 1;
        if (c == '{' || c == '}' || c == '<' || c == '>') return 1;
    } else if (type == CH_UTF8_3BYTE) {
        // 中文标点
        unsigned char c1 = (unsigned char)str[0];
        unsigned char c2 = (unsigned char)str[1];
        unsigned char c3 = (unsigned char)str[2];
        int codepoint = ((c1 & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);

        // 中文标点范围
        if (codepoint >= 0x3000 && codepoint <= 0x303F) return 1;
        if (codepoint >= 0xFF00 && codepoint <= 0xFFEF) return 1;
    }

    return 0;
}

// 分词 - 将字符串分割为token数组
int tokenize(Tokenizer* tokenizer, const char* text) {
    if (!tokenizer || !text) return 0;

    tokenizer_clear(tokenizer);

    int pos = 0;
    int text_len = strlen(text);

    while (pos < text_len && tokenizer->token_count < tokenizer->max_tokens) {
        // 跳过空白字符
        while (pos < text_len && (text[pos] == ' ' || text[pos] == '\t')) {
            pos++;
        }
        if (pos >= text_len) break;

        int start_pos = pos;
        int token_len = 0;
        int char_count = 0;
        int is_chinese_word = 0;

        // 判断第一个字符类型
        CharType first_type = get_char_type(&text[pos]);
        int bytes = get_char_bytes(&text[pos]);

        if (is_punctuation(&text[pos])) {
            // 标点符号单独作为一个token
            token_len = bytes;
            char_count = 1;
            pos += bytes;  // 重要：移动pos指针
        } else if (first_type == CH_UTF8_1BYTE) {
            // 英文单词：读取连续的ASCII字符
            while (pos < text_len && text[pos] >= 32 && text[pos] <= 126 &&
                   text[pos] != ' ' && text[pos] != '\t' && text[pos] != '\n') {
                token_len++;
                char_count++;
                pos++;
            }
        } else {
            // 中文字符：每个字单独作为一个token(简单分词)
            is_chinese_word = 1;
            token_len = bytes;
            char_count = 1;
            pos += bytes;

            // 可选：合并连续的中文字符为词
            // 简单实现：每个字单独分词
        }

        // 创建token
        Token* token = &tokenizer->tokens[tokenizer->token_count];
        token->start_pos = start_pos;
        token->length = token_len;
        token->char_count = char_count;
        token->is_chinese = is_chinese_word;

        // 分配内存并复制文本
        token->text = (char*)malloc(token_len + 1);
        if (token->text) {
            strncpy(token->text, &text[start_pos], token_len);
            token->text[token_len] = '\0';
        }

        tokenizer->token_count++;
    }

    return tokenizer->token_count;
}

// 清空token数组
void tokenizer_clear(Tokenizer* tokenizer) {
    if (!tokenizer) return;

    for (int i = 0; i < tokenizer->token_count; i++) {
        if (tokenizer->tokens[i].text) {
            free(tokenizer->tokens[i].text);
            tokenizer->tokens[i].text = NULL;
        }
    }

    tokenizer->token_count = 0;
}

// 获取字符数(不是字节数)
size_t utf8_strlen(const char* str) {
    if (!str) return 0;

    size_t len = 0;
    size_t i = 0;

    while (str[i] != '\0') {
        int bytes = get_char_bytes(&str[i]);
        if (bytes > 0) {
            len++;
            i += bytes;
        } else {
            i++;
        }
    }

    return len;
}

// UTF-8 字符串比较
int utf8_strcmp(const char* s1, const char* s2) {
    return strcmp(s1, s2);
}

// 设置控制台编码为UTF-8(Windows)
void set_console_utf8() {
#ifdef _WIN32
    // 设置控制台代码页为UTF-8 (CP 65001)
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
}

// 打印UTF-8字符串
void print_utf8(const char* str) {
    if (str) {
        printf("%s", str);
    }
}

// 中文字符串查找
const char* utf8_strstr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return NULL;
    return strstr(haystack, needle);
}

// 获取子字符串
char* utf8_substr(const char* str, int start, int length) {
    if (!str) return NULL;

    size_t total_chars = utf8_strlen(str);
    if (start < 0) start = 0;
    if (start >= (int)total_chars) return NULL;
    if (length <= 0) return NULL;
    if (start + length > (int)total_chars) {
        length = (int)total_chars - start;
    }

    // 找到起始位置的字节偏移
    int pos = 0;
    int char_count = 0;

    while (str[pos] != '\0' && char_count < start) {
        int bytes = get_char_bytes(&str[pos]);
        pos += bytes;
        char_count++;
    }

    // 计算结束位置的字节偏移
    int end_pos = pos;
    int copied_chars = 0;

    while (str[end_pos] != '\0' && copied_chars < length) {
        int bytes = get_char_bytes(&str[end_pos]);
        end_pos += bytes;
        copied_chars++;
    }

    // 分配内存并复制
    int sub_len = end_pos - pos;
    char* result = (char*)malloc(sub_len + 1);
    if (result) {
        strncpy(result, &str[pos], sub_len);
        result[sub_len] = '\0';
    }

    return result;
}

// 简单的繁简转换映射表(示例)
static const char* CHINESE_MAP[][2] = {
    {"愛", "爱"}, {"並", "并"}, {"車", "车"}, {"學", "学"},
    {"樂", "乐"}, {"門", "门"}, {"關", "关"}, {"開", "开"},
    {"體", "体"}, {"國", "国"}, {"樣", "样"}, {"現", "现"},
    {"經", "经"}, {"書", "书"}, {"買", "买"}, {"賣", "卖"},
    {"聽", "听"}, {"說", "说"}, {"話", "话"}, {"見", "见"},
    {"識", "识"}, {"記", "记"}, {"設", "设"}, {"計", "计"},
    {"機", "机"}, {"電", "电"}, {"腦", "脑"}, {"網", "网"},
    {"絡", "络"}, {"軟", "软"}, {"體", "体"}, {"製", "制"},
    {"造", "造"}, {"業", "业"}, {"務", "务"}, {"務", "务"},
};

const int CHINESE_MAP_SIZE = sizeof(CHINESE_MAP) / sizeof(CHINESE_MAP[0]);

// 繁简转换函数
char* traditional_to_simplified(const char* text) {
    if (!text) return NULL;

    int len = strlen(text);
    char* result = (char*)malloc(len + 1);
    if (!result) return NULL;

    int result_pos = 0;
    int pos = 0;

    while (pos < len) {
        int bytes = get_char_bytes(&text[pos]);
        int found = 0;

        // 检查是否需要转换
        for (int i = 0; i < CHINESE_MAP_SIZE; i++) {
            const char* trad = CHINESE_MAP[i][0];
            const char* simp = CHINESE_MAP[i][1];
            int trad_len = strlen(trad);

            if (pos + trad_len <= len && strncmp(&text[pos], trad, trad_len) == 0) {
                // 找到匹配，转换为简体
                strcpy(&result[result_pos], simp);
                result_pos += strlen(simp);
                pos += trad_len;
                found = 1;
                break;
            }
        }

        if (!found) {
            // 没有匹配，直接复制
            for (int i = 0; i < bytes && pos < len; i++) {
                result[result_pos++] = text[pos++];
            }
        }
    }

    result[result_pos] = '\0';
    return result;
}

// 简繁转换函数
char* simplified_to_traditional(const char* text) {
    if (!text) return NULL;

    int len = strlen(text);
    char* result = (char*)malloc(len + 1);
    if (!result) return NULL;

    int result_pos = 0;
    int pos = 0;

    while (pos < len) {
        int bytes = get_char_bytes(&text[pos]);
        int found = 0;

        // 检查是否需要转换
        for (int i = 0; i < CHINESE_MAP_SIZE; i++) {
            const char* trad = CHINESE_MAP[i][0];
            const char* simp = CHINESE_MAP[i][1];
            int simp_len = strlen(simp);

            if (pos + simp_len <= len && strncmp(&text[pos], simp, simp_len) == 0) {
                // 找到匹配，转换为繁体
                strcpy(&result[result_pos], trad);
                result_pos += strlen(trad);
                pos += simp_len;
                found = 1;
                break;
            }
        }

        if (!found) {
            // 没有匹配，直接复制
            for (int i = 0; i < bytes && pos < len; i++) {
                result[result_pos++] = text[pos++];
            }
        }
    }

    result[result_pos] = '\0';
    return result;
}
