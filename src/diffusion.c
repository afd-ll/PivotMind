/**
 * @file diffusion.c
 * @brief 多层扩散引擎实现
 *
 * 同时激活词汇/语义/模板/情绪四层，各层互相约束，
 * 输出序列由跨模态加权投票+侧抑制产生。
 */

#include "diffusion.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ================================================================
 *  初始化
 * ================================================================ */

int diffusion_init(DiffusionCtx* ctx, MasterTopology* master) {
    if (!ctx || !master) return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->master = master;
    ctx->depth  = 3;
    ctx->top_k  = 8;
    ctx->output_len = 20;
    ctx->decay  = 0.7f;

    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        switch (sub->type) {
            case TOPO_VOCABULARY: ctx->vocab    = sub; break;
            case TOPO_SEMANTIC:   ctx->semantic  = sub; break;
            case TOPO_TEMPLATE:   ctx->template  = sub; break;
            case TOPO_EMOTION:    ctx->emotion   = sub; break;
            default: break;
        }
    }

    if (!ctx->vocab) return -1;
    printf("[扩散引擎] 就绪 (词汇=%d, 语义=%s, 模板=%s, 情绪=%s, 深度=%d, topK=%d)\n",
           ctx->vocab->net->node_count,
           ctx->semantic ? "ON" : "OFF",
           ctx->template ? "ON" : "OFF",
           ctx->emotion  ? "ON" : "OFF",
           ctx->depth, ctx->top_k);
    return 0;
}

/* ================================================================
 *  单层扩散
 * ================================================================ */

int diffusion_spread(SubTopology* layer,
                      int* active_ids, int active_count,
                      float* scores,
                      float decay) {
    if (!layer || !layer->net || !scores || active_count <= 0) return 0;

    int spread = 0;
    for (int i = 0; i < active_count; i++) {
        int nid = active_ids[i];
        if (nid < 0 || nid >= layer->net->node_count) continue;
        ReasoningNode* node = layer->net->nodes[nid];
        if (!node) continue;

        for (int c = 0; c < node->connection_count; c++) {
            int tid = node->connections[c] ? node->connections[c]->node_id : -1;
            if (tid < 0 || tid >= layer->net->node_count) continue;
            float w = node->connection_weights[c] * decay;
            scores[tid] += w;
            spread++;
        }
    }
    return spread;
}

/* ================================================================
 *  跨层激活（名称匹配）
 *  源层激活的节点通过 concept 名匹配到目标层对应节点
 * ================================================================ */

static int _cross_by_name(SubTopology* src, int* src_ids, int src_count,
                           SubTopology* dst, float* scores, float weight) {
    if (!src || !dst || !scores) return 0;
    int spread = 0;
    for (int i = 0; i < src_count; i++) {
        ReasoningNode* sn = src->net->nodes[src_ids[i]];
        if (!sn || !sn->concept) continue;
        for (int j = 0; j < dst->net->node_count; j++) {
            ReasoningNode* dn = dst->net->nodes[j];
            if (dn && dn->concept && strcmp(sn->concept, dn->concept) == 0) {
                scores[j] += weight;
                spread++;
            }
        }
    }
    return spread;
}

/* ================================================================
 *  侧抑制
 * ================================================================ */

void diffusion_side_inhibit(DiffusionCandidate* cands, int count,
                             const char** selected, int sel_count) {
    if (!cands || !selected) return;

    for (int i = 0; i < count; i++) {
        if (cands[i].used) continue;
        for (int s = 0; s < sel_count; s++) {
            if (!selected[s] || !cands[i].word) continue;
            /* 同类词互抑：完全相同或前2字相同 */
            if (strcmp(cands[i].word, selected[s]) == 0) {
                cands[i].total_score *= 0.1f;  /* 完全重复，大抑 */
                break;
            }
            if (strncmp(cands[i].word, selected[s], 2) == 0) {
                cands[i].total_score *= 0.5f;  /* 同前缀，小抑 */
            }
        }
    }
}

/* ================================================================
 *  模板评分
 * ================================================================ */

