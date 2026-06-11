/**
 * @file prefrontal.c
 * @brief 前额叶皮层实现 — 对话+意图+认知控制
 */

#include "prefrontal.h"
#include "cingulate.h"
#include "diffusion.h"
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

    /* ── 多层扩散生成 + ACC 自适应门控 ── */
    char* response = NULL;

    DiffusionCtx dctx;
    if (diffusion_init(&dctx, pf->topology) != 0) return NULL;

    const char* words[DIFF_MAX_SEQUENCE];
    int n = diffusion_generate(&dctx, input, words, DIFF_MAX_SEQUENCE);
    if (n < 2) return NULL;

    /* 序列评分 */
    GeneratedSequence seq = {0};
    for (int i = 0; i < n && i < MAX_GENERATED_WORDS; i++) seq.words[i] = words[i];
    seq.count = n < MAX_GENERATED_WORDS ? n : MAX_GENERATED_WORDS;
    cingulate_evaluate(&seq, pf->topology, input, 5);

    /* 硬阻断 */
    if (seq.total_score < pf->block_threshold) return NULL;

    /* 拼合成回复 + 自适应阈值更新 */
    char buf[2048];
    int pos = 0;
    const char* connectors[] = {"","的","是","和","了","在"};
    for (int w = 0; w < n && pos < (int)sizeof(buf)-10; w++) {
        if (w > 0 && w < n-1 && (w % 3 == 0))
            pos += snprintf(buf+pos, sizeof(buf)-pos, "%s", connectors[w % 6]);
        pos += snprintf(buf+pos, sizeof(buf)-pos, "%s", words[w]);
    }
    response = strdup(buf);

    pf->recent_scores[pf->recent_pos] = seq.total_score;
    pf->recent_pos = (pf->recent_pos + 1) % 100;
    if (pf->recent_pos % 10 == 0) {
        float buf2[100]; memcpy(buf2, pf->recent_scores, sizeof(buf2));
        int m = pf->recent_pos < 10 ? pf->recent_pos + 1 : 100;
        for (int i = 0; i < m-1; i++)
            for (int j = i+1; j < m; j++)
                if (buf2[i] < buf2[j]) { float t = buf2[i]; buf2[i] = buf2[j]; buf2[j] = t; }
        int p75 = (int)(m * 0.25f);
        float new_th = p75 < m ? buf2[p75] : buf2[0];
        if (new_th > pf->accept_threshold + 0.02f) pf->accept_threshold = new_th;
        if (pf->accept_threshold < 0.10f) pf->accept_threshold = 0.10f;
    }

    cognitive_controller_snapshot(pf->controller, 0.5f);
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
