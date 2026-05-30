/**
 * @file cognitive_controller.c
 * @brief 认知调度中心实现
 */

#include "cognitive_controller.h"
#include "causal_reasoning.h"
#include "concept_abstraction.h"
#include "node_hash.h"
#include "utf8_tokenizer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// 前向声明
static void intent_base_load(CognitiveController* cc);

// ==================== 辅助函数 ====================

static const char* TOPO_NAMES[] = {
    "词汇", "语义", "情绪", "语法",
    "上下文", "领域", "语用", "文化",
    "概念", "主拓扑", "模板拓扑", "预留"
};

const char* cognitive_controller_topo_name(int topo_type) {
    if (topo_type >= 0 && topo_type < MAX_SUBTOPOS)
        return TOPO_NAMES[topo_type];
    return "未知";
}

// ==================== 创建/销毁 ====================

CognitiveController* cognitive_controller_create(MasterTopology* master,
                                                  MemorySystem* memory) {
    CognitiveController* cc = (CognitiveController*)calloc(1, sizeof(CognitiveController));
    if (!cc) return NULL;

    cc->master = master;
    cc->memory = memory;

    // 默认偏置
    cc->context_bias = 0.6f;
    cc->novelty_bias = 0.4f;
    cc->valence_bias = 0.3f;
    cc->coherence_target = 0.5f;

    // 默认负反馈参数
    cc->satisfaction_threshold = PM_EVALUATE_THRESHOLD;
    cc->max_retry = MAX_RETRY;
    cc->correction_strength = 0.3f;
    cc->retry_count = 0;

    // 初始意图向量：词汇和语义拓扑更高权重（回答生成的主要信号源）
    // 词汇=0.20 语义=0.18 情绪=0.10 语法=0.10 上下文=0.12 领域=0.08 语用=0.07 文化=0.05 概念=0.10
    static const float default_intent[MAX_SUBTOPOS] = {
        0.20f, 0.18f, 0.10f, 0.10f, 0.12f, 0.08f, 0.07f, 0.05f, 0.10f
    };
    for (int i = 0; i < MAX_SUBTOPOS; i++) {
        cc->intent_weights[i] = default_intent[i];
        cc->prev_intent_weights[i] = cc->intent_weights[i];
        cc->learned_base[i] = 1.0f;  // 在线学习因子初始=无调整
    }
    cc->prev_satisfaction = 0.0f;

    printf("[认知调度] 创建成功\n");
    // 尝试加载持久化的 learned_base
    intent_base_load(cc);
    return cc;
}

void cognitive_controller_destroy(CognitiveController* cc) {
    if (!cc) return;
    // 释放 patterns 数组及其内部的 node_ids
    if (cc->patterns) {
        for (int i = 0; i < cc->pattern_count; i++) {
            free(cc->patterns[i].node_ids);
        }
        free(cc->patterns);
    }
    free(cc);
}

// ==================== 重置 ====================

void cognitive_controller_reset_round(CognitiveController* cc) {
    if (!cc) return;
    cc->retry_count = 0;
    cc->current_input = NULL;
    cc->last_response = NULL;
}

void cognitive_controller_set_context(CognitiveController* cc,
                                       const char* input,
                                       const char* last_response) {
    if (!cc) return;
    cc->current_input = input;
    cc->last_response = last_response;
}

void cognitive_controller_set_intent(CognitiveController* cc, int intent_type) {
    if (!cc) return;
    if (intent_type >= 0 && intent_type <= 7)
        cc->intent_type = intent_type;
}

// ==================== 上下文关联度计算 ====================

/**
 * 估算各个子拓扑与当前输入的上下文关联度。
 * 简版：在词汇/语义/情绪拓扑中匹配输入token，
 * 返回各子拓扑的活跃节点比例。
 */
void calc_context_activations(CognitiveController* cc,
                                     float* ctx_activations) {
    if (!cc || !cc->master) return;
    MasterTopology* m = cc->master;

    // 先对输入分词
    char* tokens[64];
    int tok_count = 0;
    if (cc->current_input) {
        tok_count = utf8_tokenize(cc->current_input, tokens, 64);
    }

    // 排序 tokens 用于二分查找（避免 O(n²) 扫描）
    qsort(tokens, tok_count, sizeof(char*), (int(*)(const void*,const void*))strcmp);

    for (int t = 0; t < m->sub_topo_count && t < MAX_SUBTOPOS; t++) {
        SubTopology* sub = m->sub_topologies[t];
        if (!sub || !sub->net || sub->net->node_count == 0) {
            ctx_activations[t] = 0.0f;
            continue;
        }

        int match_count = 0;
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (!node || !node->concept) continue;
            // 精确匹配 token（O(log n) 二分查找）+ 反向模糊匹配
            if (tok_count > 0 && bsearch(&node->concept, tokens, tok_count,
                                         sizeof(char*),
                                         (int(*)(const void*,const void*))strcmp)) {
                match_count++;
            } else if (strstr(node->concept, cc->current_input)) {
                // 概念包含输入 → 模糊反向匹配
                match_count++;
            }
        }

        ctx_activations[t] = (float)match_count / sub->net->node_count;
    }

    // 释放 tokens
    for (int i = 0; i < tok_count; i++) {
        free(tokens[i]);
    }
}

// ==================== 新颖性计算 ====================

/**
 * 计算各个子拓扑的新颖性因子。
 * 基于短时记忆（最近对话）中各子拓扑被使用的频率。
 * 用得多 → 新颖性低 → 降权。
 */
