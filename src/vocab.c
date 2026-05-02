#include "../include/vocab.h"
#include "../include/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ========== 内部辅助 ==========

static int _vocab_find(Vocab* vocab, const char* word) {
    for (int i = 0; i < vocab->size; i++) {
        if (strcmp(vocab->entries[i].word, word) == 0) {
            return i;
        }
    }
    return -1;
}

static void _vocab_grow(Vocab* vocab) {
    if (vocab->size < vocab->capacity) return;
    vocab->capacity *= 2;
    vocab->entries = (VocabEntry*)realloc(vocab->entries,
                                           vocab->capacity * sizeof(VocabEntry));
}

// ========== 词表API ==========

Vocab* vocab_create(int initial_capacity) {
    Vocab* vocab = (Vocab*)malloc(sizeof(Vocab));
    if (!vocab) return NULL;

    vocab->capacity = (initial_capacity > 0) ? initial_capacity : 4096;
    vocab->entries = (VocabEntry*)calloc(vocab->capacity, sizeof(VocabEntry));
    vocab->size = 0;
    vocab->max_freq = 0;

    // 注册特殊Token
    vocab_add(vocab, "<UNK>");  // ID 0
    vocab_add(vocab, "<PAD>"); // ID 1
    vocab_add(vocab, "<BOS>"); // ID 2
    vocab_add(vocab, "<EOS>"); // ID 3
    vocab_add(vocab, "<SEP>"); // ID 4

    return vocab;
}

void vocab_destroy(Vocab* vocab) {
    if (!vocab) return;
    for (int i = 0; i < vocab->size; i++) {
        free(vocab->entries[i].word);
    }
    free(vocab->entries);
    free(vocab);
}

int vocab_add(Vocab* vocab, const char* word) {
    int idx = _vocab_find(vocab, word);
    if (idx >= 0) {
        vocab->entries[idx].freq++;
        if (vocab->entries[idx].freq > vocab->max_freq) {
            vocab->max_freq = vocab->entries[idx].freq;
        }
        return vocab->entries[idx].id;
    }

    _vocab_grow(vocab);

    VocabEntry* e = &vocab->entries[vocab->size];
    e->word = strdup(word);
    e->id = vocab->size;
    e->freq = 1;

    if (e->freq > vocab->max_freq) vocab->max_freq = e->freq;

    vocab->size++;
    return e->id;
}

void vocab_inc_freq(Vocab* vocab, const char* word) {
    int idx = _vocab_find(vocab, word);
    if (idx >= 0) {
        vocab->entries[idx].freq++;
        if (vocab->entries[idx].freq > vocab->max_freq) {
            vocab->max_freq = vocab->entries[idx].freq;
        }
    }
}

int vocab_lookup(Vocab* vocab, const char* word) {
    int idx = _vocab_find(vocab, word);
    return (idx >= 0) ? vocab->entries[idx].id : VOCAB_UNK_ID;
}

int vocab_encode(Vocab* vocab, const char* text, int* output_ids, int max_len) {
    if (!vocab || !text || !output_ids || max_len <= 0) return 0;

    char* buf = strdup(text);
    int len = 0;
    char* saveptr;
    char* token = strtok_r(buf, " \t\r\n", &saveptr);

    while (token && len < max_len) {
        // 去除token两端的空白
        while (*token == ' ' || *token == '\t') token++;
        char* end = token + strlen(token) - 1;
        while (end > token && (*end == ' ' || *end == '\t')) *end-- = '\0';

        if (*token != '\0') {
            output_ids[len++] = vocab_lookup(vocab, token);
        }
        token = strtok_r(NULL, " \t\r\n", &saveptr);
    }

    free(buf);
    return len;
}

