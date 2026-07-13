/**
 * @file prefrontal.c
 * @brief 前额叶皮层实现 — 对话+意图+认知控制
 */

#include "prefrontal.h"
#include "cingulate.h"
#include "associative_reasoning.h"
#include "broca.h"
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

    /* ── 社交礼仪快速路由：先尝试从网络扩散生成，失败则用自然回退 ── */
    {
        const char* trimmed = input;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

        const char* social_seed = NULL;
        const char* social_fallback = NULL;

        /* 问候 */
        if ((strncmp(trimmed, "你好", 4) == 0 && (!trimmed[4] || trimmed[4] == ' ' || trimmed[4] == '!'))
            || strncmp(trimmed, "嗨", 2) == 0
            || (strstr(trimmed, "在吗") && strlen(trimmed) <= 8)
            || strncmp(trimmed, "早上好", 6) == 0
            || strncmp(trimmed, "晚上好", 6) == 0
            || strncmp(trimmed, "hello", 5) == 0
            || strncmp(trimmed, "hi", 2) == 0) {
            social_seed = "问候与打招呼";
            social_fallback = "你好！有什么我可以帮你的吗？";
        }

        /* 告别 */
        if (!social_seed && (strstr(trimmed, "再见")
            || strstr(trimmed, "拜拜")
            || strstr(trimmed, "bye")
            || strstr(trimmed, "晚安"))) {
            social_seed = "告别与道别";
            social_fallback = "再见，下次聊！";
        }

        /* 感谢 */
        if (!social_seed && (strstr(trimmed, "谢谢") || strstr(trimmed, "感谢"))) {
            social_seed = "感谢与礼貌回应";
            social_fallback = "不客气！";
        }

        if (social_seed) {
            GeneratedSequence seq = {0};
            int n = cingulate_diffusion_evaluate(pf->topology, social_seed, 0.08f,
                                                   pf->controller ? pf->controller->emergent_pos : NULL,
                                                   &seq);
            if (n >= 2) {
                char* wrapped = broca_wrap_response(
                    pf->topology,
                    pf->controller ? pf->controller->emergent_pos : NULL,
                    seq.words, seq.count);
                if (wrapped && wrapped[0]) {
                    if (strlen(wrapped) >= 3) return wrapped;
                    free(wrapped);
                } else {
                    free(wrapped);
                }
            }
            if (social_fallback) return strdup(social_fallback);
            return strdup("。");
        }
    }

    /* ── 直接图查询：属性问句 (X是什么颜色/味道/感觉) ── */
    {
        const char* attr_type = NULL;
        if (strstr(input, "什么颜色") || strstr(input, "什么色的"))
            attr_type = "颜色";
        else if (strstr(input, "什么味道"))
            attr_type = "味道";
        else if (strstr(input, "什么感觉"))
            attr_type = "感觉";

        if (attr_type) {
            SubTopology* vocab = master_get_sub_topology_by_type(pf->topology, TOPO_VOCABULARY);
            if (vocab && vocab->net) {
                /* 线性扫描找概念（兼容concept_hash/node_hash双哈希不一致） */
                int attr_nid = -1, subj_nid = -1;
                for (int vi = 0; vi < vocab->net->node_count; vi++) {
                    ReasoningNode* vn = vocab->net->nodes[vi];
                    if (vn && vn->concept) {
                        if (attr_nid < 0 && strcmp(vn->concept, attr_type) == 0)
                            attr_nid = vi;
                    }
                }
                const char* attr_pos = strstr(input, "什么");
                if (attr_pos && attr_pos > input && attr_nid >= 0) {
                    int max_len = (int)(attr_pos - input);
                    for (int w = 4; w >= 1 && subj_nid < 0; w--) {
                        for (int ci = 0; ci + w * 3 <= max_len; ci += 3) {
                            char sub[16] = {0};
                            memcpy(sub, input + ci, w * 3 < 15 ? w * 3 : 15);
                            for (int vi = 0; vi < vocab->net->node_count; vi++) {
                                ReasoningNode* vn = vocab->net->nodes[vi];
                                if (vn && vn->concept && strcmp(vn->concept, sub) == 0)
                                { subj_nid = vi; break; }
                            }
                            if (subj_nid >= 0) break;
                        }
                    }
                }
                if (subj_nid >= 0 && attr_nid >= 0) {
                    ReasoningNode* subj_node = vocab->net->nodes[subj_nid];
                    ReasoningNode* attr_node = vocab->net->nodes[attr_nid];
                    if (subj_node && attr_node) {
                        float best_score = 0.0f;
                        int best_nid = -1;
                        for (int se = 0; se < subj_node->edge_count; se++) {
                            int cand_nid = subj_node->edges[se].target ?
                                subj_node->edges[se].target->node_id : -1;
                            if (cand_nid < 0) continue;
                            float attr_weight = 0.0f;
                            for (int ae = 0; ae < attr_node->edge_count; ae++) {
                                if (attr_node->edges[ae].target &&
                                    attr_node->edges[ae].target->node_id == cand_nid) {
                                    attr_weight = attr_node->edges[ae].weight;
                                    break;
                                }
                            }
                            if (attr_weight > 0.0f) {
                                float score = subj_node->edges[se].weight + attr_weight;
                                if (score > best_score) {
                                    best_score = score;
                                    best_nid = cand_nid;
                                }
                            }
                        }
                        if (best_nid >= 0 && best_nid < vocab->net->node_count) {
                            ReasoningNode* answer = vocab->net->nodes[best_nid];
                            if (answer && answer->concept && concept_is_printable(answer->concept)) {
                                char buf[256];
                                snprintf(buf, sizeof(buf), "%s是%s色的。",
                                         subj_node->concept ? subj_node->concept : "",
                                         answer->concept);
                                return strdup(buf);
                            }
                        }
                    }
                }
            }
        }
    }

    /* 意图推断 */
    cognitive_controller_set_context(pf->controller, input, NULL);
    float ctx_activations[MAX_SUBTOPOS] = {0};
    calc_context_activations(pf->controller, ctx_activations);
    compute_intent(pf->controller, ctx_activations);

    /* ── 多层扩散生成 + ACC 自适应门控 ── */
    char* response = NULL;

    for (int attempt = 0; attempt < 3; attempt++) {
        float temperature = 0.15f + attempt * 0.12f;

        GeneratedSequence seq = {0};
        int n = cingulate_diffusion_evaluate(pf->topology, input, temperature,
                                               pf->controller ? pf->controller->emergent_pos : NULL,
                                               &seq);
        if (n < 2) continue;

        char gate_buf[128];
        const char* summary = cingulate_summary(&seq, gate_buf, sizeof(gate_buf));
        printf("[前额叶] 脊前扣带评分: %s\n", summary ? summary : "?");

        CingulateGate gate = cingulate_gate(&seq, pf->accept_threshold);

        if (gate == CINGULATE_BACKTRACK || gate == CINGULATE_REWRITE) {
            pf->total_retries++;
            printf("[前额叶] ACC %s (总分=%.3f), 尝试 %d/3\n",
                   gate == CINGULATE_BACKTRACK ? "BACKTRACK" : "REWRITE",
                   seq.total_score, attempt + 1);
            continue;
        }

        if (seq.total_score < pf->block_threshold) continue;

        char* wrapped = broca_wrap_response(
            pf->topology,
            pf->controller ? pf->controller->emergent_pos : NULL,
            seq.words, seq.count);
        if (!wrapped || !wrapped[0]) {
            free(wrapped);
            continue;
        }
        response = wrapped;

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

        cognitive_controller_snapshot(pf->controller, seq.total_score);
        return response;
    }

    /* ── 二段联想扩散 fallback ── */
    {
        printf("[前额叶] 扩散无产出, 尝试联想推理...\n");
        AssociativeEngine* assoc = assoc_engine_create(pf->topology);
        if (assoc) {
            int ac = associate_from_text(assoc, input, 0);
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
    (void)response;
    if (!pf || !pf->dialog || !rating) return;

    float confidence = (strcmp(rating, "correct") == 0 || strcmp(rating, "对") == 0) ? 0.95f : 0.2f;

    char key[512];
    snprintf(key, sizeof(key), "feedback:%s", input ? input : "");
    memory_store(pf->memory, key, (void*)rating, (int)strlen(rating) + 1,
                 MEMORY_TYPE_STRING, confidence);

    cognitive_controller_snapshot(pf->controller, confidence);
}