static void calc_novelty_factors(CognitiveController* cc,
                                 float* novelty_factors) {
    // 默认：未知拓扑新颖性=1.0
    for (int i = 0; i < MAX_SUBTOPOS; i++)
        novelty_factors[i] = 1.0f;

    if (!cc || !cc->master) return;

    // 使用 leaky integrator (recent_activation) 测量近期活跃度
    // recent_activation ∈ [0, 1.0]，每次节点激活 +0.2，每轮 ×0.8 衰减
    // 公式：novelty = 1 / (1 + 10 * recent_activation)
    //   最近未使用（≈0.0）→ novelty≈1.0 全新
    //   刚用过一次（≈0.2）→ novelty≈0.33 较新
    //   频繁使用（≈1.0）→ novelty≈0.09 很熟
    for (int t = 0; t < cc->master->sub_topo_count && t < MAX_SUBTOPOS; t++) {
        SubTopology* sub = cc->master->sub_topologies[t];
        if (!sub) continue;

        float recent = sub->recent_activation;
        novelty_factors[t] = 1.0f / (1.0f + 10.0f * recent);

        // 每轮结束后衰减 — 未被激活的拓扑 novelty 自然恢复
        sub->recent_activation *= 0.8f;
    }

    if (cc->novelty_bias > 0.001f) {
        printf("  [新颖性] ");
        for (int i = 0; i < MAX_SUBTOPOS && i < cc->master->sub_topo_count; i++) {
            if (fabsf(novelty_factors[i] - 1.0f) > 0.01f)
                printf("%s=%.3f ", TOPO_NAMES[i], novelty_factors[i]);
        }
        printf("\n");
    }
}

// ==================== 效价偏好 ====================

/**
 * 获取各子拓扑的全局效价。
 * 遍历子拓扑中的所有节点，求平均效价。
 * 正值 = 用户喜欢，负值 = 用户不喜欢。
 */
static void calc_valence_prefs(CognitiveController* cc,
                                float* valence_prefs) {
    if (!cc || !cc->master) {
        for (int i = 0; i < MAX_SUBTOPOS; i++)
            valence_prefs[i] = 1.0f;
        return;
    }

    for (int t = 0; t < cc->master->sub_topo_count && t < MAX_SUBTOPOS; t++) {
        SubTopology* sub = cc->master->sub_topologies[t];
        if (!sub || !sub->net || sub->net->node_count == 0) {
            valence_prefs[t] = 1.0f;
            continue;
        }

        float total_valence = 0.0f;
        int count = 0;
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (node) {
                total_valence += node->valence;
                count++;
            }
        }

        float avg = (count > 0) ? total_valence / count : 0.0f;
        // 效价偏置：正效价 → 偏好>1，负效价 → 偏好<1
        valence_prefs[t] = 1.0f + avg;  // avg ∈ [-1,1] → result ∈ [0,2]
        if (valence_prefs[t] < 0.1f) valence_prefs[t] = 0.1f;
    }
}

// ==================== 连贯性奖励 ====================

/**
 * 计算各子拓扑与上一轮回复的语义连贯性。
 * 上一轮回复中激活的节点，在本轮高加权。
 */
static void calc_coherence_bonus(CognitiveController* cc,
                                 float* coherence_bonuses) {
    if (!cc || !cc->master) {
        for (int i = 0; i < MAX_SUBTOPOS; i++)
            coherence_bonuses[i] = 1.0f;
        return;
    }

    // 收集所有可用的上下文文本：上轮回复 + 当前输入
    // 两者同时匹配时给予更强的连贯性奖励
    const char* contexts[2] = { cc->last_response, cc->current_input };
    int ctx_count = 0;
    for (int c = 0; c < 2; c++) {
        if (contexts[c] && contexts[c][0]) ctx_count++;
    }
    if (ctx_count == 0) {
        for (int i = 0; i < MAX_SUBTOPOS; i++)
            coherence_bonuses[i] = 1.0f;
        return;
    }

    for (int t = 0; t < cc->master->sub_topo_count && t < MAX_SUBTOPOS; t++) {
        SubTopology* sub = cc->master->sub_topologies[t];
        if (!sub || !sub->net || sub->net->node_count == 0) {
            coherence_bonuses[t] = 1.0f;
            continue;
        }

        int overlap = 0;
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (!node || !node->concept) continue;
            for (int c = 0; c < 2; c++) {
                if (!contexts[c] || !contexts[c][0]) continue;
                // 分词匹配（简单版：strstr 子串匹配）
                if (strstr(contexts[c], node->concept)) {
                    overlap++;
                    break;
                }
            }
        }

        float ratio = (sub->net->node_count > 0)
                      ? (float)overlap / sub->net->node_count
                      : 0.0f;
        coherence_bonuses[t] = 1.0f + ratio * 2.0f;
        if (coherence_bonuses[t] > 3.0f) coherence_bonuses[t] = 3.0f;
    }
}

// ==================== 意图向量计算 ====================

/**
 * 根据语义意图类型获取各拓扑的基准权重
 * 0=词汇 1=语义 2=情绪 3=语法 4=上下文 5=领域 6=语用 7=文化 8=概念
 */
static void get_intent_base_weights(int intent_type, float* base) {
    // 默认：偏重词汇和语义拓扑（与 cognitive_controller_create 一致）
    static const float default_base[MAX_SUBTOPOS] = {
        0.20f, 0.18f, 0.10f, 0.10f, 0.12f, 0.08f, 0.07f, 0.05f, 0.10f
    };
    memcpy(base, default_base, MAX_SUBTOPOS * sizeof(float));

    // 意图值: UNKNOWN=0 QUERY=1 EXPLAIN=2 COMPARE=3 DEFINE=4
    //         HOWTO=5 CHAT=6 LEARN=7 TEST=8 FEEDBACK=9
    switch (intent_type) {
        case 6: // INTENT_CHAT
            base[1] = 0.25f;  base[4] = 0.20f;  base[6] = 0.15f;
            base[0] = 0.12f;  base[2] = 0.10f;
            break;
        case 1: case 4: // INTENT_QUERY / INTENT_DEFINE
            base[0] = 0.25f;  base[8] = 0.20f;  base[1] = 0.15f;  base[5] = 0.12f;
            break;
        case 2: // INTENT_EXPLAIN
            base[8] = 0.25f;  base[1] = 0.20f;  base[0] = 0.15f;  base[7] = 0.10f;
            break;
        case 5: // INTENT_HOWTO
            base[6] = 0.25f;  base[0] = 0.20f;  base[4] = 0.15f;  base[5] = 0.12f;
            break;
        case 7: // INTENT_LEARN
            base[8] = 0.30f;  base[1] = 0.20f;  base[0] = 0.15f;
            break;
        case 3: // INTENT_COMPARE
            base[1] = 0.22f;  base[8] = 0.20f;  base[0] = 0.15f;  base[7] = 0.15f;
            break;
        default:
            break;
    }
}