int vocab_decode(Vocab* vocab, const int* ids, int len, char* output, int max_len) {
    if (!vocab || !ids || !output || max_len <= 0) return 0;

    int pos = 0;
    for (int i = 0; i < len && pos < max_len - 1; i++) {
        int id = ids[i];
        const char* word = NULL;

        if (id >= 0 && id < vocab->size) {
            word = vocab->entries[id].word;
        } else {
            word = "<UNK>";
        }

        // 跳过特殊token
        if (word[0] == '<' && word[strlen(word)-1] == '>') {
            if (strcmp(word, "<BOS>") == 0 || strcmp(word, "<EOS>") == 0 ||
                strcmp(word, "<PAD>") == 0 || strcmp(word, "<SEP>") == 0) {
                continue;
            }
        }

        int wlen = strlen(word);
        if (pos + wlen + 1 >= max_len) break;

        if (pos > 0 && word[0] != '<') {
            output[pos++] = ' ';
        }
        strcpy(output + pos, word);
        pos += wlen;
    }

    output[pos] = '\0';
    return pos;
}

int _vocab_entry_cmp(const void* a, const void* b) {
    return ((VocabEntry*)b)->freq - ((VocabEntry*)a)->freq;
}

void vocab_sort_by_freq(Vocab* vocab) {
    // 按词频降序排序，保留前5个特殊token位置不变
    // 实际上排序后ID会变，需要重建映射
    qsort(vocab->entries + 5, vocab->size - 5, sizeof(VocabEntry), _vocab_entry_cmp);
    // 重建ID
    for (int i = 0; i < vocab->size; i++) {
        vocab->entries[i].id = i;
    }
}

void vocab_trim(Vocab* vocab, int min_freq) {
    int write = 5; // 保留特殊token
    for (int i = 5; i < vocab->size; i++) {
        if (vocab->entries[i].freq >= min_freq) {
            if (write != i) {
                vocab->entries[write] = vocab->entries[i];
                vocab->entries[write].id = write;
            }
            write++;
        } else {
            free(vocab->entries[i].word);
        }
    }
    vocab->size = write;
}

int vocab_save(Vocab* vocab, const char* filepath) {
    FILE* fp = fopen(filepath, "wb");
    if (!fp) return -1;

    // 写入头：size, max_freq
    fwrite(&vocab->size, sizeof(int), 1, fp);
    fwrite(&vocab->max_freq, sizeof(int), 1, fp);

    // 写入每个词条
    for (int i = 0; i < vocab->size; i++) {
        VocabEntry* e = &vocab->entries[i];
        int word_len = strlen(e->word) + 1;
        fwrite(&word_len, sizeof(int), 1, fp);
        fwrite(e->word, 1, word_len, fp);
        fwrite(&e->id, sizeof(int), 1, fp);
        fwrite(&e->freq, sizeof(int), 1, fp);
    }

    fclose(fp);
    return 0;
}

Vocab* vocab_load(const char* filepath) {
    FILE* fp = fopen(filepath, "rb");
    if (!fp) return NULL;

    Vocab* vocab = (Vocab*)malloc(sizeof(Vocab));
    if (!vocab) { fclose(fp); return NULL; }

    fread(&vocab->size, sizeof(int), 1, fp);
    fread(&vocab->max_freq, sizeof(int), 1, fp);
    vocab->capacity = vocab->size + 256;
    vocab->entries = (VocabEntry*)malloc(vocab->capacity * sizeof(VocabEntry));

    for (int i = 0; i < vocab->size; i++) {
        int word_len;
        fread(&word_len, sizeof(int), 1, fp);
        vocab->entries[i].word = (char*)malloc(word_len);
        fread(vocab->entries[i].word, 1, word_len, fp);
        fread(&vocab->entries[i].id, sizeof(int), 1, fp);
        fread(&vocab->entries[i].freq, sizeof(int), 1, fp);
    }

    fclose(fp);
    return vocab;
}

int vocab_size(Vocab* vocab) {
    return vocab ? vocab->size : 0;
}

