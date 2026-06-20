/**
 * @file idea_arena.c
 * @brief 想法竞争竞技场实现 (v0.3)
 *
 * 五维竞争空间 + 侧抑制 + 多巴胺调制 + 胜者反馈回流
 */

#include "idea_arena.h"
#include "amygdala.h"
#include "huarong_topology.h"
#include "common.h"
#include "constants.h"
#include "error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ================================================================
 *  维度名称
 * ================================================================ */

static const char* DIM_NAMES[] = {
    "目标契合", "知识一致", "新颖性", "情绪效价", "可组合"
};

const char* arena_dimension_name(int dim) {
    if (dim >= 0 && dim < ARENA_DIM_COUNT) return DIM_NAMES[dim];
    return "未知";
}

/* ================================================================
 *  创建 / 销毁
 * ================================================================ */

IdeaArena* idea_arena_create(void) {
    IdeaArena* arena = (IdeaArena*)calloc(1, sizeof(IdeaArena));
    if (!arena) return NULL;

    /* 默认参数 */
    arena->lateral_inhibition   = ARENA_LATERAL_INHIBITION;
    arena->novelty_boost        = ARENA_NOVELTY_BOOST;
    arena->convergence_threshold = ARENA_CONVERGENCE_THRESH;

    /* 硬门槛 */
    arena->hard_thresholds[ARENA_DIM_GOAL_FIT]      = ARENA_DEFAULT_GOAL_THRESH;
    arena->hard_thresholds[ARENA_DIM_CONSISTENCY]   = ARENA_DEFAULT_CONSIS_THRESH;
    arena->hard_thresholds[ARENA_DIM_NOVELTY]       = ARENA_DEFAULT_NOVEL_THRESH;
    arena->hard_thresholds[ARENA_DIM_VALENCE]       = ARENA_DEFAULT_VALENCE_THRESH;
    arena->hard_thresholds[ARENA_DIM_COMPOSABILITY] = ARENA_DEFAULT_COMPOSE_THRESH;

    arena->winner_index = -1;

    pthread_mutex_init(&arena->lock, NULL);

    return arena;
}

void idea_arena_destroy(IdeaArena* arena) {
    if (!arena) return;
    pthread_mutex_destroy(&arena->lock);
    free(arena);
}

/* ================================================================
 *  上下文设置
 * ================================================================ */

void arena_set_context(IdeaArena* arena, MasterTopology* master,
                       CognitiveState* cog_state, const char* question) {
    if (!arena) return;
    arena->master    = master;
    arena->cog_state = cog_state;
    arena->question  = question;
}

/* ================================================================
 *  候选管理
 * ================================================================ */

int arena_add_candidate(IdeaArena* arena,
                        const int* node_ids, int path_len,
                        int topo_id, int source,
                        const char* summary) {
    if (!arena || arena->candidate_count >= ARENA_MAX_CANDIDATES)
        return -1;

    CandidateIdea* ci = &arena->candidates[arena->candidate_count];
    memset(ci, 0, sizeof(CandidateIdea));

    /* 复制路径节点 */
    int copy_len = path_len < ARENA_MAX_PATH ? path_len : ARENA_MAX_PATH;
    memcpy(ci->node_ids, node_ids, copy_len * sizeof(int));
    ci->path_len = copy_len;
    ci->topo_id  = topo_id;
    ci->source   = source;

    /* 摘要 */
    if (summary)
        snprintf(ci->summary, sizeof(ci->summary), "%.255s", summary);

    ci->activation = 0.5f;   /* 初始激活 — 公平起跑 */
    ci->round      = 0;

    int idx = arena->candidate_count;
    arena->candidate_count++;
    arena->total_ideas++;

    return idx;
}

void arena_clear(IdeaArena* arena) {
    if (!arena) return;
    arena->candidate_count = 0;
    arena->winner_index    = -1;
    arena->winner_score    = 0.0f;
    arena->winner_reason[0] = '\0';
}

/* ================================================================
 *  五维评分 — 全部候选
 * ================================================================ */

void arena_score_all(IdeaArena* arena) {
    if (!arena) return;

    for (int i = 0; i < arena->candidate_count; i++) {
        arena_score_candidate(arena, &arena->candidates[i]);
    }
}

