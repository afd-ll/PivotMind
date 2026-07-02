/**
 * @file prefrontal.c
 * @brief 前额叶皮层实现 — 对话+意图+认知控制
 */

#include "prefrontal.h"
#include "cingulate.h"
#include "associative_reasoning.h"
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

    /* 多候选生成 + ACC 门控回溯（最多尝试 3 次） */
    for (int attempt = 0; attempt < 3; attempt++) {
        /* 逐次提高温度扰动，探索不同候选 */
        float temperature = 0.15f + attempt * 0.12f;

        GeneratedSequence seq = {0};
        int n = cingulate_diffusion_evaluate(pf->topology, input, temperature,
                                               pf->controller ? pf->controller->emergent_pos : NULL,
                                               &seq);
        if (n < 2) continue;

        char gate_buf[128];
        const char* summary = cingulate_summary(&seq, gate_buf, sizeof(gate_buf));
        printf("[前额叶] 脊前扣带评分: %s\n", summary ? summary : "?");

        /* ACC 门控决策 */
        CingulateGate gate = cingulate_gate(&seq, pf->accept_threshold);

        if (gate == CINGULATE_BACKTRACK || gate == CINGULATE_REWRITE) {
            pf->total_retries++;
            printf("[前额叶] ACC %s (总分=%.3f), 尝试 %d/3\n",
                   gate == CINGULATE_BACKTRACK ? "BACKTRACK" : "REWRITE",
                   seq.total_score, attempt + 1);
            continue;  /* 回溯 → 尝试下一次 */
        }

        /* 硬阻断 */
        if (seq.total_score < pf->block_threshold) continue;

        /* 拼合（扩散引擎已含模板连接词，直接拼接） */
        char buf[2048];
        int pos = 0;
        for (int w = 0; w < seq.count && pos < (int)sizeof(buf)-10; w++)
            pos += snprintf(buf+pos, sizeof(buf)-pos, "%s", seq.words[w]);
        response = strdup(buf);

        /* 更新自适应阈值 */
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

        /* 传递 ACC 真实评分，替代硬编码 0.5 */
        cognitive_controller_snapshot(pf->controller, seq.total_score);
        return response;
    }

    /* ── 二段联想扩散 fallback（扩散无产出时）── */
    {
        printf("[前额叶] 扩散无产出, 尝试联想推理...\n");
        AssociativeEngine* assoc = assoc_engine_create(pf->topology);
        if (assoc) {
            int ac = associate_from_text(assoc, input, 0);  /* 0 = 动态深度 */
            if (ac > 1) {
                response = generate_from_associations(assoc, 2048, input, ctx_activations);
                if (response) {
                    printf("[前额叶] 联想推理产出 (%d 候选)\n", ac);
                }
            }
            assoc_engine_free(assoc);
        }
        if (!response) return NULL;
        return response;
    }

    return NULL;
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
