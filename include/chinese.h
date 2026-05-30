#ifndef CHINESE_H
#define CHINESE_H

#include <stddef.h>

// 中文字符类型
typedef enum {
    CH_ASCII,           // ASCII字符
    CH_UTF8_1BYTE,      // 1字节UTF-8 (ASCII)
    CH_UTF8_2BYTE,      // 2字节UTF-8
    CH_UTF8_3BYTE,      // 3字节UTF-8 (大部分中文字符)
    CH_UTF8_4BYTE,      // 4字节UTF-8
    CH_INVALID          // 无效编码
} CharType;

// 词token结构
typedef struct {
    char* text;         // 词的文本
    int start_pos;      // 起始位置
    int length;         // 长度(字节数)
    int char_count;     // 字符数
    int is_chinese;     // 是否为中文
} Token;

// 分词器
typedef struct {
    int max_tokens;     // 最大token数量
    Token* tokens;      // token数组
    int token_count;    // 当前token数量
} Tokenizer;

// 初始化分词器
Tokenizer* tokenizer_create(int max_tokens);

// 销毁分词器
void tokenizer_destroy(Tokenizer* tokenizer);

// 获取字符类型
CharType get_char_type(const char* str);

// 获取字符长度(字节数)
int get_char_bytes(const char* str);

// 判断是否为中文字符
int is_chinese_char(const char* str);

// 判断是否为标点符号
int is_punctuation(const char* str);

// 分词 - 将字符串分割为token数组
int tokenize(Tokenizer* tokenizer, const char* text);

// 清空token数组
void tokenizer_clear(Tokenizer* tokenizer);

// 获取字符数(不是字节数)
size_t utf8_strlen(const char* str);

// UTF-8 字符串比较
int utf8_strcmp(const char* s1, const char* s2);

// 设置控制台编码为UTF-8(Windows)
void set_console_utf8();

// 打印UTF-8字符串
void print_utf8(const char* str);

// 中文字符串查找
const char* utf8_strstr(const char* haystack, const char* needle);

// 获取子字符串
char* utf8_substr(const char* str, int start, int length);

// 繁简转换映射表大小
extern const int CHINESE_MAP_SIZE;

// 繁简转换函数
char* traditional_to_simplified(const char* text);
char* simplified_to_traditional(const char* text);

#endif // CHINESE_H