void arena_score_candidate(IdeaArena* arena, CandidateIdea* idea) {
    if (!arena || !idea) return;

    MasterTopology* master      = arena->master;
    CognitiveState*  cstate     = arena->cog_state;
    const char*      question   = arena->question;

    idea->scores[ARENA_DIM_GOAL_FIT]      = arena_score_goal_fit(idea, master, question);
    idea->scores[ARENA_DIM_CONSISTENCY]   = arena_score_consistency(idea, master);
    idea->scores[ARENA_DIM_NOVELTY]       = arena_score_novelty(idea, master);
    idea->scores[ARENA_DIM_VALENCE]       = arena_score_valence(idea, cstate);
    idea->scores[ARENA_DIM_COMPOSABILITY] = arena_score_composability(idea, arena);

    /* 更新置信度 */
    idea->confidence = 0.0f;
    for (int d = 0; d < ARENA_DIM_COUNT; d++)
        idea->confidence += idea->scores[d];
    idea->confidence /= ARENA_DIM_COUNT;
}

/* ================================================================
 *  维度1: 目标契合度
 * ================================================================ */

float arena_score_goal_fit(const CandidateIdea* idea,
                           MasterTopology* master,
                           const char* question) {
    if (!question || !question[0] || !idea) return 0.5f;  /* 无问题 → 中分 */

    (void)master;

    /* 方法：检查路径节点概念与问题关键词的文本重叠 */
    float score = 0.3f;  /* 基础分 */

    /* 解析问题关键词 */
    int q_len = (int)strlen(question);

    /* 启发式：路径越长（信息量越大），目标契合度基准越高 */
    float len_bonus = idea->path_len > 0
        ? logf(1.0f + (float)idea->path_len) / logf(5.0f) * 0.2f
        : 0.0f;
    score += len_bonus;

    /* 检查 summary 和 question 的 token 重叠 */
    int overlap_char = 0;
    int q_chars[256] = {0};

    for (int qi = 0; qi < q_len && qi < 256; qi++)
        q_chars[(unsigned char)question[qi]] = 1;

    for (int si = 0; idea->summary[si]; si++) {
        if (q_chars[(unsigned char)idea->summary[si]]) {
            overlap_char++;
            q_chars[(unsigned char)idea->summary[si]] = 0;  /* 不重复计数 */
        }
    }

    if (q_len > 0) {
        float char_overlap = (float)overlap_char / (float)(q_len < 256 ? q_len : 256);
        score += char_overlap * 0.3f;
    }

    return clamp(score, 0.1f, 1.0f);
}

/* ================================================================
 *  维度2: 知识一致性
 * ================================================================ */

float arena_score_consistency(const CandidateIdea* idea,
                              MasterTopology* master) {
    if (!idea || !master) return 0.5f;

    float score = 0.7f;  /* 默认偏高 — 除非检测到矛盾 */

    SubTopology* sem = master_get_sub_topology_by_type(master, TOPO_SEMANTIC);
    if (!sem || !sem->net) return score;

    /* 检查路径节点是否有矛盾边 */
    for (int i = 0; i < idea->path_len; i++) {
        int node_id = idea->node_ids[i];
        if (node_id < 0) continue;

        if (node_id >= sem->net->node_count) continue;
        ReasoningNode* node = sem->net->nodes[node_id];
        if (!node) continue;

        /* 检查节点 confidence — 高置信节点 → 更一致 */
        score += node->confidence * 0.05f;

        /* 检查是否有 self_contradiction 标记（如果字段存在） */
        /* 简化：低 confidence 节点降低一致性分数 */
        if (node->confidence < 0.3f)
            score -= 0.1f;
    }

    return clamp(score, 0.1f, 1.0f);
}

/* ================================================================
 *  维度3: 新颖性
 * ================================================================ */