// ==================== 意图基准在线学习 ====================
// LEARN_RATE 使用 cognitive_params.h 中的定义 (0.005f)

#define INTENT_BASE_FILE "intent_base.bin"

static void intent_base_save(CognitiveController* cc) {
    if (!cc) return;
    FILE* f = fopen(INTENT_BASE_FILE, "wb");
    if (f) {
        fwrite(cc->learned_base, sizeof(float), MAX_SUBTOPOS, f);
        fclose(f);
    }
}

static void intent_base_load(CognitiveController* cc) {
    if (!cc) return;
    FILE* f = fopen(INTENT_BASE_FILE, "rb");
    if (f) {
        size_t n = fread(cc->learned_base, sizeof(float), MAX_SUBTOPOS, f);
        fclose(f);
        if (n == MAX_SUBTOPOS) {
            printf("[在线学习] 已加载 learned_base (来自 %s)\n", INTENT_BASE_FILE);
        }
    }
    // 文件不存在 → 保持默认值（全1.0）
}

void intent_base_learn(CognitiveController* cc, const int* used_topos,
                       int topo_count, float feedback) {
    if (!cc || !used_topos || topo_count <= 0 || feedback <= 0.0f) return;

    // EMA 更新：趋近目标值 (1.0 + 0.5×feedback) × 活跃度
    // α × feedback 动态步长：反馈高时学得快，反馈低时微调
    float alpha = LEARN_RATE * feedback;
    if (alpha > 0.05f) alpha = 0.05f;  // 单步最大 5%

    float target = 1.0f + 0.5f * feedback;  // 激活拓扑的目标因子

    for (int i = 0; i < topo_count; i++) {
        int t = used_topos[i];
        if (t >= 0 && t < MAX_SUBTOPOS) {
            cc->learned_base[t] = (1.0f - alpha) * cc->learned_base[t] + alpha * target;
        }
    }

    // 归一化：保持平均 ≈1.0（使得 intent_base × learned_base 的乘积量级不变）
    float sum = 0.0f;
    for (int i = 0; i < MAX_SUBTOPOS; i++)
        sum += cc->learned_base[i];
    if (sum > 0.0f) {
        float scale = (float)MAX_SUBTOPOS / sum;
        for (int i = 0; i < MAX_SUBTOPOS; i++)
            cc->learned_base[i] *= scale;
    }

    // 持久化
    intent_base_save(cc);

    if (feedback > 0.5f) {
        printf("  [在线学习] α=%.4f 反馈=%.2f | 活跃拓扑: ", alpha, feedback);
        for (int i = 0; i < topo_count; i++)
            printf("%s ", TOPO_NAMES[used_topos[i]]);
        printf("→ learned_base 已更新\n");
    }
}

void compute_intent(CognitiveController* cc, const float* ctx_activations) {
    if (!cc) return;

    printf("\n[认知调度] 计算意图向量...\n");

    // 1. 从语义意图获取基准权重
    float intent_base[MAX_SUBTOPOS];
    get_intent_base_weights(cc->intent_type, intent_base);

    // 2. 计算调优因子
    float novelty[MAX_SUBTOPOS];
    float valence_p[MAX_SUBTOPOS];
    float coherence[MAX_SUBTOPOS];
    calc_novelty_factors(cc, novelty);
    calc_valence_prefs(cc, valence_p);
    calc_coherence_bonus(cc, coherence);

    // 3. 合成意图权重
    float total = 0.0f;
    for (int i = 0; i < MAX_SUBTOPOS; i++) {
        float base = intent_base[i] * cc->learned_base[i];  // 硬编码基准 × 在线学习因子
        float nf = 1.0f + cc->novelty_bias * (novelty[i] - 1.0f);
        float vf = 1.0f + cc->valence_bias * (valence_p[i] - 1.0f);
        float cf = 1.0f + (cc->coherence_target * 0.5f) * (coherence[i] - 1.0f);

        float ctx_f = ctx_activations
                        ? (1.0f + cc->context_bias * ctx_activations[i])
                        : 1.0f;
        float w = base * ctx_f * nf * vf * cf;

        cc->intent_weights[i] = w;
        total += w;

        printf("  [%s]  base=%.3f nov=%.3f val=%.3f coh=%.3f → raw=%.3f\n",
               TOPO_NAMES[i], base, novelty[i], valence_p[i], coherence[i], w);
    }

    // 4. 归一化
    if (total > 0.0f) {
        for (int i = 0; i < MAX_SUBTOPOS; i++) {
            cc->intent_weights[i] /= total;
        }
    } else {
        // 兜底：均匀分布
        for (int i = 0; i < MAX_SUBTOPOS; i++) {
            cc->intent_weights[i] = 1.0f / MAX_SUBTOPOS;
        }
    }

    printf("[认知调度] 意图向量: ");
    for (int i = 0; i < MAX_SUBTOPOS; i++) {
        if (cc->intent_weights[i] > 0.05f)
            printf("%s=%.3f ", TOPO_NAMES[i], cc->intent_weights[i]);
    }
    printf("\n");
}

// ==================== 因果路径筛选（中游：约束 walk 候选） ====================

/**
 * 因果一致性检查：路径中相邻概念对在因果图中的边强度
 *
 * 对路径中每对相邻节点 (n[i], n[i+1])：
 * 1. 获取概念名 → 在因果图中查找对应节点
 * 2. 检查是否存在因果边（正向/反向）
 * 3. 累积因果一致性评分
 *
 * @return 0-1，1 = 所有相邻对都有因果边支持
 */