float diffusion_template_score(DiffusionCtx* ctx,
                                const char** words, int count) {
    if (!ctx || !ctx->template || !ctx->template->net || count < 2) return 0.5f;

    float score = 0;
    int pairs = 0;
    for (int i = 0; i < count - 1; i++) {
        if (!words[i] || !words[i+1]) continue;
        /* 在模板拓扑中找相邻词对 */
        for (int j = 0; j < ctx->template->net->node_count && j < 50; j++) {
            ReasoningNode* tn = ctx->template->net->nodes[j];
            if (!tn || !tn->concept) continue;
            if (strstr(tn->concept, words[i]) && strstr(tn->concept, words[i+1]))
                score += tn->activation;
        }
        pairs++;
    }
    return pairs > 0 ? score / pairs : 0.5f;
}

/* ================================================================
 *  候选排序（降序）
 * ================================================================ */

static int _cand_cmp(const void* a, const void* b) {
    float sa = ((const DiffusionCandidate*)a)->total_score;
    float sb = ((const DiffusionCandidate*)b)->total_score;
    return (sa < sb) ? 1 : (sa > sb) ? -1 : 0;
}

/* ================================================================
 *  多层扩散主函数
 * ================================================================ */

int diffusion_generate(DiffusionCtx* ctx,
                        const char* input,
                        const char** output_words,
                        int max_output) {
    if (!ctx || !ctx->master || !ctx->vocab || !input || !output_words) return 0;

    /* ── 第0步：输入分词 → 词汇层初始激活 ── */
    int active_ids[DIFF_MAX_CANDIDATES];
    int active_count = 0;

    char copy[2048];
    strncpy(copy, input, sizeof(copy)-1);
    copy[sizeof(copy)-1] = 0;
    char* tok = strtok(copy, " \t\n\r。，！？、；：");
    while (tok && active_count < 64) {
        for (int i = 0; i < ctx->vocab->net->node_count; i++) {
            ReasoningNode* n = ctx->vocab->net->nodes[i];
            if (n && n->concept && strcmp(n->concept, tok) == 0) {
                active_ids[active_count++] = i;
                n->activation += 0.2f;  /* 输入激活 */
                break;
            }
        }
        tok = strtok(NULL, " \t\n\r。，！？、；：");
    }
    if (active_count == 0) return 0;

    /* ── 分配评分数组 ── */
    int vn = ctx->vocab->net->node_count;
    float* vocab_scores = (float*)calloc(vn, sizeof(float));
    float* sem_scores   = NULL;
    float* tpl_scores   = NULL;
    float* emo_scores   = NULL;
    if (!vocab_scores) return 0;

    int sn = ctx->semantic ? ctx->semantic->net->node_count : 0;
    int tn = ctx->template ? ctx->template->net->node_count : 0;
    int en = ctx->emotion  ? ctx->emotion->net->node_count  : 0;
    if (sn) sem_scores = (float*)calloc(sn, sizeof(float));
    if (tn) tpl_scores = (float*)calloc(tn, sizeof(float));
    if (en) emo_scores = (float*)calloc(en, sizeof(float));

    /* ── 第1步：词汇层初始扩散 ── */
    float cur_decay = 1.0f;
    int cur_ids[DIFF_MAX_CANDIDATES];
    int cur_count = active_count;
    memcpy(cur_ids, active_ids, cur_count * sizeof(int));

    for (int d = 0; d < ctx->depth && cur_count > 0; d++) {
        cur_decay *= ctx->decay;

        /* 词汇层自身扩散 */
        diffusion_spread(ctx->vocab, cur_ids, cur_count, vocab_scores, cur_decay);

        /* 跨到语义层（名称匹配） */
        if (ctx->semantic && sem_scores) {
            _cross_by_name(ctx->vocab, cur_ids, cur_count,
                            ctx->semantic, sem_scores, cur_decay * 0.4f);
            /* 语义层内部扩散 */
            int sem_active[64]; int sem_cnt = 0;
            for (int i = 0; i < sn && sem_cnt < 64; i++) {
                if (sem_scores[i] > 0.01f) sem_active[sem_cnt++] = i;
            }
            if (sem_cnt > 0) {
                diffusion_spread(ctx->semantic, sem_active, sem_cnt,
                                 sem_scores, cur_decay * 0.3f);
                /* 语义层回归词汇层 */
                _cross_by_name(ctx->semantic, sem_active, sem_cnt,
                                ctx->vocab, vocab_scores, cur_decay * 0.3f);
            }
        }

        /* 跨到模板层（名称匹配） */
        if (d == 0 && ctx->template && tpl_scores) {
            _cross_by_name(ctx->vocab, cur_ids, cur_count,
                            ctx->template, tpl_scores, cur_decay * 0.5f);
        }

        /* 更新当前活跃集 = 本轮得分最高的 K 个词 */
        DiffusionCandidate tmp[DIFF_MAX_CANDIDATES];
        int tmp_cnt = 0;
        for (int i = 0; i < vn && tmp_cnt < DIFF_MAX_CANDIDATES; i++) {
            if (vocab_scores[i] > 0.001f && ctx->vocab->net->nodes[i]) {
                tmp[tmp_cnt].node_id     = i;
                tmp[tmp_cnt].total_score = vocab_scores[i];
                tmp[tmp_cnt].word        = ctx->vocab->net->nodes[i]->concept;
                tmp_cnt++;
            }
        }
        qsort(tmp, tmp_cnt, sizeof(DiffusionCandidate), _cand_cmp);

        cur_count = tmp_cnt < ctx->top_k ? tmp_cnt : ctx->top_k;
        for (int i = 0; i < cur_count; i++) {
            cur_ids[i] = tmp[i].node_id;
        }
    }

    /* ── 第2步：收敛 → 加权综合评分 ── */
    DiffusionCandidate final[DIFF_MAX_CANDIDATES];
    int final_cnt = 0;
    for (int i = 0; i < vn && final_cnt < DIFF_MAX_CANDIDATES; i++) {
        if (vocab_scores[i] < 0.001f) continue;
        ReasoningNode* n = ctx->vocab->net->nodes[i];
        if (!n || !n->concept) continue;

        final[final_cnt].node_id       = i;
        final[final_cnt].vocab_score   = vocab_scores[i];
        final[final_cnt].semantic_score = (sn && sem_scores) ? sem_scores[i % sn] : 0;
        final[final_cnt].template_score = (tn && tpl_scores) ? tpl_scores[i % tn] : 0;
        final[final_cnt].emotion_score  = (en && emo_scores) ? emo_scores[i % en] : 0;
        final[final_cnt].total_score    =
            vocab_scores[i] * 0.45f +
            final[final_cnt].semantic_score * 0.25f +
            final[final_cnt].template_score * 0.20f +
            final[final_cnt].emotion_score  * 0.10f;
        final[final_cnt].word = n->concept;
        final[final_cnt].used = 0;
        final_cnt++;
    }

    qsort(final, final_cnt, sizeof(DiffusionCandidate), _cand_cmp);

    /* ── 第3步：逐词选取 + 侧抑制 ── */
    int out = 0;
    const char* selected[DIFF_MAX_SEQUENCE];
    int sel = 0;

    for (int i = 0; i < final_cnt && out < max_output; i++) {
        if (final[i].used) continue;

        /* 侧抑制：检查与已选词的重叠 */
        int inhibited = 0;
        for (int s = 0; s < sel; s++) {
            if (final[i].word && selected[s] &&
                strcmp(final[i].word, selected[s]) == 0) {
                inhibited = 1; break;
            }
        }
        if (inhibited) continue;

        output_words[out++] = final[i].word;
        selected[sel++] = final[i].word;
        final[i].used = 1;

        /* 侧抑制传播 */
        for (int j = i + 1; j < final_cnt; j++) {
            if (!final[j].used && final[j].word && final[i].word &&
                strcmp(final[j].word, final[i].word) == 0) {
                final[j].total_score *= 0.2f;  /* 重复词大抑 */
            }
        }
    }

    /* ── 清理 ── */
    free(vocab_scores);
    free(sem_scores);
    free(tpl_scores);
    free(emo_scores);

    return out;
}