float arena_score_novelty(const CandidateIdea* idea,
                          MasterTopology* master) {
    if (!idea || !master) return 0.3f;

    float score = 0.3f;  /* 默认中低 — 默认偏向已知 */
    int   nodes_found = 0;

    SubTopology* sem = master_get_sub_topology_by_type(master, TOPO_SEMANTIC);
    if (!sem || !sem->net) return score;

    /* 低 selection_count → 高新颖性 */
    for (int i = 0; i < idea->path_len; i++) {
        int node_id = idea->node_ids[i];
        if (node_id < 0) continue;
        if (node_id >= sem->net->node_count) continue;

        ReasoningNode* node = sem->net->nodes[node_id];
        if (!node) continue;

        nodes_found++;

        /* selection_count 越高 → 越不新颖（反向） */
        float novelty = 1.0f - clamp((float)node->selection_count / 100.0f, 0.0f, 0.9f);
        score += novelty * 0.1f;
    }

    if (nodes_found > 0) score /= (float)nodes_found;

    /* 低置信度节点 → 可能是新探索 → 加新颖分 */
    float novelty_from_confidence = 0.0f;
    for (int i = 0; i < idea->path_len; i++) {
        int node_id = idea->node_ids[i];
        if (node_id < 0) continue;
        if (node_id >= sem->net->node_count) continue;
        ReasoningNode* node = sem->net->nodes[node_id];
        if (node)
            novelty_from_confidence += (1.0f - node->confidence) * 0.15f;
    }
    score += novelty_from_confidence;

    return clamp(score, 0.05f, 1.0f);
}

/* ================================================================
 *  维度4: 情绪效价
 * ================================================================ */

float arena_score_valence(const CandidateIdea* idea,
                          CognitiveState* cog_state) {
    if (!idea) return 0.5f;

    float score = 0.5f;

    /* 如果有认知状态，根据当前情绪偏置效价评分 */
    if (cog_state) {
        /* 正向情绪 → 偏好正值效价 */
        float mood = cog_state->emotion_pleasure - cog_state->emotion_pain;
        score += mood * 0.3f;

        /* 社交驱动高 → 偏好适中/正面 */
        score += cog_state->drive_social * 0.1f;

        /* 好奇驱动 → 中性偏正 */
        score += cog_state->drive_curiosity * 0.05f;
    }

    return clamp(score, 0.0f, 1.0f);
}

/* ================================================================
 *  维度5: 可组合性
 * ================================================================ */

float arena_score_composability(const CandidateIdea* idea,
                                IdeaArena* arena) {
    if (!idea || !arena) return 0.3f;

    /* 只有一个候选 → 自然满分 */
    if (arena->candidate_count <= 1) return 1.0f;

    float score = 0.5f;
    int   relationships = 0;

    for (int j = 0; j < arena->candidate_count; j++) {
        CandidateIdea* other = &arena->candidates[j];
        if (other == idea) continue;

        float overlap = idea_overlap(idea, other);

        /* 
         * 中等重叠 (0.2~0.6) → 互补 → 高分（互为补充但不冗余）
         * 高重叠 (>0.6) → 冗余 → 低分
         * 极低重叠 (<0.1) → 完全无关 → 中低分
         */
        if (overlap > 0.6f)       score -= 0.2f;
        else if (overlap > 0.2f)  score += 0.25f;
        else if (overlap < 0.05f) score -= 0.1f;

        relationships++;
    }

    if (relationships > 0)
        score += logf(1.0f + (float)relationships) / logf(5.0f) * 0.15f;

    return clamp(score, 0.1f, 1.0f);
}

/* ================================================================
 *  idea_overlap 与 composite_score
 * ================================================================ */

float idea_overlap(const CandidateIdea* a, const CandidateIdea* b) {
    if (!a || !b || a->path_len == 0 || b->path_len == 0) return 0.0f;

    int overlap = 0;
    for (int ai = 0; ai < a->path_len; ai++) {
        for (int bj = 0; bj < b->path_len; bj++) {
            if (a->node_ids[ai] == b->node_ids[bj]) {
                overlap++;
                break;
            }
        }
    }

    int max_len = a->path_len > b->path_len ? a->path_len : b->path_len;
    return max_len > 0 ? (float)overlap / (float)max_len : 0.0f;
}

float idea_composite_score(const CandidateIdea* idea) {
    if (!idea) return 0.0f;

    /* 加权求和 — 目标契合和一致性权重更高 */
    static const float weights[ARENA_DIM_COUNT] = {
        0.30f,   /* Goal Fit    — 最重要：回答了问题吗？ */
        0.25f,   /* Consistency — 第二重要：和知识一致吗？ */
        0.15f,   /* Novelty     — 有新颖性更好 */
        0.15f,   /* Valence     — 情绪合适 */
        0.15f    /* Composability — 能和其他想法拼接 */
    };

    float score = 0.0f;
    for (int d = 0; d < ARENA_DIM_COUNT; d++)
        score += idea->scores[d] * weights[d];

    return clamp(score, 0.0f, 1.0f);
}