float causal_path_score(CognitiveController* cc,
                        SubTopology* sub,
                        const int* node_ids,
                        int path_len) {
    if (!cc || !sub || !node_ids || path_len < 2) return 0.5f;
    CausalGraph* cg = (CausalGraph*)cc->causal_graph;
    if (!cg || cg->edge_count == 0) return 0.5f;  // 因果图为空，中性

    if (!sub->net) return 0.5f;

    // 构建反向映射 topo_id → cg_id（一次 O(n)，避免循环内 O(n²)）
    int max_topo_id = sub->net->node_count;
    int* topo_to_cg = (int*)calloc(max_topo_id, sizeof(int));
    if (!topo_to_cg) return 0.5f;
    for (int i = 0; i < max_topo_id; i++) topo_to_cg[i] = -1;
    for (int c = 0; c < cg->node_count; c++) {
        int tid = cg->node_mapping[c];
        if (tid >= 0 && tid < max_topo_id)
            topo_to_cg[tid] = c;
    }

    int pairs = 0;
    float total_strength = 0.0f;

    for (int i = 0; i < path_len - 1; i++) {
        int from_id = node_ids[i];
        int to_id = node_ids[i + 1];
        if (from_id < 0 || from_id >= max_topo_id) continue;
        if (to_id < 0 || to_id >= max_topo_id) continue;

        // O(1) 反向查找
        int cg_cause = topo_to_cg[from_id];
        int cg_effect = topo_to_cg[to_id];

        if (cg_cause >= 0 && cg_effect >= 0) {
            // 查找正向因果边
            CausalEdge* edge = get_causal_edge(cg, cg_cause, cg_effect);
            if (edge) {
                total_strength += edge->strength;
                pairs++;
                continue;
            }
            // 查找反向因果边
            edge = get_causal_edge(cg, cg_effect, cg_cause);
            if (edge) {
                total_strength += edge->strength * 0.5f;  // 反向减半
                pairs++;
            }
        }
    }

    float result;
    if (pairs == 0) {
        result = 0.35f;  // 因果图有边但未匹配 → 略低于中性
    } else {
        float avg = total_strength / pairs;
        result = avg < 0.0f ? 0.0f : (avg > 1.0f ? 1.0f : avg);
    }
    free(topo_to_cg);
    return result;
}

// ==================== 自矛盾检测 ====================

/**
 * 自矛盾检测：同一回复路径中是否存在互斥概念
 *
 * 1. 情绪矛盾：正效价词 + 负效价词同时出现
 * 2. 概念跳跃：路径中相邻概念在概念层次中跨层 > 3 层
 *
 * @return 0-1，1 = 无矛盾
 */
static float self_contradiction_check(CognitiveController* cc,
                                      const PathResult* draft) {
    if (!cc || !draft || draft->length < 2) return 1.0f;

    SubTopology* sub = NULL;
    if (cc->master && draft->topo_id >= 0 &&
        draft->topo_id < cc->master->sub_topo_count) {
        sub = cc->master->sub_topologies[draft->topo_id];
    }
    if (!sub || !sub->net) return 1.0f;

    float penalty = 0.0f;
    int checks = 0;

    // 1. 情绪矛盾检测
    int has_positive = 0, has_negative = 0;
    for (int i = 0; i < draft->length; i++) {
        int nid = draft->node_ids[i];
        if (nid < 0 || nid >= sub->net->node_count) continue;
        ReasoningNode* node = sub->net->nodes[nid];
        if (!node) continue;
        if (node->valence > 0.3f) has_positive = 1;
        if (node->valence < -0.3f) has_negative = 1;
    }
    checks++;
    if (has_positive && has_negative) {
        penalty += 0.4f;  // 明显的情绪矛盾
    }

    // 2. 概念层次跳跃检测（如果概念层次可用）
    ConceptHierarchy* ch = (ConceptHierarchy*)cc->concept_hierarchy;
    if (ch && ch->node_count > 0) {
        for (int i = 0; i < draft->length - 1; i++) {
            int from_id = draft->node_ids[i];
            int to_id = draft->node_ids[i + 1];
            if (from_id < 0 || from_id >= sub->net->node_count) continue;
            if (to_id < 0 || to_id >= sub->net->node_count) continue;
            ReasoningNode* fn = sub->net->nodes[from_id];
            ReasoningNode* tn = sub->net->nodes[to_id];
            if (!fn || !fn->concept || !tn || !tn->concept) continue;

            // 按名称在概念层次中查找
            int level_from = -1, level_to = -1;
            for (int ci = 0; ci < ch->node_count; ci++) {
                ConceptNode* cn = ch->nodes[ci];
                if (!cn || !cn->name) continue;
                if (level_from < 0 && strcmp(cn->name, fn->concept) == 0)
                    level_from = (int)cn->level;
                if (level_to < 0 && strcmp(cn->name, tn->concept) == 0)
                    level_to = (int)cn->level;
                if (level_from >= 0 && level_to >= 0) break;
            }

            if (level_from >= 0 && level_to >= 0) {
                int gap = abs(level_from - level_to);
                checks++;
                if (gap > PM_CONCEPT_JUMP_LIMIT) {
                    penalty += 0.15f * (gap - PM_CONCEPT_JUMP_LIMIT);  // 跨度越大惩罚越重
                }
            }
        }
    }

    if (penalty > 1.0f) penalty = 1.0f;
    return 1.0f - penalty;
}

// ==================== 内感受评估（下游：内驱力检验） ====================

/**
 * 内感受评估 — 检查路径是否满足系统当前内驱力
 *
 * 效价不是评分维度，而是驱动力的综合表征：
 * - 好奇驱动高 → 偏好新颖路径
 * - 舒适驱动高 → 偏好连贯熟悉路径
 * - 社交驱动高 → 偏好情绪积极的路径
 *
 * 因果/语义约束已上移到 walk 阶段（causal_path_score），
 * 不在 evaluate 中重复评估。
 */
