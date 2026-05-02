/**
 * @file utf8_tokenizer.h
 * @brief UTF-8中文分词工具
 */

#ifndef UTF8_TOKENIZER_H
#define UTF8_TOKENIZER_H

// UTF-8字符长度判断
int utf8_char_len(unsigned char c);

// 检查是否是中文字符
int is_chinese(const char* p);

// 提取单个UTF-8字符
char* utf8_get_char(const char* p);

// UTF-8中文分词
int utf8_tokenize(const char* text, char** tokens, int max_tokens);

// 打印tokens（调试用）
void print_tokens(char** tokens, int count);

#endif // UTF8_TOKENIZER_H
