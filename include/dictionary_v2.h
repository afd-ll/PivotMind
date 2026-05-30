#ifndef DICTIONARY_V2_H
#define DICTIONARY_V2_H

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 词典条目结构体
typedef struct {
    int id;                    // 条目ID
    char* traditional;         // 繁体字
    char* simplified;          // 简体字
    char* pinyin;              // 拼音
    char* definition;          // 英文释义
    char* category;            // 词性分类
    int frequency;             // 词频权重
} DictionaryEntry;

// 词典数据库句柄
typedef struct {
    sqlite3* db;               // SQLite数据库连接
    sqlite3_stmt* search_stmt; // 精确查询预处理语句
    sqlite3_stmt* fuzzy_stmt;  // 模糊查询预处理语句
    sqlite3_stmt* pinyin_stmt; // 拼音查询预处理语句
} DictionaryDB;

// ==========================================
// 核心API函数
// ==========================================

/**
 * @brief 初始化词典数据库
 * @param db_path 数据库文件路径
 * @return 成功返回DictionaryDB指针，失败返回NULL
 * @note 使用完毕后必须调用dict_close()释放资源
 */
DictionaryDB* dict_init(const char* db_path);

/**
 * @brief 关闭词典数据库
 * @param dict 词典数据库句柄
 * @note 安全函数，可传入NULL指针
 */
void dict_close(DictionaryDB* dict);

/**
 * @brief 精确查询词语
 * @param dict 词典数据库句柄
 * @param word 要查询的词语（支持繁体或简体）
 * @param count 输出参数，返回结果数量
 * @return 成功返回DictionaryEntry数组，失败返回NULL
 * @note 返回的数组必须使用dict_free_results()释放
 * @note 最多返回10个结果
 */
DictionaryEntry* dict_search(DictionaryDB* dict, const char* word, int* count);

/**
 * @brief 模糊查询（前缀匹配）
 * @param dict 词典数据库句柄
 * @param prefix 查询前缀
 * @param count 输出参数，返回结果数量
 * @return 成功返回DictionaryEntry数组，失败返回NULL
 * @note 返回的数组必须使用dict_free_results()释放
 * @note 最多返回20个结果
 */
DictionaryEntry* dict_search_prefix(DictionaryDB* dict, const char* prefix, int* count);

/**
 * @brief 拼音查询
 * @param dict 词典数据库句柄
 * @param pinyin 拼音前缀
 * @param count 输出参数，返回结果数量
 * @return 成功返回DictionaryEntry数组，失败返回NULL
 * @note 返回的数组必须使用dict_free_results()释放
 * @note 最多返回20个结果
 */
DictionaryEntry* dict_search_pinyin(DictionaryDB* dict, const char* pinyin, int* count);

/**
 * @brief 释放查询结果
 * @param entries 要释放的DictionaryEntry数组
 * @param count 数组中元素的数量
 * @note 安全函数，可传入NULL指针
 */
void dict_free_results(DictionaryEntry* entries, int count);

// ==========================================
// 工具函数
// ==========================================

/**
 * @brief 打印单个词典条目（用于调试）
 * @param entry 要打印的词典条目
 */
void dict_print_entry(const DictionaryEntry* entry);

/**
 * @brief 获取词典统计信息
 * @param dict 词典数据库句柄
 * @param total_entries 输出参数，返回总条目数
 * @param unique_words 输出参数，返回唯一词数
 * @return 成功返回0，失败返回-1
 */
int dict_get_stats(DictionaryDB* dict, int* total_entries, int* unique_words);

#endif // DICTIONARY_V2_H