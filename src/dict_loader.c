/**
 * dict_loader.c — 词典加载与词性标注实现
 *
 * 哈希表: 开放寻址法, FNV-1a 哈希
 * 分词: 正向最大匹配 (FMM)
 */
#include "dict_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ===== 内部结构 ===== */

#define DICT_HASH_SIZE  524287   /* 质数, ~50万词表负载因子 ~0.67 */
#define DICT_KEY_MAX    64

typedef struct {
    char   key[DICT_KEY_MAX];
    int    freq;
    char   pos[8];
    int    used;   /* 0=空, 1=占用 */
} DictSlot;

struct DictTable {
    DictSlot* slots;
    int       size;
    int       count;
};

/* FNV-1a 哈希 */
static unsigned dict_hash(const char* s) {
    unsigned h = 2166136261u;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

/* ===== 公开接口 ===== */

DictTable* dict_table_create(int capacity) {
    DictTable* dt = (DictTable*)calloc(1, sizeof(DictTable));
    if (!dt) return NULL;
    dt->size = (capacity > 256) ? capacity : DICT_HASH_SIZE;
    dt->slots = (DictSlot*)calloc(dt->size, sizeof(DictSlot));
    if (!dt->slots) { free(dt); return NULL; }
    return dt;
}

void dict_table_destroy(DictTable* dt) {
    if (!dt) return;
    free(dt->slots);
    free(dt);
}

int dict_table_size(DictTable* dt) {
    return dt ? dt->count : 0;
}

int dict_table_insert(DictTable* dt, const char* word, int freq, const char* pos) {
    if (!dt || !word) return -1;
    unsigned h = dict_hash(word);
    for (int probe = 0; probe < dt->size; probe++) {
        unsigned idx = (h + probe) % dt->size;
        if (!dt->slots[idx].used) {
            strncpy(dt->slots[idx].key, word, DICT_KEY_MAX - 1);
            dt->slots[idx].key[DICT_KEY_MAX - 1] = '\0';
            dt->slots[idx].freq = freq;
            strncpy(dt->slots[idx].pos, pos ? pos : "x", 7);
            dt->slots[idx].pos[7] = '\0';
            dt->slots[idx].used = 1;
            dt->count++;
            return 0;
        }
        if (strcmp(dt->slots[idx].key, word) == 0) {
            /* 已存在，更新 */
            dt->slots[idx].freq = freq;
            if (pos) { strncpy(dt->slots[idx].pos, pos, 7); dt->slots[idx].pos[7] = '\0'; }
            return 0;
        }
    }
    return -1; /* 表满 */
}

const DictEntry* dict_table_lookup(DictTable* dt, const char* word) {
    if (!dt || !word) return NULL;
    unsigned h = dict_hash(word);
    for (int probe = 0; probe < dt->size; probe++) {
        unsigned idx = (h + probe) % dt->size;
        if (!dt->slots[idx].used) return NULL;
        if (strcmp(dt->slots[idx].key, word) == 0) {
            /* 返回临时指针（调用方需立即使用或复制） */
            static DictEntry entry;
            entry.word = dt->slots[idx].key;
            entry.freq = dt->slots[idx].freq;
            strncpy(entry.pos, dt->slots[idx].pos, 7);
            entry.pos[7] = '\0';
            return &entry;
        }
    }
    return NULL;
}

int dict_load_jieba(DictTable* dt, const char* file_path) {
    if (!dt || !file_path) return -1;

    FILE* fp = fopen(file_path, "r");
    if (!fp) {
        fprintf(stderr, "[词典] 无法打开: %s\n", file_path);
        return -1;
    }

    int loaded = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        /* 去除尾部换行 */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;

        char word[DICT_KEY_MAX];
        char pos[8] = "x";
        int freq = 1;

        /* 解析: word freq pos（空格分隔） */
        int n = sscanf(line, "%63s %d %7s", word, &freq, pos);
        if (n >= 1) {
            if (dict_table_insert(dt, word, freq, (n >= 3) ? pos : "x") == 0)
                loaded++;
        }
    }
    fclose(fp);
    printf("[词典] 从 %s 加载 %d 条词\n", file_path, loaded);
    return loaded;
}

/* ===== 正向最大匹配分词 ===== */

