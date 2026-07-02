/**
 * @file cingulate.c
 * @brief 前扣带回实现 — 自我监控评分引擎
 *
 * 四维评分：语义一致性 + 模板匹配度 + 情绪一致性 + 长度合理性
 * 输入 ACC 门控决策：PASS → 输出 | BACKTRACK → 回溯 | REWRITE → 重来
 */

#include "cingulate.h"
#include "huarong_topology.h"
#include "diffusion.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ================================================================
 *  语义一致性：生成词序列与输入的语义层重叠度
 * ================================================================ */
static float semantic_consistency(GeneratedSequence* seq,
                                   MasterTopology* topo,
                                   const char* input) {
    if (!topo || seq->count < 2) return 0.3f;

    /* 在语义拓扑中找 input 的激活节点 */
    SubTopology* sem = master_get_sub_topology_by_type(topo, TOPO_SEMANTIC);
    if (!sem || !sem->net) return 0.5f;  /* 无语义层跳过 */

    /* 统计生成词中哪些在语义拓扑有对应 */
    int matched = 0;
    for (int i = 0; i < seq->count && i < MAX_GENERATED_WORDS; i++) {
        const char* w = seq->words[i];
        if (!w) continue;
        for (int j = 0; j < sem->net->node_count; j++) {
            ReasoningNode* n = sem->net->nodes[j];
            if (n && n->concept && strcmp(n->concept, w) == 0) {
                matched++;
                break;
            }
        }
        if (i < seq->count - 1) {
            /* O(1) 连接哈希查找相邻词之间的边 */
            ReasoningNode* na = NULL, *nb = NULL;
            for (int j = 0; j < sem->net->node_count; j++) {
                if (na && nb) break;
                ReasoningNode* n = sem->net->nodes[j];
                if (!n || !n->concept) continue;
                if (!na && strcmp(n->concept, seq->words[i]) == 0)   na = n;
                if (!nb && strcmp(n->concept, seq->words[i+1]) == 0) nb = n;
            }
            if (na && nb) {
                /* 线性扫描连接（已知连接数量有限，O(conn_count)可接受） */
                for (int k = 0; k < na->edge_count; k++) {
                    if (na->edges[k].target == nb) {
                        float edge_w = na->edges[k].weight;
                        if (edge_w > 0.1f) matched += 2;
                        else matched += 1;
                        break;
                    }
                }
            }
        }
    }
    (void)input;  /* 未来：比对 input 语义向量 */

    float max_score = (float)(seq->count * 3);
    return max_score > 0 ? (float)matched / max_score : 0.5f;
}

/* ================================================================
 *  模板匹配度：生成词序列能否被某条模板容纳
 * ================================================================ */
static float template_fit(GeneratedSequence* seq, MasterTopology* topo) {
    if (!topo || seq->count < 3) return 0.2f;

    SubTopology* tpl = master_get_sub_topology_by_type(topo, TOPO_TEMPLATE);
    if (!tpl || !tpl->net) return 0.4f;

    /* 统计模板节点的自连接数 → 模板越完整分数越高 */
    int total_conns = 0;
    int sampled = 0;
    for (int i = 0; i < 50 && i < tpl->net->node_count; i++) {
        ReasoningNode* n = tpl->net->nodes[i];
        if (n) { total_conns += n->edge_count; sampled++; }
    }
    float avg_conn = sampled > 0 ? (float)total_conns / sampled : 0;

    /* 模板连接密度越高 → 句式越完整 → 分数越高 */
    float density = avg_conn / 20.0f;
    if (density > 1.0f) density = 1.0f;

    /* 最近一次构建成功率（从模板节点激活度推断） */
    float activity_sum = 0;
    int act_count = 0;
    for (int i = 0; i < 20 && i < tpl->net->node_count; i++) {
        ReasoningNode* n = tpl->net->nodes[i];
        if (n) { activity_sum += n->activation; act_count++; }
    }
    float avg_activity = act_count > 0 ? activity_sum / act_count : 0.5f;

    return density * 0.6f + avg_activity * 0.4f;
}

/* ================================================================
 *  情绪一致性：生成序列的情绪层激活是否与当前基调一致
 * ================================================================ */
