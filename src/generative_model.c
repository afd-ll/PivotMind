#include "generative_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 初始化词汇表（使用 gen_ 前缀避免与 vocab 模块冲突）
GenVocabulary* gen_vocab_create(int max_size) {
    GenVocabulary* vocab = (GenVocabulary*)malloc(sizeof(GenVocabulary));
    vocab->words = (char**)malloc(sizeof(char*) * max_size);
    vocab->size = 0;
    vocab->max_size = max_size;

    // 添加特殊token
    gen_vocab_add_word(vocab, "<PAD>");  // 填充
    gen_vocab_add_word(vocab, "<SOS>");  // 句子开始
    gen_vocab_add_word(vocab, "<EOS>");  // 句子结束
    gen_vocab_add_word(vocab, "<UNK>");  // 未知词

    return vocab;
}

void gen_vocab_destroy(GenVocabulary* vocab) {
    if (vocab) {
        for (int i = 0; i < vocab->size; i++) {
            if (vocab->words[i]) {
                free(vocab->words[i]);
            }
        }
        free(vocab->words);
        free(vocab);
    }
}

int gen_vocab_add_word(GenVocabulary* vocab, const char* word) {
    if (vocab->size >= vocab->max_size) {
        int new_max_size = vocab->max_size * 2;
        char** new_words = (char**)realloc(vocab->words,
                                         sizeof(char*) * new_max_size);
        if (!new_words) {
            // realloc失败，返回错误
            return -1;
        }
        vocab->words = new_words;
        vocab->max_size = new_max_size;
    }

    // 检查词是否已存在
    for (int i = 0; i < vocab->size; i++) {
        if (strcmp(vocab->words[i], word) == 0) {
            return i;  // 返回已存在的ID
        }
    }

    vocab->words[vocab->size] = strdup(word);
    if (!vocab->words[vocab->size]) {
        return -1;  // strdup失败
    }
    return vocab->size++;
}

int gen_vocab_get_word_id(GenVocabulary* vocab, const char* word) {
    for (int i = 0; i < vocab->size; i++) {
        if (strcmp(vocab->words[i], word) == 0) {
            return i;
        }
    }
    return 3;  // <UNK>的ID
}

const char* gen_vocab_get_word(GenVocabulary* vocab, int id) {
    if (id >= 0 && id < vocab->size) {
        return vocab->words[id];
    }
    return "<UNK>";
}