/* ================================================================
 *  核心竞争算法
 * ================================================================ */

int arena_compete(IdeaArena* arena) {
    if (!arena || arena->candidate_count == 0) {
        arena->winner_index = -1;
        return -1;
    }

    /* 杏仁核调制——在竞争前注入 */
    if (arena->cog_state) {
        arena->novelty_boost      = arena_compute_novelty_boost(arena->cog_state);
        arena->lateral_inhibition = arena_compute_inhibition(arena->cog_state);
    }

    /* 多轮竞争直到收敛 */
    for (int round = 0; round < ARENA_MAX_ROUNDS; round++) {
        arena->total_rounds++;
        int converged = arena_compete_round(arena);
        if (converged) break;
    }

    arena->winner_index = arena_pick_winner(arena);

    if (arena->winner_index >= 0) {
        CandidateIdea* w = &arena->candidates[arena->winner_index];
        arena->winner_score = w->activation;

        /* 生成选择理由 */
        snprintf(arena->winner_reason, sizeof(arena->winner_reason),
                 "选中#%d: 目标契合=%.2f 一致=%.2f 新颖=%.2f 效价=%.2f 可组合=%.2f "
                 "综合=%.2f | 抑制了%d个相似候选",
                 arena->winner_index,
                 w->scores[0], w->scores[1], w->scores[2],
                 w->scores[3], w->scores[4],
                 idea_composite_score(w),
                 arena->candidate_count - 1);

        arena->total_wins++;
    }

    return arena->winner_index;
}

int arena_compete_round(IdeaArena* arena) {
    if (!arena) return -1;
    int n = arena->candidate_count;
    if (n == 0) return -1;
    if (n == 1) return 1;  /* 只有1个候选，立即收敛 */

    /* 第一步：硬门槛淘汰 — Phase 3 差异化衰减 */
    int survivors[ARENA_MAX_CANDIDATES];
    int surv_count = 0;

    for (int i = 0; i < n; i++) {
        CandidateIdea* ci = &arena->candidates[i];
        int   fail_dims    = 0;
        float total_gap    = 0.0f;  /* 累计低于门槛的差距 */
        int   total_count  = 0;

        for (int d = 0; d < ARENA_DIM_COUNT; d++) {
            if (ci->scores[d] < arena->hard_thresholds[d]) {
                fail_dims++;
                total_gap += arena->hard_thresholds[d] - ci->scores[d];
                total_count++;
            }
        }

        if (fail_dims == 0) {
            /* 全部通过 → 存活 */
            survivors[surv_count++] = i;
        } else {
            /* 差异化淘汰：计算衰退严重度
             * severity ∈ [0.1, 1.0]
             *   仅 1 维轻微不达标 → severity=0.2（轻度惩罚）
             *   多维严重不达标   → severity=0.9（重度惩罚） */
            float avg_gap = total_count > 0 ? total_gap / (float)total_count : 0.1f;
            float dim_penalty = (float)fail_dims / (float)ARENA_DIM_COUNT;
            float severity    = 0.1f + dim_penalty * 0.4f + avg_gap * 0.5f;
            if (severity > 0.95f) severity = 0.95f;

            /* severity 高 → activation 残留低（严重淘汰）
             * severity 低 → activation 有小残留（半淘汰，仍有复活可能） */
            ci->activation = 0.25f * (1.0f - severity);
            /* range: [0.0125, 0.225] */
        }
    }

    if (surv_count == 0) {
        /* 全员淘汰 → 差异化恢复：按综合得分恢复初始激活 */
        for (int i = 0; i < n; i++) {
            CandidateIdea* ci = &arena->candidates[i];
            float composite = idea_composite_score(ci);
            /* 高综合分 → 恢复更多激活（0.25~0.40）
             * 低综合分 → 恢复较少激活（0.15~0.25） */
            ci->activation = 0.15f + composite * 0.25f;
            if (ci->activation > 0.40f) ci->activation = 0.40f;
        }
        surv_count = n;
        for (int i = 0; i < n; i++) survivors[i] = i;
    }

    /* 第二步：侧抑制 — 相似候选互相削弱 */
    for (int si = 0; si < surv_count; si++) {
        CandidateIdea* ci = &arena->candidates[survivors[si]];
        float inhibition = 0.0f;

        for (int sj = 0; sj < surv_count; sj++) {
            if (si == sj) continue;
            CandidateIdea* cj = &arena->candidates[survivors[sj]];

            float overlap = idea_overlap(ci, cj);

            if (overlap > 0.4f) {
                float cj_power = idea_composite_score(cj);
                float ci_power = idea_composite_score(ci);

                if (cj_power > ci_power) {
                    inhibition += overlap * (cj_power - ci_power) * arena->lateral_inhibition;
                }
            }
        }

        ci->activation -= inhibition;
        if (ci->activation < 0.05f) ci->activation = 0.0f;
    }

    /* 第三步：新颖性奖励（多巴胺调制） */
    for (int si = 0; si < surv_count; si++) {
        CandidateIdea* ci = &arena->candidates[survivors[si]];
        if (ci->activation <= 0.0f) continue;

        float bonus = ci->scores[ARENA_DIM_NOVELTY] * arena->novelty_boost * 0.5f;
        ci->activation += bonus;
        if (ci->activation > 1.0f) ci->activation = 1.0f;
    }

    /* 检查收敛：最高和次高差距 */
    float best_act  = -1.0f;
    float second_act = -1.0f;

    for (int si = 0; si < surv_count; si++) {
        float act = arena->candidates[survivors[si]].activation;
        if (act > best_act) {
            second_act = best_act;
            best_act   = act;
        } else if (act > second_act) {
            second_act = act;
        }
    }

    if (best_act - second_act >= arena->convergence_threshold)
        return 1;  /* 已收敛 */

    return 0;  /* 需要更多轮 */
}