int dict_segment_text(DictTable* dt, const char* text,
                      char out_words[][64], char out_pos[][8],
                      int out_cap) {
    if (!dt || !text || !out_words || out_cap <= 0) return 0;

    int count = 0;
    const char* p = text;
    const int max_word_len = 12;  /* 中文最长词约6-8个汉字 = 18-24字节 */

    while (*p && count < out_cap) {
        /* 跳过分隔符 */
        if (isspace((unsigned char)*p)) { p++; continue; }

        /* 查找最长匹配 */
        int best_len = 0;
        const char* best_pos = "x";

        const char* end = p;
        int byte_count = 0;
        while (*end && byte_count < max_word_len * 3) {
            end++;
            byte_count++;
        }

        /* 从最长开始倒序匹配 */
        for (const char* try_end = end; try_end > p; ) {
            /* 回退一个 UTF-8 字符 */
            const char* prev = try_end - 1;
            while (prev > p && (unsigned char)*prev >= 0x80 && (unsigned char)*prev < 0xC0)
                prev--;
            try_end = prev;

            int word_len = (int)(try_end - p);
            if (word_len <= 0) continue;

            char buf[64];
            if (word_len >= (int)sizeof(buf)) continue;
            memcpy(buf, p, word_len);
            buf[word_len] = '\0';

            const DictEntry* e = dict_table_lookup(dt, buf);
            if (e) {
                best_len = word_len;
                best_pos = e->pos;
                break;
            }
        }

        if (best_len > 0) {
            /* 找到词典词 */
            if (best_len >= 63) best_len = 63;
            memcpy(out_words[count], p, best_len);
            out_words[count][best_len] = '\0';
            strncpy(out_pos[count], best_pos, 7);
            out_pos[count][7] = '\0';
            p += best_len;
        } else {
            /* 未收录的单字 */
            int clen = 1;
            if ((unsigned char)*p >= 0x80) {
                /* UTF-8 多字节字符 */
                if ((unsigned char)*p >= 0xE0) clen = 3;
                else clen = 2;
            }
            if (clen >= 63) clen = 63;
            memcpy(out_words[count], p, clen);
            out_words[count][clen] = '\0';
            strncpy(out_pos[count], "x", 7);
            out_pos[count][7] = '\0';
            p += clen;
        }
        count++;
    }
    return count;
}

/* ===== 词性映射 ===== */

const char* pos_to_syntax_node(const char* pos) {
    if (!pos || !*pos) return NULL;

    /* jieba 词性标签 → 内部语法节点名 */
    switch (pos[0]) {
        case 'a': return "ADJ";      /* 形容词 */
        case 'b': return "NOUN";     /* 区别词 → 名词大类 */
        case 'c': return "CONJ";     /* 连词 */
        case 'd': return "ADV";      /* 副词 */
        case 'e': return "INTJ";     /* 叹词 */
        case 'f': return "NOUN";     /* 方位词 → 名词 */
        case 'g': return "NOUN";     /* 语素 → 名词 */
        case 'h': return "PREP";     /* 前缀 */
        case 'i': return "INTJ";     /* 成语/习语 */
        case 'j': return "NOUN";     /* 简称 */
        case 'k': return "PREP";     /* 后缀 */
        case 'l': return "INTJ";     /* 习语 */
        case 'm': return "NUM";      /* 数词 */
        case 'n':
            if (pos[1] == 'r') return "NOUN"; /* 人名 */
            if (pos[1] == 's') return "NOUN"; /* 地名 */
            if (pos[1] == 't') return "NOUN"; /* 机构 */
            if (pos[1] == 'z') return "NOUN"; /* 其他专名 */
            return "NOUN";           /* 名词 */
        case 'o': return "INTJ";     /* 拟声词 */
        case 'p': return "PREP";     /* 介词 */
        case 'q': return "NUM";      /* 量词 */
        case 'r': return "PRON";     /* 代词 */
        case 's': return "NOUN";     /* 处所词 */
        case 't': return "NOUN";     /* 时间词 */
        case 'u': return "PART";     /* 助词 */
        case 'v':
            if (pos[1] == 'n') return "VERB"; /* 名动词 */
            if (pos[1] == 'd') return "ADV";  /* 副动词 */
            return "VERB";           /* 动词 */
        case 'w': return "PART";     /* 标点/符号 */
        case 'x': return NULL;       /* 非语素字（无对应） */
        case 'y': return "PART";     /* 语气词 */
        case 'z': return "NOUN";     /* 状态词 */
        default:  return NULL;
    }
}