float evaluate_draft(CognitiveController* cc,
                     const PathResult* draft,
                     int draft_len) {
    if (!cc || !draft || draft_len <= 0) return 0.0f;

    // 1. 路径基础质量：激活充足性
    float activation_score = (draft->length > 0)
                             ? draft->act_sum / draft->length
                             : 0.0f;
    if (activation_score > 1.0f) activation_score = 1.0f;

    // 2. 拓扑连贯性：路径内相邻节点连接强度
    float coherence_score = 0.5f;
    if (cc->master && draft->length >= 2) {
        SubTopology* sub = NULL;
        if (draft->topo_id >= 0 && draft->topo_id < cc->master->sub_topo_count)
            sub = cc->master->sub_topologies[draft->topo_id];
        if (sub && sub->net) {
            int count = 0;
            float total_weight = 0.0f;
            for (int i = 0; i < draft->length - 1; i++) {
                int from = draft->node_ids[i];
                int to = draft->node_ids[i + 1];
                if (from >= 0 && from < sub->net->node_count &&
                    to >= 0 && to < sub->net->node_count) {
                    ReasoningNode* fn = sub->net->nodes[from];
                    if (fn) {
                        for (int c = 0; c < fn->connection_count; c++) {
                            ReasoningNode* conn = fn->connections[c];
                            if (conn && conn->node_id == to) {
                                total_weight += fn->connection_weights[c];
                                count++;
                                break;
                            }
                        }
                    }
                }
            }
            coherence_score = (count > 0) ? total_weight / count : 0.3f;
        }
    }

    // 3. 效价（内驱力检验）— 保留符号信息，映射到 [0,1]
    float drive_score = 0.5f;
    if (draft && draft->length > 0 && cc->master) {
        SubTopology* sub = NULL;
        if (draft->topo_id >= 0 && draft->topo_id < cc->master->sub_topo_count)
            sub = cc->master->sub_topologies[draft->topo_id];
        if (sub && sub->net) {
            float val_sum = 0.0f;
            int val_count = 0;
            for (int i = 0; i < draft->length; i++) {
                int nid = draft->node_ids[i];
                if (nid >= 0 && nid < sub->net->node_count) {
                    ReasoningNode* node = sub->net->nodes[nid];
                    if (node) {
                        val_sum += (node->valence + 1.0f) / 2.0f;  // 符号保留，映射到[0,1]
                        val_count++;
                    }
                }
            }
            if (val_count > 0) {
                drive_score = val_sum / val_count;
            }
        }
    }

    // 4. 自矛盾检测 — 扣分项
    float contradict_penalty = 1.0f - self_contradiction_check(cc, draft);

    // 综合评分：激活(25%) + 连贯(35%) + 效价(25%) + 语义(15%) - 矛盾扣分
    // 语义对齐：路径与 query_anchor 的一致性（暂用激活+连贯替代，后续接入）
    float satisfaction = 0.25f * activation_score
                       + 0.35f * coherence_score
                       + 0.25f * drive_score;
    satisfaction += activation_score * coherence_score * 0.15f;  // 交叉项替代语义维度
    satisfaction -= contradict_penalty * 0.25f;
    if (satisfaction < 0.0f) satisfaction = 0.0f;
    if (satisfaction > 1.0f) satisfaction = 1.0f;

    printf("[内感受] 质量=%.3f 连贯=%.3f 效价=%.3f 矛盾=-%.3f → 满意度=%.3f (阈值=%.2f)\n",
           activation_score, coherence_score, drive_score, contradict_penalty,
           satisfaction, cc->satisfaction_threshold);

    // ═══ 回路1: 效价回流（多巴胺标记）═══
    // 满意度回写到路径节点的valence，满意→正标记，不满意→负标记
    // 下次走边时自然倾向正标记多的方向
    {
        float val_delta = (satisfaction - 0.5f) * 0.05f;  // ±0.025 max per eval
        SubTopology* val_sub = NULL;
        if (cc->master && draft->topo_id >= 0 && draft->topo_id < cc->master->sub_topo_count)
            val_sub = cc->master->sub_topologies[draft->topo_id];
        if (val_sub && val_sub->net) {
            for (int vi = 0; vi < draft->length; vi++) {
                int nid = draft->node_ids[vi];
                if (nid >= 0 && nid < val_sub->net->node_count) {
                    ReasoningNode* vn = val_sub->net->nodes[nid];
                    if (vn) {
                        vn->valence += val_delta;
                        if (vn->valence > 1.0f) vn->valence = 1.0f;
                        if (vn->valence < -1.0f) vn->valence = -1.0f;
                    }
                }
            }
        }
    }

    // ═══ 回路3: cognitive_confidence EMA更新 ═══
    // 三维置信度: predictive_accuracy, user_satisfaction, novelty_bonus
    // 每次评估后用satisfaction做指数移动平均更新
    {
        SubTopology* cf_sub = NULL;
        if (cc->master && draft->topo_id >= 0 && draft->topo_id < cc->master->sub_topo_count)
            cf_sub = cc->master->sub_topologies[draft->topo_id];
        if (cf_sub && cf_sub->net) {
            for (int ci = 0; ci < draft->length; ci++) {
                int nid = draft->node_ids[ci];
                if (nid >= 0 && nid < cf_sub->net->node_count) {
                    ReasoningNode* cn = cf_sub->net->nodes[nid];
                    if (cn && cn->cognitive_confidence) {
                        // EMA: 90%保留旧值 + 10%吸收新值
                        float new_predictive = (satisfaction >= cc->satisfaction_threshold) ? 0.9f : 0.3f;
                        float new_satisfaction = satisfaction;
                        float new_novelty = (cn->selection_count < 5) ? 0.8f : 0.4f;
                        cognitive_confidence_update(cn->cognitive_confidence,
                            cn->cognitive_confidence->predictive_accuracy * 0.9f + new_predictive * 0.1f,
                            cn->cognitive_confidence->user_satisfaction * 0.9f + new_satisfaction * 0.1f,
                            cn->cognitive_confidence->novelty_bonus      * 0.9f + new_novelty      * 0.1f);
                    }
                }
            }
        }
    }

    return satisfaction;
}

