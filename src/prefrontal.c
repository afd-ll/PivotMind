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

    printf("[前额叶] 就绪 (对话系统+认知调度)\n");
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

    /* 执行对话 */
    DialogReasoning* reasoning = NULL;
    char* response = dialog_process(pf->dialog, input, &reasoning);

    if (response) {
        cognitive_controller_snapshot(pf->controller, 0.5f);
        if (reasoning) dialog_reasoning_destroy(reasoning);
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
