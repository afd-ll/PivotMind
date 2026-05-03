#ifndef VOCAB_H
#define VOCAB_H

#include <stddef.h>

// ========== 词表结构 ==========

// 词条
typedef struct {
    char* word;        // 词文本
    int id;            // 词ID
    int freq;          // 词频
} VocabEntry;

// 词表
typedef struct {
    VocabEntry* entries;   // 词条数组
    int size;              // 词表大小
    int capacity;          // 词表容量
    int max_freq;          // 最大词频
} Vocab;

// 特殊Token ID
#define VOCAB_UNK_ID     0   // 未知词
#define VOCAB_PAD_ID     1   // 填充
#define VOCAB_BOS_ID     2   // 句首
#define VOCAB_EOS_ID     3   // 句尾
#define VOCAB_SEP_ID     4   // 分隔符

// ========== 词表API ==========

/**
 * 创建词表
 * @param initial_capacity 初始容量
 * @return 词表指针
 */
Vocab* vocab_create(int initial_capacity);

/**
 * 销毁词表
 */
void vocab_destroy(Vocab* vocab);

/**
 * 添加词到词表
 * @return 词ID
 */
int vocab_add(Vocab* vocab, const char* word);

/**
 * 增加词频
 */
void vocab_inc_freq(Vocab* vocab, const char* word);

/**
 * 查找词ID，不存在返回UNK_ID
 */
int vocab_lookup(Vocab* vocab, const char* word);

/**
 * 序列编码：将文本转为ID序列
 * @param vocab 词表
 * @param text 输入文本（空格分隔的词）
 * @param output_ids 输出ID数组
 * @param max_len 最大长度
 * @return 实际编码长度
 */
int vocab_encode(Vocab* vocab, const char* text, int* output_ids, int max_len);

/**
 * 序列解码：将ID序列转为文本
 * @param vocab 词表
 * @param ids ID数组
 * @param len ID数量
 * @param output 输出缓冲区
 * @param max_len 输出缓冲区大小
 * @return 输出字符串长度
 */
int vocab_decode(Vocab* vocab, const int* ids, int len, char* output, int max_len);

/**
 * 按词频排序词表（从高到低）
 */
void vocab_sort_by_freq(Vocab* vocab);

/**
 * 裁剪低频词
 * @param min_freq 最小词频阈值
 */
void vocab_trim(Vocab* vocab, int min_freq);

/**
 * 保存词表到文件
 */
int vocab_save(Vocab* vocab, const char* filepath);

/**
 * 从文件加载词表
 */
Vocab* vocab_load(const char* filepath);

/**
 * 获取词表大小
 */
int vocab_size(Vocab* vocab);

/**
 * 获取词表信息字符串
 */
char* vocab_info(Vocab* vocab);

// ========== 词表构建器 ==========

/**
 * 从文本文件构建词表
 * @param vocab 输出词表
 * @param filepath 文件路径
 * @param min_freq 最小词频（低于此不加入词表）
 * @return 添加的词数
 */
int vocab_build_from_file(Vocab* vocab, const char* filepath, int min_freq);

/**
 * 从多个文件构建词表
 * @param vocab 输出词表
 * @param filepaths 文件路径数组
 * @param count 文件数量
 * @param min_freq 最小词频
 * @return 添加的词数
 */
int vocab_build_from_files(Vocab* vocab, const char** filepaths, int count, int min_freq);

#endif // VOCAB_H