// ==================== 修正向量 ====================

void compute_correction_vector(CognitiveController* cc,
                               const PathResult* draft,
                               float satisfaction,
                               float* correction) {
    if (!cc || !correction) return;

    // 对所有子拓扑初始化为 0
    memset(correction, 0, sizeof(float) * MAX_SUBTOPOS);

    float deficit = cc->satisfaction_threshold - satisfaction;
    if (deficit <= 0) return;  // 已达标，不需要修正

    // 如果满意度低，找出问题出在哪个子拓扑
    // 简版：压低了产生当前路径的子拓扑的权重
    //       提升了其他可能提供替代路径的子拓扑的权重
    if (draft && draft->topo_id >= 0 && draft->topo_id < MAX_SUBTOPOS) {
        // 压低下这个路径所属的子拓扑
        correction[draft->topo_id] = -deficit;

        // 提升其他子拓扑（均分）
        float boost = deficit / (MAX_SUBTOPOS - 1);
        for (int i = 0; i < MAX_SUBTOPOS; i++) {
            if (i != draft->topo_id) {
                correction[i] += boost;
            }
        }

        printf("[修正向量] 压 %s(%.2f) → 提升其他 (+%.3f each)\n",
               TOPO_NAMES[draft->topo_id], -deficit, boost);
    }
}

// ==================== 负反馈修正 ====================

RetryStatus revise_and_retry(CognitiveController* cc,
                             const PathResult* draft,
                             float satisfaction) {
    if (!cc) return RETRY_FAILED;

    if (satisfaction >= cc->satisfaction_threshold) {
        printf("[认知调度] ✓ 满意，无需修正\n");
        return RETRY_OK;  // 通过
    }

    if (cc->retry_count >= cc->max_retry) {
        printf("[认知调度] ! 已达最大修正次数(%d)，强制输出\n", cc->max_retry);
        return RETRY_FAILED;  // 已达上限
    }

    // 生成修正向量
    float correction[MAX_SUBTOPOS];
    compute_correction_vector(cc, draft, satisfaction, correction);

    // 回写意图权重
    for (int i = 0; i < MAX_SUBTOPOS; i++) {
        cc->intent_weights[i] *= (1.0f + cc->correction_strength * correction[i]);
    }

    // 归一化
    float total = 0.0f;
    for (int i = 0; i < MAX_SUBTOPOS; i++) total += cc->intent_weights[i];
    if (total > 0.0f) {
        for (int i = 0; i < MAX_SUBTOPOS; i++) {
            cc->intent_weights[i] /= total;
        }
    }

    cc->retry_count++;

    // 三级降级策略
    RetryStatus status;
    if (cc->retry_count == 1) {
        // 第1次修正：从候选路径池重排（不重搜）
        status = RETRY_FROM_POOL;
    } else if (cc->retry_count == 2) {
        // 第2次修正：缩域重搜
        status = RETRY_WITH_SEARCH;
    } else {
        // 第3次修正：强制输出
        status = RETRY_FAILED;
    }

    printf("[认知调度] 第 %d 次修正: 满意度 %.3f < %.3f → %s\n",
           cc->retry_count, satisfaction, cc->satisfaction_threshold,
           status == RETRY_FROM_POOL ? "重排" :
           status == RETRY_WITH_SEARCH ? "重搜" : "强制输出");
    printf("[认知调度] 修正后意图: ");
    for (int i = 0; i < MAX_SUBTOPOS; i++) {
        if (cc->intent_weights[i] > 0.05f)
            printf("%s=%.3f ", TOPO_NAMES[i], cc->intent_weights[i]);
    }
    printf("\n");

    return status;
}

// ==================== 候选路径池 ====================

void pool_save_path(CognitiveController* cc, int topo_idx,
                    const PathResult* path) {
    if (!cc || !path || topo_idx < 0 || topo_idx >= MAX_SUBTOPOS) return;
    int idx = cc->pool_counts[topo_idx];
    if (idx >= PATH_POOL_SIZE) return;  // 池满

    cc->path_pool[topo_idx][idx] = *path;
    cc->pool_counts[topo_idx]++;
}

int pool_select_best(CognitiveController* cc, int topo_idx,
                     PathResult* out) {
    if (!cc || !out || topo_idx < 0 || topo_idx >= MAX_SUBTOPOS) return 0;
    if (cc->pool_counts[topo_idx] == 0) return 0;

    // 在当前意图权重下重新计算池中路径的评分
    float intent_w = cc->intent_weights[topo_idx];
    int best_idx = 0;
    float best_score = -1.0f;

    for (int i = 0; i < cc->pool_counts[topo_idx]; i++) {
        PathResult* p = &cc->path_pool[topo_idx][i];
        // 重评分：原来的 score 乘以当前意图权重
        float re_score = p->score * intent_w;
        if (re_score > best_score) {
            best_score = re_score;
            best_idx = i;
        }
    }

    *out = cc->path_pool[topo_idx][best_idx];
    return 1;
}

// ==================== 快照 ====================

void cognitive_controller_snapshot(CognitiveController* cc, float satisfaction) {
    if (!cc) return;
    memcpy(cc->prev_intent_weights, cc->intent_weights, sizeof(float) * MAX_SUBTOPOS);
    cc->prev_satisfaction = satisfaction;
}

// ==================== 路径观察与概念涌现 ====================

// 获取子拓扑中的节点
static ReasoningNode* cc_get_node(SubTopology* sub, int node_id) {
    if (!sub || !sub->net) return NULL;
    if (node_id < 0 || node_id >= sub->net->node_count) return NULL;
    return sub->net->nodes[node_id];
}

// 获取节点概念名
static const char* cc_node_name(SubTopology* sub, int node_id) {
    ReasoningNode* n = cc_get_node(sub, node_id);
    return n ? n->concept : NULL;
}