int arena_pick_winner(IdeaArena* arena) {
    if (!arena || arena->candidate_count == 0) return -1;

    float best_act = -1.0f;
    int   best_idx = 0;

    for (int i = 0; i < arena->candidate_count; i++) {
        if (arena->candidates[i].activation > best_act) {
            best_act = arena->candidates[i].activation;
            best_idx = i;
        }
    }

    return best_act > 0.1f ? best_idx : -1;
}

/* ================================================================
 *  杏仁核调制
 * ================================================================ */

float arena_compute_novelty_boost(CognitiveState* state) {
    if (!state) return ARENA_NOVELTY_BOOST;

    /*
     * 探索模式 → 高新颖性奖励（愿意冒险）
     * 利用模式 → 低新颖性奖励（偏好熟悉）
     *
     * 探索率范围 0~1，映射为新颖奖励 0.05~0.5
     */
    return 0.05f + state->explore_rate * 0.45f;
}

float arena_compute_inhibition(CognitiveState* state) {
    if (!state) return ARENA_LATERAL_INHIBITION;

    /*
     * 利用模式 → 高抑制（要求连贯）
     * 探索模式 → 低抑制（允许多样性）
     *
     * 映射: inhibit = 0.8 - explore_rate * 0.3 → 0.5~0.8
     */
    return 0.8f - state->explore_rate * 0.3f;
}

/* ================================================================
 *  胜者回流 — Phase 2: 含 motivational_bias 边权重调节
 * ================================================================ */

