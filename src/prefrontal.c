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

    pf->accept_threshold = 0.55f;
    pf->max_retries = 3;

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

    /* ── DLPFC 生成 + ACC 监控 + 回溯循环 ── */
    char* response = NULL;
    int retries = 0;

    for (int attempt = 0; attempt < pf->max_retries; attempt++) {
        DialogReasoning* reasoning = NULL;
        char* candidate = dialog_process(pf->dialog, input, &reasoning);

        if (!candidate) continue;

        /* ── ACC 评估 ── */
        GeneratedSequence seq = {0};
        /* 解析 candidate 为单词序列 */
        char* tok = strtok(strdup(candidate), " \t\n\r");
        while (tok && seq.count < MAX_GENERATED_WORDS) {
            seq.words[seq.count] = tok;
            seq.word_ids[seq.count] = 0;  /* ID 后续接入 */
            seq.count++;
            tok = strtok(NULL, " \t\n\r");
        }

        cingulate_evaluate(&seq, pf->topology, input, 5);

        CingulateGate gate = cingulate_gate(&seq, pf->accept_threshold);

        if (gate == CINGULATE_PASS) {
            /* 通过 — 输出 */
            response = candidate;
            if (reasoning) dialog_reasoning_destroy(reasoning);
            pf->total_retries += retries;
            if (retries > 0) fprintf(stderr, "[ACC] 回溯%d次后通过 (得分=%.2f)\n",
                                     retries, seq.total_score);
            break;
        }

        if (gate == CINGULATE_BACKTRACK && attempt < pf->max_retries - 1) {
            /* 回溯 — 告知 controller 降低出错的子拓扑权重 */
            fprintf(stderr, "[ACC] 回溯 (step=%d, 原因=%s, 得分=%.2f)\n",
                    seq.backtrack_step, seq.error_msg ? seq.error_msg : "?",
                    seq.total_score);
            cognitive_controller_snapshot(pf->controller, 0.1f);  /* 降低本路径权重 */
            retries++;
            if (reasoning) dialog_reasoning_destroy(reasoning);
            free(candidate);
            continue;
        }

        /* 彻底失败 — 用最后一次结果 */
        if (reasoning) dialog_reasoning_destroy(reasoning);
        free(candidate);
        break;
    }

    if (response) {
        cognitive_controller_snapshot(pf->controller, 0.5f);
    }

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