// 拼接多字符概念名
static char* cc_join_names(SubTopology* sub, const int* ids, int len) {
    char buf[256] = {0};
    int pos = 0;
    for (int i = 0; i < len && pos < 250; i++) {
        const char* name = cc_node_name(sub, ids[i]);
        if (name) {
            int nlen = strlen(name);
            if (pos + nlen < 250) {
                memcpy(buf + pos, name, nlen);
                pos += nlen;
            }
        }
    }
    return strdup(buf);
}

// 计算序列内部平均边强度
static float cc_avg_edge_strength(SubTopology* sub, const int* ids, int len) {
    if (len < 2) return 0.0f;
    float total = 0.0f;
    int count = 0;
    for (int i = 0; i < len - 1; i++) {
        ReasoningNode* from = cc_get_node(sub, ids[i]);
        if (!from) continue;
        int to_id = ids[i + 1];
        for (int c = 0; c < from->connection_count; c++) {
            ReasoningNode* t = from->connections[c];
            if (t && t->node_id == to_id) {
                total += from->connection_weights[c];
                count++;
                break;
            }
        }
    }
    return count > 0 ? total / count : 0.0f;
}

void cognitive_controller_observe_path(CognitiveController* cc,
                                        int topo_type,
                                        const int* node_ids,
                                        int path_len) {
    if (!cc || !node_ids || path_len < 2) return;
    if (path_len > CC_PATH_MAX_LEN) path_len = CC_PATH_MAX_LEN;

    int cur = cc->path_buf_cursor;
    memcpy(cc->path_buf_nodes[cur], node_ids, path_len * sizeof(int));
    cc->path_buf_lens[cur] = path_len;
    cc->path_buf_topo[cur] = topo_type;

    cc->path_buf_cursor = (cur + 1) % CC_PATH_BUF_SIZE;
    if (cc->path_buf_count < CC_PATH_BUF_SIZE) cc->path_buf_count++;

    // 周期扫描触发器
    cc->scan_counter++;
    if (cc->scan_counter >= 50) {
        cc->scan_counter = 0;
        cognitive_controller_scan_patterns(cc);
    }
}

// 创建复合节点（概念拓扑中）
static int cc_create_composite(CognitiveController* cc,
                                const int* node_ids, int len,
                                int topo_type) {
    if (!cc || !cc->master || len < 2) return -1;

    SubTopology* vocab = master_get_sub_topology_by_type(cc->master, (TopologyType)topo_type);
    SubTopology* concept = master_get_sub_topology_by_type(cc->master, TOPO_CONCEPT);
    if (!vocab || !concept || !concept->net) return -1;

    // 拼接名称
    char* comp_name = cc_join_names(vocab, node_ids, len);
    if (!comp_name) return -1;

    // 查重
    if (concept->net && concept->node_hash) {
        if (node_hash_find(concept->node_hash, comp_name)) {
            ReasoningNode* existing = node_hash_find(concept->node_hash, comp_name);
            if (existing) { free(comp_name); return existing->node_id; }
        }
    }

    // 创建复合节点
    ReasoningNode* composite = huarong_net_add_node(
        concept->net, comp_name, NULL, 0);
    free(comp_name);
    if (!composite) return -1;
    composite->confidence = 1.0f;
    composite->activation = 0.5f;

    // 找词汇拓扑和概念拓扑的对应ID
    int vocab_topo_id = -1;
    for (int t = 0; t < cc->master->sub_topo_count; t++) {
        if (cc->master->sub_topologies[t] &&
            cc->master->sub_topologies[t]->type == TOPO_VOCABULARY) {
            vocab_topo_id = t;
            break;
        }
    }
    int concept_topo_id = -1;
    for (int t = 0; t < cc->master->sub_topo_count; t++) {
        if (cc->master->sub_topologies[t] &&
            cc->master->sub_topologies[t]->type == TOPO_CONCEPT) {
            concept_topo_id = t;
            break;
        }
    }

    if (vocab_topo_id >= 0 && concept_topo_id >= 0) {
        // 从第一个字符 → 复合节点
        master_add_cross_link(cc->master,
            vocab_topo_id, node_ids[0],
            concept_topo_id, composite->node_id,
            0.8f, "composes");

        // 从复合节点 → 继承最后一个字的最强出边
        ReasoningNode* last = cc_get_node(vocab, node_ids[len - 1]);
        if (last) {
            for (int c = 0; c < last->connection_count && c < 10; c++) {
                ReasoningNode* target = last->connections[c];
                if (!target) continue;
                float w = last->connection_weights[c];
                if (w > 0.3f) {
                    master_add_cross_link(cc->master,
                        concept_topo_id, composite->node_id,
                        vocab_topo_id, target->node_id,
                        w * (cc->composite_boost > 0 ? cc->composite_boost : 1.1f),
                        "continues");
                }
            }
        }
    }

    printf("[认知调度·概念涌现] 创建复合节点 '%s'(ID=%d, %d字)\n",
           composite->concept, composite->node_id, len);
    return composite->node_id;
}