int arena_feedback_to_master(IdeaArena* arena, MasterTopology* master) {
    if (!arena || !master) return -1;
    if (arena->winner_index < 0) return 0;

    CandidateIdea* winner = &arena->candidates[arena->winner_index];

    SubTopology* sem = master_get_sub_topology_by_type(master, TOPO_SEMANTIC);
    if (!sem || !sem->net) return -1;

    /* ── 胜者路径节点 → confidence 小幅上升 ── */
    for (int i = 0; i < winner->path_len; i++) {
        int nid = winner->node_ids[i];
        if (nid < 0 || nid >= sem->net->node_count) continue;

        ReasoningNode* node = sem->net->nodes[nid];
        if (!node) continue;

        int lock_idx = nid & (PM_NODE_LOCK_COUNT - 1);
        pthread_mutex_lock(&sem->net->node_locks[lock_idx]);

        /* EMA 上升 */
        node->confidence = node->confidence * 0.95f + 0.05f;
        if (node->confidence > 1.0f) node->confidence = 1.0f;

        /* 胜者路径节点间边权重小幅提升 */
        if (i > 0) {
            int prev_nid = winner->node_ids[i - 1];
            if (prev_nid >= 0 && prev_nid < sem->net->node_count) {
                for (int e = 0; e < node->edge_count && e < node->edge_capacity; e++) {
                    if (node->edges[e].target &&
                        node->edges[e].target->node_id == prev_nid) {
                        /* EMA 提升边权重（正向反馈） */
                        node->edges[e].weight =
                            node->edges[e].weight * 0.92f + 0.08f;
                        if (node->edges[e].weight > 1.0f)
                            node->edges[e].weight = 1.0f;
                        break;
                    }
                }
            }
        }

        pthread_mutex_unlock(&sem->net->node_locks[lock_idx]);
    }

    /* ── 被淘汰/低分候选 → 路径节点惩罚 — Phase 3 分级衰减 ── */
    for (int j = 0; j < arena->candidate_count; j++) {
        CandidateIdea* ci = &arena->candidates[j];
        if (ci == winner) continue;

        float act = ci->activation;

        /* 分级判定：
         *   act < 0.05  → penalty_level = 3 (重度淘汰)
         *   act < 0.15  → penalty_level = 2 (中度淘汰)
         *   act < 0.30  → penalty_level = 1 (轻度劣势)
         *   act >= 0.30 → penalty_level = 0 (不惩罚) */
        int penalty_level = 0;
        if (act < 0.05f)       penalty_level = 3;
        else if (act < 0.15f)  penalty_level = 2;
        else if (act < 0.30f)  penalty_level = 1;
        else continue;  /* 高分不惩罚 */

        /* 分级衰减系数：
         *   L3: motivational_bias *= 0.60, selection_count -= 3
         *   L2: motivational_bias *= 0.75, selection_count -= 2
         *   L1: motivational_bias *= 0.90, selection_count -= 1 */
        float mb_decay[4]   = { 0.00f, 0.90f, 0.75f, 0.60f };
        int   sel_penalty[4] = { 0, 1, 2, 3 };

        for (int i = 0; i < ci->path_len; i++) {
            int nid = ci->node_ids[i];
            if (nid < 0 || nid >= sem->net->node_count) continue;

            ReasoningNode* node = sem->net->nodes[nid];
            if (!node) continue;

            int lock_idx = nid & (PM_NODE_LOCK_COUNT - 1);
            pthread_mutex_lock(&sem->net->node_locks[lock_idx]);

            /* selection_count 分级衰减 */
            node->selection_count -= sel_penalty[penalty_level];
            if (node->selection_count < 0)
                node->selection_count = 0;

            /* 路径边的 motivational_bias 分级衰减 */
            if (i > 0) {
                int prev_nid = ci->node_ids[i - 1];
                if (prev_nid >= 0 && prev_nid < sem->net->node_count) {
                    for (int e = 0; e < node->edge_count &&
                         e < node->edge_capacity; e++) {
                        if (node->edges[e].target &&
                            node->edges[e].target->node_id == prev_nid) {
                            node->edges[e].motivational_bias *= mb_decay[penalty_level];
                            if (node->edges[e].motivational_bias < 0.005f)
                                node->edges[e].motivational_bias = 0.005f;
                            break;
                        }
                    }
                }
            }

            pthread_mutex_unlock(&sem->net->node_locks[lock_idx]);
        }
    }

    return 0;
}

/* ================================================================
 *  调试输出
 * ================================================================ */

void arena_print_state(IdeaArena* arena, FILE* out) {
    if (!arena || !out) return;

    fprintf(out, "===== IdeaArena State =====\n");
    fprintf(out, "Candidates: %d, Winner: %d, Score: %.3f\n",
            arena->candidate_count, arena->winner_index, arena->winner_score);
    fprintf(out, "Inhibition=%.2f NovelBoost=%.2f Converge=%.2f\n",
            arena->lateral_inhibition, arena->novelty_boost,
            arena->convergence_threshold);
    fprintf(out, "Reason: %s\n", arena->winner_reason);

    for (int i = 0; i < arena->candidate_count; i++) {
        CandidateIdea* ci = &arena->candidates[i];
        fprintf(out, "  #%d act=%.3f conf=%.3f | G=%.2f C=%.2f N=%.2f V=%.2f Co=%.2f | %s\n",
                i, ci->activation, ci->confidence,
                ci->scores[0], ci->scores[1], ci->scores[2],
                ci->scores[3], ci->scores[4],
                ci->summary[0] ? ci->summary : "(no summary)");
    }

    fprintf(out, "============================\n");
}
