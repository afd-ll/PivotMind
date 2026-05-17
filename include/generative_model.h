#ifndef GENERATIVE_MODEL_H
#define GENERATIVE_MODEL_H

#include "tensor.h"
#include "model.h"
#include <stdbool.h>

// 词汇表结构（生成模型专用）
typedef struct {
    char** words;       // 词列表
    int size;          // 词汇表大小
    int max_size;      // 最大容量
} GenVocabulary;

// 训练样本结构
typedef struct {
    int* input_ids;
    int input_len;
    int* target_ids;
    int target_len;
} TrainingSample;

// 初始化词汇表（使用 gen_ 前缀避免与 vocab 模块冲突）
GenVocabulary* gen_vocab_create(int max_size);
void gen_vocab_destroy(GenVocabulary* vocab);
int gen_vocab_add_word(GenVocabulary* vocab, const char* word);
int gen_vocab_get_word_id(GenVocabulary* vocab, const char* word);
const char* gen_vocab_get_word(GenVocabulary* vocab, int id);

#endif
