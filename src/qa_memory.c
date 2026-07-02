/* qa_memory.c — 轻量 QA 记忆检索实现
 *
 * 策略：
 *   1. 加载时将 Q 文本分词存入 token 集合
 *   2. 查询时将 input 分词后对每条 Q 计算交集分数
 *   3. 返回最高分的 A，最低阈值防止噪音匹配
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qa_memory.h"
#include "utf8_tokenizer.h"

#define MAX_QA_LINE  512
#define MAX_TOKENS   64

typedef struct {
    char*  question;
    char*  answer;
    char** tokens;       /* 问题分词结果 */
    int    token_count;
} QAEntry;

struct QAMemory {
    QAEntry* entries;
    int      count;
    int      capacity;
};

QAMemory* qa_memory_create(const char* pipe_path, int max_entries) {
    if (!pipe_path || max_entries <= 0) return NULL;

    FILE* f = fopen(pipe_path, "r");
    if (!f) { fprintf(stderr, "[QA记忆] 无法打开 %s\n", pipe_path); return NULL; }

    QAMemory* m = (QAMemory*)calloc(1, sizeof(QAMemory));
    if (!m) { fclose(f); return NULL; }

    m->capacity = max_entries;
    m->entries  = (QAEntry*)calloc(max_entries, sizeof(QAEntry));
    if (!m->entries) { free(m); fclose(f); return NULL; }

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

        // 对 Q 分词
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

    // 对输入分词
    char* tokens[MAX_TOKENS];
    int tc = utf8_tokenize(input, tokens, MAX_TOKENS);
    if (tc <= 0) return NULL;

    // Token 集合去重
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

    // 遍历所有 QA 对，计算交集分数
    int best_idx = -1;
    float best_score = 0.0f;
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
        if (match > 0) {
            // 分数 = 匹配数 / sqrt(Q 长度)，奖励短问题高命中率
            float score = (float)match / sqrtf((float)e->token_count + 1);
            if (score > best_score) {
                best_score = score;
                best_idx = i;
                best_answer = e->answer;
            }
        }
    }

    // 清理输入分词
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