static float emotion_consistency(GeneratedSequence* seq, MasterTopology* topo) {
    if (!topo) return 0.5f;

    SubTopology* emo = master_get_sub_topology_by_type(topo, TOPO_EMOTION);
    if (!emo || !emo->net || emo->net->node_count == 0) return 0.5f;

    /* 情绪层平均 valence → 一致性 = 1 - |variance| */
    float valence_sum = 0;
    float valence_sq = 0;
    int vn = 0;
    for (int i = 0; i < 50 && i < emo->net->node_count; i++) {
        ReasoningNode* n = emo->net->nodes[i];
        if (n) { valence_sum += n->valence; valence_sq += n->valence * n->valence; vn++; }
    }
    float variance = vn > 1 ? (valence_sq - valence_sum * valence_sum / vn) / (vn - 1) : 0;

    /* 方差越小 = 情绪越一致 */
    float score = 1.0f - sqrtf(variance < 0 ? 0 : variance);
    if (score < 0.1f) score = 0.1f;
    (void)seq;
    return score;
}

/* ================================================================
 *  长度合理性：3~20个词最合理
 * ================================================================ */
static float length_reasonability(GeneratedSequence* seq) {
    if (!seq || seq->count == 0) return 0.0f;
    if (seq->count <= 1) return 0.1f;
    if (seq->count <= 2) return 0.2f;
    if (seq->count >= 3 && seq->count <= 8) return 1.0f;
    if (seq->count <= 12) return 0.7f;
    if (seq->count <= 20) return 0.4f;
    return 0.2f;
}

/* ================================================================
 *  主评估函数
 * ================================================================ */
void cingulate_evaluate(GeneratedSequence* seq,
                         MasterTopology* topo,
                         const char* input,
                         int max_backtrack_depth) {
    if (!seq || seq->count == 0) return;

    seq->semantic_score = semantic_consistency(seq, topo, input);
    seq->template_score = template_fit(seq, topo);
    seq->emotion_score  = emotion_consistency(seq, topo);
    seq->length_score   = length_reasonability(seq);

    /* 加权综合 */
    seq->total_score = seq->semantic_score * 0.40f
                     + seq->template_score * 0.30f
                     + seq->emotion_score  * 0.15f
                     + seq->length_score   * 0.15f;

    /* 回溯定位：找到最低分维度对应的步数 */
    seq->backtrack_step = -1;
    seq->error_msg = NULL;

    if (seq->semantic_score < 0.25f) {
        seq->backtrack_step = max_backtrack_depth / 2;
        seq->error_msg = "语义不一致";
    } else if (seq->template_score < 0.20f) {
        seq->backtrack_step = max_backtrack_depth / 3;
        seq->error_msg = "模板匹配失败";
    } else if (seq->length_score < 0.2f) {
        seq->backtrack_step = 0;
        seq->error_msg = "长度异常";
    }
}

CingulateGate cingulate_gate(GeneratedSequence* seq, float threshold) {
    if (!seq) return CINGULATE_REWRITE;
    if (seq->total_score >= threshold) return CINGULATE_PASS;
    if (seq->backtrack_step >= 0) return CINGULATE_BACKTRACK;
    return CINGULATE_REWRITE;
}

const char* cingulate_summary(GeneratedSequence* seq, char* buf, int size) {
    if (!seq || !buf) return "null";
    snprintf(buf, size,
        "S=%.2f(语义%.2f 模板%.2f 情绪%.2f 长度%.2f) %s",
        seq->total_score,
        seq->semantic_score, seq->template_score,
        seq->emotion_score, seq->length_score,
        seq->error_msg ? seq->error_msg : "OK");
    return buf;
}

/* ================================================================
 *  公共管线：扩散生成 + ACC 评估
 *  消除 prefrontal.c / prefrontal_executive.c / DMN 的重复代码
 * ================================================================ */
int cingulate_diffusion_evaluate(MasterTopology* topo,
                                  const char* input,
                                  float temperature,
                                  struct EmergentPOS* emergent_pos,
                                  GeneratedSequence* out_seq) {
    if (!topo || !input || !out_seq) return 0;

    memset(out_seq, 0, sizeof(*out_seq));

    DiffusionCtx dctx;
    if (diffusion_init(&dctx, topo) != 0) return 0;
    dctx.temperature = temperature;
    dctx.emergent_pos = emergent_pos;

    const char* words[DIFF_MAX_SEQUENCE];
    int n = diffusion_generate(&dctx, input, words, DIFF_MAX_SEQUENCE);
    if (n < 2) return 0;

    for (int i = 0; i < n && i < MAX_GENERATED_WORDS; i++)
        out_seq->words[i] = words[i];
    out_seq->count = n < MAX_GENERATED_WORDS ? n : MAX_GENERATED_WORDS;
    cingulate_evaluate(out_seq, topo, input, 5);

    return n;
}