int cognitive_controller_scan_patterns(CognitiveController* cc) {
    if (!cc || cc->path_buf_count < 5) return 0;
    if (!cc->patterns) {
        cc->pattern_capacity = 256;
        cc->patterns = calloc(cc->pattern_capacity, sizeof(*cc->patterns));
    }

    if (cc->min_pattern_freq <= 0) cc->min_pattern_freq = 8;
    if (cc->min_edge_strength <= 0) cc->min_edge_strength = 0.4f;

    int created = 0;

    // 扫描 2-gram
    for (int b = 0; b < cc->path_buf_count; b++) {
        int len = cc->path_buf_lens[b];
        int topo = cc->path_buf_topo[b];
        int* nodes = cc->path_buf_nodes[b];

        for (int s = 0; s < len - 1; s++) {
            int from = nodes[s], to = nodes[s + 1];
            if (from < 0 || to < 0) continue;

            // 在已有模式中找
            int found = -1;
            for (int p = 0; p < cc->pattern_count; p++) {
                if (cc->patterns[p].length == 2 &&
                    cc->patterns[p].node_ids[0] == from &&
                    cc->patterns[p].node_ids[1] == to) {
                    found = p;
                    break;
                }
            }

            if (found < 0) {
                // 新增模式
                if (cc->pattern_count >= cc->pattern_capacity) {
                    cc->pattern_capacity *= 2;
                    cc->patterns = realloc(cc->patterns,
                                           cc->pattern_capacity * sizeof(*cc->patterns));
                    memset(&cc->patterns[cc->pattern_count], 0,
                           (cc->pattern_capacity - cc->pattern_count) * sizeof(*cc->patterns));
                }
                found = cc->pattern_count++;
                cc->patterns[found].node_ids = (int*)malloc(4 * sizeof(int));
                cc->patterns[found].node_ids[0] = from;
                cc->patterns[found].node_ids[1] = to;
                cc->patterns[found].length = 2;
                cc->patterns[found].count = 0;
                cc->patterns[found].composite_id = -1;
            }

            // 如果是词汇拓扑的路径才统计
            SubTopology* sub = master_get_sub_topology_by_type(cc->master, (TopologyType)topo);
            if (sub && sub->type == TOPO_VOCABULARY) {
                cc->patterns[found].count++;
            }
        }
    }

    // 检查哪些 2-gram 达到阈值
    SubTopology* vocab = master_get_sub_topology_by_type(cc->master, TOPO_VOCABULARY);
    if (!vocab) return 0;

    for (int p = 0; p < cc->pattern_count; p++) {
        if (cc->patterns[p].length != 2) continue;
        if (cc->patterns[p].composite_id >= 0) continue;
        if (cc->patterns[p].count < cc->min_pattern_freq) continue;

        float edge_str = cc_avg_edge_strength(
            vocab, cc->patterns[p].node_ids, 2);
        if (edge_str < cc->min_edge_strength) continue;

        cc->patterns[p].avg_edge_strength = edge_str;
        cc->patterns[p].composite_id = cc_create_composite(
            cc, cc->patterns[p].node_ids, 2, TOPO_VOCABULARY);
        if (cc->patterns[p].composite_id >= 0) created++;

        // 检查 3-gram 扩展
        for (int b = 0; b < cc->path_buf_count; b++) {
            int len = cc->path_buf_lens[b];
            int* nodes = cc->path_buf_nodes[b];
            for (int s = 0; s < len - 2; s++) {
                if (nodes[s] == cc->patterns[p].node_ids[0] &&
                    nodes[s+1] == cc->patterns[p].node_ids[1]) {
                    // 找到第三个字的候选
                    int third = nodes[s+2];
                    int seq3[3] = {cc->patterns[p].node_ids[0],
                                   cc->patterns[p].node_ids[1], third};

                    // 检查是否已有此 3-gram
                    int found3 = -1;
                    for (int q = 0; q < cc->pattern_count; q++) {
                        if (cc->patterns[q].length != 3) continue;
                        if (cc->patterns[q].node_ids[0] == seq3[0] &&
                            cc->patterns[q].node_ids[1] == seq3[1] &&
                            cc->patterns[q].node_ids[2] == seq3[2]) {
                            found3 = q;
                            break;
                        }
                    }

                    if (found3 < 0) {
                        if (cc->pattern_count >= cc->pattern_capacity) {
                            cc->pattern_capacity *= 2;
                            cc->patterns = realloc(cc->patterns,
                                                   cc->pattern_capacity * sizeof(*cc->patterns));
                            memset(&cc->patterns[cc->pattern_count], 0,
                                   (cc->pattern_capacity - cc->pattern_count) * sizeof(*cc->patterns));
                        }
                        int q = cc->pattern_count++;
                        cc->patterns[q].node_ids = (int*)malloc(4 * sizeof(int));
                        cc->patterns[q].node_ids[0] = seq3[0];
                        cc->patterns[q].node_ids[1] = seq3[1];
                        cc->patterns[q].node_ids[2] = seq3[2];
                        cc->patterns[q].length = 3;
                        cc->patterns[q].count = 0;
                        cc->patterns[q].composite_id = -1;
                    }

                    // 统计频率
                    int idx = found3 < 0 ? cc->pattern_count - 1 : found3;
                    cc->patterns[idx].count++;
                }
            }
        }
    }

    if (created > 0) {
        printf("[认知调度·概念涌现] 本轮创建 %d 个复合节点 (共 %d 模式)\n",
               created, cc->pattern_count);
    }

    // 清理路径缓冲（滚动刷新）
    if (cc->path_buf_count >= CC_PATH_BUF_SIZE) {
        // 只保留最近的一半
        int keep = CC_PATH_BUF_SIZE / 2;
        int start = (cc->path_buf_cursor - keep + CC_PATH_BUF_SIZE) % CC_PATH_BUF_SIZE;
        for (int i = 0; i < keep; i++) {
            int src = (start + i) % CC_PATH_BUF_SIZE;
            memcpy(cc->path_buf_nodes[i], cc->path_buf_nodes[src],
                   CC_PATH_MAX_LEN * sizeof(int));
            cc->path_buf_lens[i] = cc->path_buf_lens[src];
            cc->path_buf_topo[i] = cc->path_buf_topo[src];
        }
        cc->path_buf_count = keep;
        cc->path_buf_cursor = keep % CC_PATH_BUF_SIZE;
    }

    // 处理动态跨拓扑建边（连续5次命中自动建边, 100轮过期重置）
    cc->master->cross_hit_round++;
    int cross_created = master_process_cross_hits(cc->master, 5, 100);
    if (cross_created > 0) {
        printf("[CognitiveController] 动态创建 %d 条跨拓扑边\n", cross_created);
    }

    return created + cross_created;
}

int cognitive_controller_pattern_count(CognitiveController* cc) {
    if (!cc) return 0;
    int count = 0;
    for (int i = 0; i < cc->pattern_count; i++) {
        if (cc->patterns[i].composite_id >= 0) count++;
    }
    return count;
}