char* vocab_info(Vocab* vocab) {
    static char buf[256];
    if (!vocab) { buf[0] = '\0'; return buf; }
    snprintf(buf, sizeof(buf), "vocab_size=%d, max_freq=%d", vocab->size, vocab->max_freq);
    return buf;
}

// ========== 词表构建器 ==========

static int _read_file_lines(const char* filepath, char*** lines_out) {
    FILE* fp = fopen(filepath, "r");
    if (!fp) return -1;

    int capacity = 4096;
    int count = 0;
    char** lines = (char**)malloc(capacity * sizeof(char*));
    char line[8192];

    while (fgets(line, sizeof(line), fp)) {
        // 去除换行
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }
        // 跳过注释和空行
        if (len == 0 || line[0] == '#') continue;

        if (count >= capacity) {
            capacity *= 2;
            lines = (char**)realloc(lines, capacity * sizeof(char*));
        }
        lines[count++] = strdup(line);
    }

    fclose(fp);
    *lines_out = lines;
    return count;
}

static void _free_lines(char** lines, int count) {
    for (int i = 0; i < count; i++) free(lines[i]);
    free(lines);
}

int vocab_build_from_file(Vocab* vocab, const char* filepath, int min_freq) {
    char** lines;
    int line_count = _read_file_lines(filepath, &lines);
    if (line_count < 0) return -1;

    int added = 0;

    for (int i = 0; i < line_count; i++) {
        char* line = lines[i];

        // 预处理：去除行首行尾空白
        while (*line == ' ' || *line == '\t') line++;
        int len = strlen(line);
        while (len > 0 && (line[len-1] == ' ' || line[len-1] == '\t')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        // 提取对话数据（格式: 问题|答案 或 问题|）
        // 也支持纯文本，直接按空格分词
        char* pipe = strchr(line, '|');
        if (pipe) {
            // 处理问句部分
            *pipe = '\0';
            char* question = line;
            char* answer = pipe + 1;

            // 去除答案开头的空白
            while (*answer == ' ' || *answer == '\t') answer++;

            // 分词并统计词频
            char qbuf[4096] = {0};
            char abuf[4096] = {0};
            int qpos = 0, apos = 0;

            // 问句：按空格分词
            char* saveptr;
            char* token = strtok_r(question, " \t", &saveptr);
            while (token) {
                if (qpos > 0) qbuf[qpos++] = ' ';
                strcpy(qbuf + qpos, token);
                qpos += strlen(token);
                vocab_inc_freq(vocab, token);
                added++;
                token = strtok_r(NULL, " \t", &saveptr);
            }

            // 答句：按空格分词
            token = strtok_r(answer, " \t", &saveptr);
            while (token) {
                if (apos > 0) abuf[apos++] = ' ';
                strcpy(abuf + apos, token);
                apos += strlen(token);
                vocab_inc_freq(vocab, token);
                added++;
                token = strtok_r(NULL, " \t", &saveptr);
            }
        } else {
            // 纯文本：逐字分词（中文单字作为基本单元）
            // 同时统计相邻双字的词频（bigram）

            // 按空格分词
            char* saveptr;
            char* token = strtok_r(line, " \t", &saveptr);
            while (token) {
                vocab_inc_freq(vocab, token);
                added++;
                token = strtok_r(NULL, " \t", &saveptr);
            }
        }
    }

    _free_lines(lines, line_count);

    // 裁剪低频词
    if (min_freq > 1) {
        vocab_trim(vocab, min_freq);
    }

    return added;
}

int vocab_build_from_files(Vocab* vocab, const char** filepaths, int count, int min_freq) {
    int total = 0;
    for (int i = 0; i < count; i++) {
        int n = vocab_build_from_file(vocab, filepaths[i], 1); // 先全部加入
        if (n >= 0) total += n;
    }
    if (min_freq > 1) vocab_trim(vocab, min_freq);
    return total;
}