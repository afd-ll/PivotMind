/**
 * @file prefrontal.c
 * @brief 前额叶皮层实现 — 对话+意图+认知控制
 */

#include "prefrontal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Prefrontal* prefrontal_create(MasterTopology* topology,
                               MemorySystem* memory,
                               CausalGraph* causal_graph,
                               ActiveLearner* learner) {
    if (!topology || !memory) return NULL;

    Prefrontal* pf = (Prefrontal*)calloc(1, sizeof(Prefrontal));
    if (!pf) return NULL;

    pf->topology = topology;
    pf->memory   = memory;

    pf->dialog = dialog_system_create(topology, memory, causal_graph, learner);
    if (!pf->dialog) { free(pf); return NULL; }

    pf->controller = cognitive_controller_create(topology, memory);
    if (!pf->controller) { dialog_system_destroy(pf->dialog); free(pf); return NULL; }

    pf->accept_threshold = 0.15f;   /* 初始很低，让回复先通过 */
    pf->block_threshold  = 0.08f;   /* 只拦截明显垃圾 */
    pf->max_retries = 1;
    pf->recent_pos = 0;

    printf("[前额叶] 就绪 (DLPFC生成+ACC监控, 阈值=%.2f, 最大回溯=%d)\n",
           pf->accept_threshold, pf->max_retries);
    return pf;
}

void prefrontal_destroy(Prefrontal* pf) {
    if (!pf) return;
    if (pf->controller) cognitive_controller_destroy(pf->controller);
    if (pf->dialog)     dialog_system_destroy(pf->dialog);
    free(pf);
}

char* prefrontal_chat(Prefrontal* pf, const char* input) {
    if (!pf || !pf->dialog || !input) return NULL;

    /* 意图推断 */
    cognitive_controller_set_context(pf->controller, input, NULL);
    float ctx_activations[MAX_SUBTOPOS] = {0};
    calc_context_activations(pf->controller, ctx_activations);
    compute_intent(pf->controller, ctx_activations);

    /* ── DLPFC 生成 + ACC 自适应门控 ── */
    char* response = NULL;

    for (int attempt = 0; attempt <= pf->max_retries; attempt++) {
        DialogReasoning* reasoning = NULL;
        char* candidate = dialog_process(pf->dialog, input, &reasoning);
        if (!candidate) { if (reasoning) dialog_reasoning_destroy(reasoning); continue; }

        /* ACC 评分 */
        GeneratedSequence seq = {0};
        char* dup = strdup(candidate);
        if (dup) {
            char* tok = strtok(dup, " \t\n\r");
            while (tok && seq.count < MAX_GENERATED_WORDS) {
                seq.words[seq.count] = tok;
                seq.count++;
                tok = strtok(NULL, " \t\n\r");
            }
        }
        cingulate_evaluate(&seq, pf->topology, input, 5);

        /* 硬阻断：垃圾回复 */
        if (seq.total_score < pf->block_threshold) {
            if (reasoning) dialog_reasoning_destroy(reasoning);
            free(dup);
            free(candidate);
            continue;
        }

        /* 放行 + 自适应更新阈值 */
        response = candidate;
        if (reasoning) dialog_reasoning_destroy(reasoning);
        pf->recent_scores[pf->recent_pos] = seq.total_score;
        pf->recent_pos = (pf->recent_pos + 1) % 100;

        /* 每10次采样更新阈值 = 75分位数 */
        if (pf->recent_pos % 10 == 0) {
            float buf[100];
            memcpy(buf, pf->recent_scores, sizeof(buf));
            /* 冒泡取75分位（只算前100个） */
            int n = pf->recent_pos < 10 ? pf->recent_pos + 1 : 100;
            for (int i = 0; i < n - 1; i++)
                for (int j = i + 1; j < n; j++)
                    if (buf[i] < buf[j]) { float t = buf[i]; buf[i] = buf[j]; buf[j] = t; }
            int p75 = (int)(n * 0.25f);
            float new_threshold = p75 < n ? buf[p75] : buf[0];
            if (new_threshold > pf->accept_threshold + 0.02f)
                pf->accept_threshold = new_threshold;
            if (pf->accept_threshold < 0.10f) pf->accept_threshold = 0.10f;
        }

        free(dup);
        break;
    }

    if (response) cognitive_controller_snapshot(pf->controller, 0.5f);
    return response;
}

void prefrontal_feedback(Prefrontal* pf, const char* input,
                          const char* response, const char* rating) {
    if (!pf || !pf->dialog || !rating) return;

    float confidence = (strcmp(rating, "correct") == 0 || strcmp(rating, "对") == 0) ? 0.95f : 0.2f;

    /* 在线学习：调高本对话中使用的拓扑权重 */
    // intent_base_learn called internally if needed

    /* 存入记忆 */
    char key[512];
    snprintf(key, sizeof(key), "feedback:%s", input ? input : "");
    memory_store(pf->memory, key, (void*)rating, (int)strlen(rating) + 1,
                 MEMORY_TYPE_STRING, confidence);

    cognitive_controller_snapshot(pf->controller, confidence);
}
