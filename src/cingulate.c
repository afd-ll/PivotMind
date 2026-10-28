/**
 * @file cingulate.c
 * @brief 前扣带回实现 — 自我监控评分引擎
 *
 * 四维评分：语义一致性 + 模板匹配度 + 情绪一致性 + 长度合理性
 * 输入 ACC 门控决策：PASS → 输出 | BACKTRACK → 回溯 | REWRITE → 重来
 */

#include "cingulate.h"
#include "huarong_topology.h"
#include "multi_topology.h"      /* v0.6.3：要写 master->feedback_ptr（需完整结构） */
#include "generation_feedback.h" /* v0.6.3：生成端在线反馈 + 内调节 */
#include "diffusion.h"
#include "transition_model.h"    /* CX-39：bigram 链约束词序 */
#include "utf8_tokenizer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ================================================================
 *  语义一致性：生成词序列与输入在词汇拓扑中的边权验证
 *  P2: 不再依赖(通常为空的)TOPO_SEMANTIC，改用词汇拓扑边权
 *  对每个生成词，检查输入词到它的最大边权 → 强关联/弱关联/噪音
 * ================================================================ */
static float semantic_consistency(GeneratedSequence* seq,
                                   MasterTopology* topo,
                                   const char* input) {
    if (!topo || seq->count < 2) return 0.3f;
    if (!input || !input[0]) return 0.4f;

    /* 获取词汇拓扑 */
    SubTopology* vocab = master_get_sub_topology_by_type(topo, TOPO_VOCABULARY);
    if (!vocab || !vocab->net) return 0.5f;

    /* 对输入分词，找到对应的词汇拓扑节点ID */
    #define CING_TOK_MAX 256
    char* tokens[CING_TOK_MAX];
    int tok_count = utf8_tokenize(input, tokens, CING_TOK_MAX);
    if (tok_count > CING_TOK_MAX) tok_count = CING_TOK_MAX;
    int input_nids[CING_TOK_MAX];
    int input_nid_count = 0;
    for (int t = 0; t < tok_count && input_nid_count < CING_TOK_MAX; t++) {
        if (!tokens[t]) continue;
        int nid = huarong_net_find_concept(vocab->net, tokens[t]);
        if (nid >= 0) input_nids[input_nid_count++] = nid;
    }
    for (int t = 0; t < tok_count; t++) free(tokens[t]);
    if (input_nid_count == 0) return 0.4f;  /* 输入词都不在词汇表中 */

    /* 对每个生成词，检查与输入词的最大边权 */
    float total_relevance = 0.0f;
    for (int i = 0; i < seq->count && i < MAX_GENERATED_WORDS; i++) {
        const char* w = seq->words[i];
        if (!w) continue;
        int gen_nid = huarong_net_find_concept(vocab->net, w);
        if (gen_nid < 0) continue;
        ReasoningNode* gen_node = vocab->net->nodes[gen_nid];
        if (!gen_node) continue;

        /* 找任何输入节点到该生成词的最大边权 */
        float max_edge_weight = 0.0f;
        for (int in = 0; in < input_nid_count; in++) {
            ReasoningNode* in_node = vocab->net->nodes[input_nids[in]];
            if (!in_node) continue;
            for (int e = 0; e < in_node->edge_count; e++) {
                if (in_node->edges[e].target &&
                    in_node->edges[e].target->node_id == gen_nid) {
                    if (in_node->edges[e].weight > max_edge_weight)
                        max_edge_weight = in_node->edges[e].weight;
                    break;
                }
            }
        }
        /* 边权 >=0.5 → 强语义关联, 0.3~0.5 → 弱关联, <0.3 → 噪音 */
        total_relevance += (max_edge_weight >= 0.5f) ? 1.0f :
                           (max_edge_weight >= 0.3f) ? 0.5f : 0.1f;
    }

    float avg_relevance = seq->count > 0 ? total_relevance / (float)seq->count : 0.0f;
    return avg_relevance;
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

/* ── CX-39：用 bigram 链约束词序 ────────────────────────────────────────
 * 表来源：tools/qa_bigram 灌 907k QA 语料（当前 218 万唯一对）。
 * ⚠️ 表按 state 的节点 id 编址 ⇒ **换词表后必须重建**，否则 id 错位、整表作废。
 *
 * 为什么要它：CX-32 拆掉 QA 遮罩后生成层第一次暴露 —— 选词对了但词序是乱的
 * （「学习的编程的我想和想学」）。扩散按强度选词，本来就没有语序约束。
 *
 * 做法：**选词不动，只重排**。对每个起点做贪心（下一步取 P(next|prev) 最大），
 * 用 ΣlogP(命中) − 0.05×未命中 打分；只有明显优于原顺序（+0.3 nats）才替换。
 * ⚠️ 不改善就不动 —— 宁可保留原来的乱序，也不要引入新的乱序。
 * 开关：PIVOTMIND_TM_ORDER=0 关闭。 */
#define TM_ORDER_MAXW 12

static float _tm_chain_score(TransitionModel* tm, const int* id,
                             const int* ord, int n, int* hit_out) {
    float s = 0.0f;
    int hit = 0;
    for (int i = 0; i + 1 < n; i++) {
        int a = id[ord[i]], b = id[ord[i + 1]];
        if (a < 0 || b < 0) continue;
        float p = tm_prob(tm, a, b);
        if (p > 0.0f) { s += logf(p); hit++; }
    }
    if (hit_out) *hit_out = hit;
    return s - 0.05f * (float)(n - 1 - hit);
}

static void _tm_reorder_words(MasterTopology* topo, GeneratedSequence* seq) {
    if (!topo || !seq || seq->count < 3) return;
    const char* v = getenv("PIVOTMIND_TM_ORDER");
    if (v && v[0] == '0') return;
    TransitionModel* tm = tm_global();
    if (!tm || tm_pair_count(tm) < 1000) return;

    SubTopology* vocab = NULL;
    for (int t = 0; t < topo->sub_topo_count; t++)
        if (topo->sub_topologies[t] &&
            topo->sub_topologies[t]->type == TOPO_VOCABULARY) {
            vocab = topo->sub_topologies[t];
            break;
        }
    if (!vocab || !vocab->net) return;

    int n = seq->count;
    if (n > TM_ORDER_MAXW) n = TM_ORDER_MAXW;   /* 越长重排越不可靠，也越慢 */
    int id[TM_ORDER_MAXW], ord[TM_ORDER_MAXW], best[TM_ORDER_MAXW];
    int seqord[TM_ORDER_MAXW], used[TM_ORDER_MAXW];
    int mapped = 0;
    for (int i = 0; i < n; i++) {
        id[i] = huarong_net_find_concept(vocab->net, seq->words[i]);
        ord[i] = i;
        best[i] = i;
        if (id[i] >= 0) mapped++;
    }
    if (mapped < 3) return;                     /* 词不在词汇拓扑里 ⇒ 无从评分 */

    int hit0 = 0;
    float base = _tm_chain_score(tm, id, ord, n, &hit0);
    if (hit0 == 0) return;                      /* 一条可用转移都没有 ⇒ 不动 */
    float bestScore = base;

    for (int s0 = 0; s0 < n; s0++) {
        memset(used, 0, sizeof(int) * (size_t)n);
        used[s0] = 1;
        seqord[0] = s0;
        for (int k = 1; k < n; k++) {
            int prev = seqord[k - 1];
            int pick = -1;
            float pick_p = -1.0f;
            for (int w = 0; w < n; w++) {
                if (used[w]) continue;
                float p = (id[prev] >= 0 && id[w] >= 0)
                          ? tm_prob(tm, id[prev], id[w]) : 0.0f;
                if (p > pick_p) { pick_p = p; pick = w; }
            }
            if (pick < 0) break;
            seqord[k] = pick;
            used[pick] = 1;
        }
        int hit = 0;
        float sc = _tm_chain_score(tm, id, seqord, n, &hit);
        if (sc > bestScore) {
            bestScore = sc;
            memcpy(best, seqord, sizeof(int) * (size_t)n);
        }
    }

    if (bestScore <= base + 0.3f) return;       /* 没明显改善 ⇒ 保持原序 */
    const char* tmp[TM_ORDER_MAXW];
    for (int i = 0; i < n; i++) tmp[i] = seq->words[i];
    for (int i = 0; i < n; i++) seq->words[i] = tmp[best[i]];
    fprintf(stderr, "[词序] bigram 重排 %.2f → %.2f（%d 词）\n",
            (double)base, (double)bestScore, n);
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

    /* v0.5.10 fix: 对话扩散全程持读锁——diffusion_generate 锁外遍历
     * nodes/edges 并写 emergent_class_ids（已删），词巩固线程并发
     * realloc 数组 → 悬垂指针 SIGSEGV（08-08 三次 139 崩溃实锤，
     * 栈: diffusion_generate → emergent_pos_tag）。
     * 调用者（prefrontal/dmn/pfe）均无持锁，读锁不会嵌套死锁。 */
    pthread_rwlock_rdlock(&topo->rwlock);

    memset(out_seq, 0, sizeof(*out_seq));

    DiffusionCtx dctx;
    if (diffusion_init(&dctx, topo) != 0) {
        pthread_rwlock_unlock(&topo->rwlock);
        return 0;
    }
    dctx.temperature = temperature;
    dctx.emergent_pos = emergent_pos;

    const char* words[DIFF_MAX_SEQUENCE];
    int n = diffusion_generate(&dctx, input, words, DIFF_MAX_SEQUENCE);
    if (n < 2) {
        diffusion_cleanup(&dctx);
        pthread_rwlock_unlock(&topo->rwlock);
        return 0;
    }

    for (int i = 0; i < n && i < MAX_GENERATED_WORDS; i++)
        out_seq->words[i] = words[i];
    out_seq->count = n < MAX_GENERATED_WORDS ? n : MAX_GENERATED_WORDS;

    /* CX-39：先按 bigram 链重排，再交给 ACC 评估 —— 评价必须看到最终词序 */
    _tm_reorder_words(topo, out_seq);

    /* v0.6.3 反馈（在线）：把「这次生成走出去了多远」就地写进注入位。
     *   边数 = dctx.spread_edges_out（diffusion_generate 刚算出）
     *   词数 = n（本函数的返回值，也是 out_seq->count 的来源）
     * ⚠️ 只写、不消费 —— 本轮只落「反馈 / 内调节」两条量；情绪→参数那一环不动。
     * ⚠️ 不在 dmn 路径写（做梦不算"这次对话走出多远"）⇒ 来源天然分开。 */
    if (topo->feedback_ptr) {
        generation_feedback_observe((GenerationFeedback*)topo->feedback_ptr,
                                    dctx.spread_edges_out, n);
    }

    cingulate_evaluate(out_seq, topo, input, 5);

    diffusion_cleanup(&dctx);
    pthread_rwlock_unlock(&topo->rwlock);
    return n;
}
