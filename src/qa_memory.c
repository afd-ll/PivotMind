/* qa_memory.c — 轻量 QA 记忆检索实现
 *
 * 策略：
 *   1. 加载时将 Q 文本分词存入 token 集合
 *   2. 查询时将 input 分词后对每条 Q 计算交集分数
 *   3. 返回最高分的 A，最低阈值防止噪音匹配
 *   4. 支持运行时添加 QA 对
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "qa_memory.h"
#include "utf8_tokenizer.h"

#define MAX_QA_LINE  512
#define MAX_TOKENS   64

typedef struct {
    char*  question;
    char*  answer;
    char** tokens;
    int    token_count;
} QAEntry;

struct QAMemory {
    QAEntry* entries;
    int      count;
    int      capacity;
};

QAMemory* qa_memory_create(const char* pipe_path, int max_entries) {
    if (max_entries <= 0) return NULL;

    QAMemory* m = (QAMemory*)calloc(1, sizeof(QAMemory));
    if (!m) return NULL;

    m->capacity = max_entries;
    m->entries  = (QAEntry*)calloc(max_entries, sizeof(QAEntry));
    if (!m->entries) { free(m); return NULL; }

    /* NULL pipe_path: 空初始化，运行时通过 qa_memory_add 填充 */
    if (!pipe_path) {
        fprintf(stderr, "[QA记忆] 空初始化 (容量 %d)\n", max_entries);
        return m;
    }

    FILE* f = fopen(pipe_path, "r");
    if (!f) {
        fprintf(stderr, "[QA记忆] 无法打开 %s\n", pipe_path);
        free(m->entries);
        free(m);
        return NULL;
    }

    char line[MAX_QA_LINE];
    while (fgets(line, sizeof(line), f) && m->count < max_entries) {
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
        if (!len) continue;

        char* pipe = strchr(line, '|');
        if (!pipe) continue;
        *pipe = 0;

        char* q = line;
        char* a = pipe + 1;

        char* tokens[MAX_TOKENS];
        int tc = utf8_tokenize(q, tokens, MAX_TOKENS);

        if (tc > 0) {
            QAEntry* e = &m->entries[m->count];
            e->question = strdup(q);
            e->answer   = strdup(a);
            e->tokens   = (char**)malloc(tc * sizeof(char*));
            e->token_count = tc;
            for (int i = 0; i < tc; i++) {
                e->tokens[i] = strdup(tokens[i]);
                free(tokens[i]);
            }
            m->count++;
        } else {
            for (int i = 0; i < tc; i++) free(tokens[i]);
        }
    }
    fclose(f);
    fprintf(stderr, "[QA记忆] 加载 %d 对 (上限 %d)\n", m->count, max_entries);
    return m;
}

const char* qa_memory_query(QAMemory* m, const char* input) {
    if (!m || !input || !input[0]) return NULL;

    char* tokens[MAX_TOKENS];
    int tc = utf8_tokenize(input, tokens, MAX_TOKENS);
    if (tc <= 0) return NULL;

    char* utokens[MAX_TOKENS];
    int utc = 0;
    for (int i = 0; i < tc; i++) {
        if (!tokens[i]) continue;
        int dup = 0;
        for (int j = 0; j < utc; j++) {
            if (strcmp(tokens[i], utokens[j]) == 0) { dup = 1; break; }
        }
        if (!dup) utokens[utc++] = tokens[i];
    }

    int best_idx = -1;
    float best_score = 0.0f;  /* 最低阈值：消除单字重叠噪音 */
    const char* best_answer = NULL;

    for (int i = 0; i < m->count; i++) {
        QAEntry* e = &m->entries[i];
        int match = 0;
        for (int j = 0; j < utc; j++) {
            for (int k = 0; k < e->token_count; k++) {
                if (strcmp(utokens[j], e->tokens[k]) == 0) {
                    match++;
                    break;
                }
            }
        }
        float score = (float)match / sqrtf((float)e->token_count + 1);
        if (match > 0 && score > best_score) {
            best_score = score;
            best_idx = i;
            best_answer = e->answer;
        }
    }

    for (int i = 0; i < tc; i++) free(tokens[i]);

    if (best_idx >= 0) {
        fprintf(stderr, "[QA记忆] 命中 #%d score=%.3f Q=\"%s\" A=\"%s\"\n",
                best_idx, (double)best_score,
                m->entries[best_idx].question,
                best_answer ? best_answer : "(null)");
        return best_answer;
    }
    return NULL;
}

void qa_memory_destroy(QAMemory* m) {
    if (!m) return;
    for (int i = 0; i < m->count; i++) {
        free(m->entries[i].question);
        free(m->entries[i].answer);
        for (int j = 0; j < m->entries[i].token_count; j++)
            free(m->entries[i].tokens[j]);
        free(m->entries[i].tokens);
    }
    free(m->entries);
    free(m);
}

int qa_memory_count(QAMemory* m) {
    return m ? m->count : 0;
}

int qa_memory_add(QAMemory* m, const char* question, const char* answer) {
    if (!m || !question || !answer || m->count >= m->capacity) return -1;

    char* tokens[MAX_TOKENS];
    int tc = utf8_tokenize(question, tokens, MAX_TOKENS);
    if (tc <= 0) return -1;

    QAEntry* e = &m->entries[m->count];
    e->question = strdup(question);
    e->answer   = strdup(answer);
    e->tokens   = (char**)malloc(tc * sizeof(char*));
    e->token_count = tc;
    for (int i = 0; i < tc; i++) {
        e->tokens[i] = strdup(tokens[i]);
        free(tokens[i]);
    }
    m->count++;
    return 0;
}
