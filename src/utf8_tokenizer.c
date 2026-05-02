/**
 * @file utf8_tokenizer.c
 * @brief UTF-8中文分词工具
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// UTF-8字符长度判断
int utf8_char_len(unsigned char c) {
    if ((c & 0x80) == 0) return 1;        // ASCII
    if ((c & 0xE0) == 0xC0) return 2;     // 2字节
    if ((c & 0xF0) == 0xE0) return 3;     // 3字节（中文）
    if ((c & 0xF8) == 0xF0) return 4;     // 4字节
    return 1;
}

// 检查是否是中文字符
int is_chinese(const char* p) {
    unsigned char c = (unsigned char)*p;
    return (c & 0x80) != 0;  // 高位为1，可能是中文
}

// 提取单个UTF-8字符
char* utf8_get_char(const char* p) {
    int len = utf8_char_len((unsigned char)*p);
    char* result = (char*)malloc(len + 1);
    if (result) {
        strncpy(result, p, len);
        result[len] = '\0';
    }
    return result;
}

// UTF-8中文分词
int utf8_tokenize(const char* text, char** tokens, int max_tokens) {
    const char* p = text;
    int count = 0;
    
    while (*p && count < max_tokens) {
        // 跳过空白
        while (*p && isspace(*p)) p++;
        if (!*p) break;
        
        unsigned char c = (unsigned char)*p;
        int len = utf8_char_len(c);
        
        // 中文：单个字符作为一个token
        if ((c & 0x80) != 0) {
            tokens[count] = utf8_get_char(p);
            if (tokens[count]) count++;
            p += len;
        }
        // ASCII：连续字符作为一个token
        else {
            const char* start = p;
            while (*p && !isspace(*p) && !is_chinese(p)) {
                p++;
            }
            int token_len = p - start;
            tokens[count] = (char*)malloc(token_len + 1);
            if (tokens[count]) {
                strncpy(tokens[count], start, token_len);
                tokens[count][token_len] = '\0';
                count++;
            }
        }
    }
    
    return count;
}

// 打印tokens（调试用）
void print_tokens(char** tokens, int count) {
    printf("分词结果: %d 个token\n", count);
    for (int i = 0; i < count; i++) {
        printf("  [%d] %s\n", i, tokens[i]);
    }
}

// 测试
#ifdef TEST_UTF8
int main() {
    const char* test = "清晨的火车站，一个男人坐在长椅上";
    char* tokens[100];
    int count = utf8_tokenize(test, tokens, 100);
    print_tokens(tokens, count);
    
    for (int i = 0; i < count; i++) {
        free(tokens[i]);
    }
    
    return 0;
}
#endif
