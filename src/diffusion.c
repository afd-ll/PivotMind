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
 *  虚词/停用词检查
 * ================================================================ */

static int is_function_word(const char* word) {
    if (!word || !word[0]) return 0;
    static const char* stopwords[] = {
        /* 英文虚词 */
        "a", "an", "the",
        "be", "is", "am", "are", "was", "were", "been", "being",
        "have", "has", "had", "having",
        "do", "does", "did", "doing",
        "will", "would", "shall", "should", "may", "might", "must", "can", "could",
        "of", "in", "to", "for", "with", "on", "at", "from", "by", "about", "into",
        "through", "during", "before", "after", "above", "below",
        "up", "down", "out", "off", "over", "under", "again", "then",
        "here", "there", "when", "where", "why", "how",
        "all", "both", "each", "every", "some", "more", "most", "other", "such",
        "no", "nor", "not", "only", "same", "so", "than", "too", "very", "just",
        "that", "this", "what", "which", "who",
        "it", "they", "them", "he", "she", "we", "you",
        "his", "her", "its", "their", "our", "my", "your",
        "and", "but", "or", "if", "while", "because", "as", "until",
        "also", "now", "well", "way", "even", "new", "make", "like",
        "Mr", "Mrs", "Ms", "Dr", "Mr.",
        /* 中文虚词 */
        "\xe7\x9a\x84",   /* 的 */
        "\xe4\xba\x86",   /* 了 */
        "\xe5\x9c\xa8",   /* 在 */
        "\xe6\x98\xaf",   /* 是 */
        "\xe6\x88\x91",   /* 我 */
        "\xe4\xbd\xa0",   /* 你 */
        "\xe4\xbb\x96",   /* 他 */
        "\xe5\xa5\xb9",   /* 她 */
        "\xe5\xae\x83",   /* 它 */
        "\xe4\xbb\xac",   /* 们 */
        "\xe8\xbf\x99",   /* 这 */
        "\xe9\x82\xa3",   /* 那 */
        "\xe5\x93\xaa",   /* 哪 */
        "\xe5\x92\x8c",   /* 和 */
        "\xe4\xb8\x8e",   /* 与 */
        "\xe6\x88\x96",   /* 或 */
        "\xe4\xbd\x86",   /* 但 */
        "\xe8\x80\x8c",   /* 而 */
        "\xe4\xb8\x94",   /* 且 */
        "\xe5\xb0\xb1",   /* 就 */
        "\xe4\xb9\x9f",   /* 也 */
        "\xe9\x83\xbd",   /* 都 */
        "\xe5\xbe\x88",   /* 很 */
        "\xe8\xbf\x98",   /* 还 */
        "\xe8\xa6\x81",   /* 要 */
        "\xe4\xbc\x9a",   /* 会 */
        "\xe8\x83\xbd",   /* 能 */
        "\xe5\x8f\xaf",   /* 可 */
        "\xe4\xbb\xa5",   /* 以 */
        "\xe6\x8a\x8a",   /* 把 */
        "\xe8\xa2\xab",   /* 被 */
        "\xe5\xaf\xb9",   /* 对 */
        "\xe4\xbb\x8e",   /* 从 */
        "\xe8\x87\xaa",   /* 自 */
        "\xe5\x88\xb0",   /* 到 */
        "\xe5\x90\x91",   /* 向 */
        "\xe7\x94\xa8",   /* 用 */
        "\xe7\x94\xb1",   /* 由 */
        "\xe4\xb8\xba",   /* 为 */
        "\xe7\xbb\x99",   /* 给 */
        "\xe8\xae\xa9",   /* 让 */
        "\xe5\x8f\xab",   /* 叫 */
        "\xe4\xbd\xbf",   /* 使 */
        "\xe4\xb8\x8a",   /* 上 */
        "\xe4\xb8\xad",   /* 中 */
        "\xe4\xb8\x8b",   /* 下 */
        "\xe7\x9d\x80",   /* 着 */
        "\xe8\xbf\x87",   /* 过 */
        "\xe5\xbe\x97",   /* 得 */
        "\xe4\xb9\x8b",   /* 之 */
        "\xe6\x89\x80",   /* 所 */
        "\xe5\xa6\x82",   /* 如 */
        "\xe8\x8b\xa5",   /* 若 */
        "\xe8\x99\xbd",   /* 虽 */
        "\xe5\x9b\xa0",   /* 因 */
        "\xe6\x95\x85",   /* 故 */
        "\xe5\x90\x97",   /* 吗 */
        "\xe5\x90\xa7",   /* 吧 */
        "\xe5\x91\xa2",   /* 呢 */
        "\xe5\x95\x8a",   /* 啊 */
        "\xe5\x93\xa6",   /* 哦 */
        "\xe5\x97\xaf",   /* 嗯 */
        "\xe4\xb8\xaa",   /* 个 */
        "\xe4\xba\x9b",   /* 些 */
        "\xe7\xa7\x8d",   /* 种 */
        "\xe6\xac\xa1",   /* 次 */
        "\xe5\x9b\x9e",   /* 回 */
        "\xe7\x82\xb9",   /* 点 */
        "\xe9\x87\x8c",   /* 里 */
        "\xe5\xa4\x96",   /* 外 */
        "\xe8\xbe\xb9",   /* 边 */
        NULL
    };
    for (const char** p = stopwords; *p; p++)
        if (strcmp(word, *p) == 0) return 1;
    return 0;
}

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
    ctx->temperature = 0.15f;  // 默认温度扰动

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
                      float decay,
                      float temperature) {
    if (!layer || !layer->net || !scores || active_count <= 0) return 0;

    int spread = 0;
    for (int i = 0; i < active_count; i++) {
        int nid = active_ids[i];
        if (nid < 0 || nid >= layer->net->node_count) continue;
        ReasoningNode* node = layer->net->nodes[nid];
        if (!node) continue;

        for (int c = 0; c < node->edge_count; c++) {
            int tid = node->edges[c].target ? node->edges[c].target->node_id : -1;
            if (tid < 0 || tid >= layer->net->node_count) continue;
            float w = node->edges[c].weight * decay;
            // 温度扰动：如果 temperature > 0，添加随机噪声
            if (temperature > 0.0f) {
                float noise = ((float)(rand() % 201) - 100.0f) / 100.0f;  // -1.0 ~ 1.0
                w *= (1.0f + noise * temperature);
                if (w < 0.0f) w = 0.0f;
            }
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

    /* ── 第0步：滑动窗口分词 → 词汇层初始激活 ── */
    int active_ids[DIFF_MAX_CANDIDATES];
    int active_count = 0;
    int input_len = (int)strlen(input);

    /* 逐字扫描 + bigram + trigram 全部尝试匹配 */
    for (int win = 3; win >= 1 && active_count == 0; win--) {
        for (int pos = 0; pos + win <= input_len; pos++) {
            if (active_count >= 64) break;
            char sub[16];
            int sl = win < 15 ? win : 15;
            memcpy(sub, input + pos, sl);
            sub[sl] = 0;
            if (sl < 1) continue;

            int nid = huarong_net_find_concept(ctx->vocab->net, sub);
            if (nid >= 0) {
                int dup = 0;
                for (int d = 0; d < active_count; d++)
                    if (active_ids[d] == nid) { dup = 1; break; }
                if (!dup) {
                    active_ids[active_count++] = nid;
                    ctx->vocab->net->nodes[nid]->activation += 0.2f;
                }
            }
        }
        if (active_count > 0) break;  /* 长匹配优先 */
    }
    if (active_count == 0) return 0;

    /* ── 分配/复用评分数组 (静态避免反复calloc) ── */
    int vn = ctx->vocab->net->node_count;
    int sn = ctx->semantic ? ctx->semantic->net->node_count : 0;
    int tn = ctx->template ? ctx->template->net->node_count : 0;
    int en = ctx->emotion  ? ctx->emotion->net->node_count  : 0;
    int need = vn;
    if (sn > need) need = sn;
    if (tn > need) need = tn;
    if (en > need) need = en;
    if (!ctx->_vocab_scores || ctx->_score_cap < need) {
        free(ctx->_vocab_scores); free(ctx->_sem_scores);
        free(ctx->_tpl_scores);   free(ctx->_emo_scores);
        ctx->_vocab_scores = (float*)calloc(need, sizeof(float));
        ctx->_sem_scores   = sn ? (float*)calloc(sn, sizeof(float)) : NULL;
        ctx->_tpl_scores   = tn ? (float*)calloc(tn, sizeof(float)) : NULL;
        ctx->_emo_scores   = en ? (float*)calloc(en, sizeof(float)) : NULL;
        ctx->_score_cap    = need;
    }
    float* vocab_scores = ctx->_vocab_scores;
    float* sem_scores   = ctx->_sem_scores;
    float* tpl_scores   = ctx->_tpl_scores;
    float* emo_scores   = ctx->_emo_scores;
    memset(vocab_scores, 0, vn * sizeof(float));
    if (sem_scores) memset(sem_scores, 0, sn * sizeof(float));
    if (tpl_scores) memset(tpl_scores, 0, tn * sizeof(float));
    if (emo_scores) memset(emo_scores, 0, en * sizeof(float));

    /* ── 第1步：词汇层初始扩散 ── */
    float cur_decay = 1.0f;
    int cur_ids[DIFF_MAX_CANDIDATES];
    int cur_count = active_count;
    memcpy(cur_ids, active_ids, cur_count * sizeof(int));

    for (int d = 0; d < ctx->depth && cur_count > 0; d++) {
        cur_decay *= ctx->decay;

        /* 词汇层自身扩散 */
        diffusion_spread(ctx->vocab, cur_ids, cur_count,
                         vocab_scores, cur_decay, ctx->temperature);

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
                                 sem_scores, cur_decay * 0.3f, ctx->temperature);
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

        /* 更新当前活跃集 = 本轮得分最高的 K 个词（跳过虚词） */
        DiffusionCandidate tmp[DIFF_MAX_CANDIDATES];
        int tmp_cnt = 0;
        for (int i = 0; i < vn && tmp_cnt < DIFF_MAX_CANDIDATES; i++) {
            if (vocab_scores[i] > 0.001f && ctx->vocab->net->nodes[i]) {
                const char* concept = ctx->vocab->net->nodes[i]->concept;
                if (concept && is_function_word(concept)) continue;
                tmp[tmp_cnt].node_id     = i;
                tmp[tmp_cnt].total_score = vocab_scores[i];
                tmp[tmp_cnt].word        = concept;
                tmp_cnt++;
            }
        }
        qsort(tmp, tmp_cnt, sizeof(DiffusionCandidate), _cand_cmp);

        cur_count = tmp_cnt < ctx->top_k ? tmp_cnt : ctx->top_k;
        for (int i = 0; i < cur_count; i++) {
            cur_ids[i] = tmp[i].node_id;
        }
    }

    /* ── 第2步：收敛 → 综合评分（跳过虚词；跨层反馈已通过 _cross_by_name 回流至 vocab_scores） ── */
    DiffusionCandidate final[DIFF_MAX_CANDIDATES];
    int final_cnt = 0;
    for (int i = 0; i < vn && final_cnt < DIFF_MAX_CANDIDATES; i++) {
        if (vocab_scores[i] < 0.001f) continue;
        ReasoningNode* n = ctx->vocab->net->nodes[i];
        if (!n || !n->concept) continue;
        if (is_function_word(n->concept)) continue;

        final[final_cnt].node_id       = i;
        final[final_cnt].vocab_score   = vocab_scores[i];
        final[final_cnt].semantic_score = 0;
        final[final_cnt].template_score = 0;
        final[final_cnt].emotion_score  = 0;
        final[final_cnt].total_score    = vocab_scores[i];  /* 已含跨层回流 */
        final[final_cnt].word = n->concept;
        final[final_cnt].used = 0;
        final_cnt++;
    }

    qsort(final, final_cnt, sizeof(DiffusionCandidate), _cand_cmp);

    /* ── 第3步：模板导向 → 从 template 层取最佳句式 ── */
    const char* tpl_pattern = NULL;
    if (ctx->template && tpl_scores && tn > 0) {
        float best_tpl_score = 0;
        for (int i = 0; i < tn; i++) {
            if (tpl_scores[i] > best_tpl_score && ctx->template->net->nodes[i]) {
                best_tpl_score = tpl_scores[i];
                tpl_pattern = ctx->template->net->nodes[i]->concept;
            }
        }
        if (tpl_pattern) printf("[扩散] 模板: %s (%.3f)\n", tpl_pattern, best_tpl_score);
    }

    /* ── 第4步：句式导向输出 ── */
    /* 解析模板概念中的连接词: "N的N是N的N" → ["", "的", "是", "的"] */
    const char* connectors[DIFF_MAX_SEQUENCE];
    char conn_pool[128];  /* 小对象池，避免 strdup 泄漏 */
    int conn_pool_pos = 0;
    int conn_count = 0;
    if (tpl_pattern) {
        const char* p = tpl_pattern;
        while (*p && conn_count < DIFF_MAX_SEQUENCE) {
            if (*p == 'N' || *p == 'V' || *p == 'A') {
                connectors[conn_count++] = "";  /* 实词位置 */
                p++;
            } else {
                /* 收集连续的非N字符作为连接词 */
                int ci = 0;
                while (*p && *p != 'N' && *p != 'V' && *p != 'A' && ci < 7 && conn_pool_pos + ci < 127)
                    conn_pool[conn_pool_pos + ci++] = *p++;
                if (ci > 0) {
                    conn_pool[conn_pool_pos + ci] = 0;
                    connectors[conn_count++] = conn_pool + conn_pool_pos;
                    conn_pool_pos += ci + 1;
                }
            }
        }
    }

    /* 如果模板没能提供足够连接词，用默认 */
    if (conn_count < 2) {
        const char* fallback[] = {"", "的", "是", "和", "了", "在"};
        for (int i = 0; i < 6; i++) connectors[i] = fallback[i];
        conn_count = 6;
    }

    int out = 0;
    const char* selected[DIFF_MAX_SEQUENCE];
    int sel = 0;

    for (int i = 0; i < final_cnt && out < max_output; i++) {
        if (final[i].used) continue;

        /* 过滤垃圾词: 单字、@符号、纯标点、虚词 */
        if (!final[i].word || strlen(final[i].word) < 2) continue;
        if (final[i].word[0] == '@' || final[i].word[0] == '?' ||
            final[i].word[0] == 'H' && final[i].word[1] == 'e') continue;
        if (is_function_word(final[i].word)) continue;

        int inhibited = 0;
        for (int s = 0; s < sel; s++) {
            if (final[i].word && selected[s] &&
                strcmp(final[i].word, selected[s]) == 0) {
                inhibited = 1; break;
            }
        }
        if (inhibited) continue;

        /* 模板连接词: 每2个实词插一次 */
        if (out > 0 && (out % 3 == 0)) {
            int ci = ((out - 1) % conn_count);
            if (connectors[ci] && connectors[ci][0]) {
                output_words[out++] = connectors[ci];
            }
        }
        output_words[out++] = final[i].word;
        selected[sel++] = final[i].word;
        final[i].used = 1;
    }

    /* ── 静态数组不释放 (ctx生命周期管理) ── */

    return out;
}
