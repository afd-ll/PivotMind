/**
 * @file cognitive_controller.c
 * @brief 认知调度中心实现
 */

#include "cognitive_controller.h"
#include "node_hash.h"
#include "utf8_tokenizer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ==================== 辅助函数 ====================

static const char* TOPO_NAMES[] = {
    "词汇", "语义", "情绪", "语法",
    "上下文", "领域", "语用", "文化",
    "概念", "主拓扑"
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
    cc->satisfaction_threshold = 0.5f;
    cc->max_retry = MAX_RETRY;
    cc->correction_strength = 0.3f;
    cc->retry_count = 0;

    // 初始意图向量：均匀分布
    for (int i = 0; i < MAX_SUBTOPOS; i++) {
        cc->intent_weights[i] = 1.0f / MAX_SUBTOPOS;
        cc->prev_intent_weights[i] = cc->intent_weights[i];
    }
    cc->prev_satisfaction = 0.0f;

    printf("[认知调度] 创建成功\n");
    return cc;
}

void cognitive_controller_destroy(CognitiveController* cc) {
    if (!cc) return;
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

// ==================== 上下文关联度计算 ====================

/**
 * 估算各个子拓扑与当前输入的上下文关联度。
 * 简版：在词汇/语义/情绪拓扑中匹配输入token，
 * 返回各子拓扑的活跃节点比例。
 */
static void calc_context_activations(CognitiveController* cc,
                                     float* ctx_activations) {
    if (!cc || !cc->master) return;
    MasterTopology* m = cc->master;

    // 先对输入分词
    char* tokens[64];
    int tok_count = 0;
    if (cc->current_input) {
        tok_count = utf8_tokenize(cc->current_input, tokens, 64);
    }

    for (int t = 0; t < m->sub_topo_count && t < MAX_SUBTOPOS; t++) {
        SubTopology* sub = m->sub_topologies[t];
        if (!sub || !sub->net || sub->net->node_count == 0) {
            ctx_activations[t] = 0.0f;
            continue;
        }

        int match_count = 0;
        // 统计此子拓扑中与输入token匹配的节点比例
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (!node || !node->concept) continue;
            // 模糊匹配：节点的概念包含在输入中，或输入包含节点概念
            for (int i = 0; i < tok_count; i++) {
                if (strstr(cc->current_input, node->concept) ||
                    strstr(node->concept, cc->current_input)) {
                    match_count++;
                    break;
                }
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
    if (!cc || !cc->memory) {
        for (int i = 0; i < MAX_SUBTOPOS; i++)
            novelty_factors[i] = 1.0f;
        return;
    }

    // 简版：从记忆中查找最近5条回复，统计各子拓扑出现频率
    // 由于当前 memory 系统没有按子拓扑维度存储的机制，
    // 暂用均匀新颖性，后续可以逐步细化
    for (int i = 0; i < MAX_SUBTOPOS; i++) {
        novelty_factors[i] = 1.0f;
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
    if (!cc || !cc->last_response || !cc->master) {
        for (int i = 0; i < MAX_SUBTOPOS; i++)
            coherence_bonuses[i] = 1.0f;
        return;
    }

    // 对上一轮回复分词
    char* tokens[64];
    int tok_count = utf8_tokenize(cc->last_response, tokens, 64);

    for (int t = 0; t < cc->master->sub_topo_count && t < MAX_SUBTOPOS; t++) {
        SubTopology* sub = cc->master->sub_topologies[t];
        if (!sub || !sub->net || sub->net->node_count == 0) {
            coherence_bonuses[t] = 1.0f;
            continue;
        }

        // 检查上轮回复中出现的概念在此子拓扑中的比例
        int overlap = 0;
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (!node || !node->concept) continue;
            for (int i = 0; i < tok_count; i++) {
                if (strcmp(node->concept, tokens[i]) == 0) {
                    overlap++;
                    break;
                }
            }
        }

        float ratio = (sub->net->node_count > 0)
                      ? (float)overlap / sub->net->node_count
                      : 0.0f;
        // 有重叠 → 加分
        coherence_bonuses[t] = 1.0f + ratio * 2.0f;
        if (coherence_bonuses[t] > 3.0f) coherence_bonuses[t] = 3.0f;
    }

    for (int i = 0; i < tok_count; i++) {
        free(tokens[i]);
    }
}

// ==================== 意图向量计算 ====================

void compute_intent(CognitiveController* cc, const float* ctx_activations) {
    if (!cc) return;

    printf("\n[认知调度] 计算意图向量...\n");

    float ctx[MAX_SUBTOPOS];
    float novelty[MAX_SUBTOPOS];
    float valence_p[MAX_SUBTOPOS];
    float coherence[MAX_SUBTOPOS];

    // 1. 计算各分量
    if (ctx_activations) {
        memcpy(ctx, ctx_activations, sizeof(float) * MAX_SUBTOPOS);
    } else {
        calc_context_activations(cc, ctx);
    }
    calc_novelty_factors(cc, novelty);
    calc_valence_prefs(cc, valence_p);
    calc_coherence_bonus(cc, coherence);

    // 2. 合成意图权重
    float total = 0.0f;
    for (int i = 0; i < MAX_SUBTOPOS; i++) {
        float base = ctx[i];
        float nf = 1.0f + cc->novelty_bias * (novelty[i] - 1.0f);
        float vf = 1.0f + cc->valence_bias * (valence_p[i] - 1.0f);
        float cf = 1.0f + (cc->coherence_target * 0.5f) * (coherence[i] - 1.0f);

        // 核心公式：base × novelty × valence × coherence
        // 加上上下文偏置的混合
        float w = (cc->context_bias * base + (1.0f - cc->context_bias) * 0.1f)
                  * nf * vf * cf;

        cc->intent_weights[i] = w;
        total += w;

        printf("  [%s]  ctx=%.3f nov=%.3f val=%.3f coh=%.3f → raw=%.3f\n",
               TOPO_NAMES[i], ctx[i], novelty[i], valence_p[i], coherence[i], w);
    }

    // 3. 归一化
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

// ==================== 内感受评估 ====================

float evaluate_draft(CognitiveController* cc,
                     const PathResult* draft,
                     int draft_len) {
    if (!cc || !draft || draft_len <= 0) return 0.0f;

    // 评估维度：
    // 1. 效价评分：使用情绪拓扑的平均效价
    // 2. 连贯性评分：路径内节点之间的连接强度
    // 3. 激活充足性：路径的平均激活值

    float valence_score = 0.0f;
    float coherence_score = 0.0f;
    float activation_score = 0.0f;

    // 1. 效价
    if (cc->master) {
        SubTopology* emotion = master_get_sub_topology_by_type(
            cc->master, TOPO_EMOTION);
        if (emotion && emotion->net && emotion->net->node_count > 0) {
            float sum = 0.0f;
            for (int n = 0; n < emotion->net->node_count; n++) {
                ReasoningNode* node = emotion->net->nodes[n];
                if (node) sum += node->valence;
            }
            valence_score = (sum / emotion->net->node_count + 1.0f) / 2.0f; // [-1,1] → [0,1]
        } else {
            valence_score = 0.5f; // 默认中性
        }

        // 2. 连贯性：看路径中相邻节点的连接权重
        if (draft->length >= 2) {
            int count = 0;
            float total_weight = 0.0f;
            // 查找每个子拓扑中相邻节点间的连接
            SubTopology* sub = NULL;
            if (draft->topo_id >= 0 &&
                draft->topo_id < cc->master->sub_topo_count) {
                sub = cc->master->sub_topologies[draft->topo_id];
            }
            if (sub && sub->net) {
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
            }
            coherence_score = (count > 0) ? total_weight / count : 0.3f;
        } else {
            coherence_score = 0.5f;
        }
    }

    // 3. 激活充足性
    activation_score = (draft->length > 0)
                       ? draft->act_sum / draft->length
                       : 0.0f;
    if (activation_score > 1.0f) activation_score = 1.0f;

    // 综合评分：效价 30% + 连贯性 40% + 激活 30%
    float satisfaction = 0.3f * valence_score
                       + 0.4f * coherence_score
                       + 0.3f * activation_score;

    printf("[内感受] 效价=%.3f 连贯=%.3f 激活=%.3f → 满意度=%.3f (阈值=%.2f)\n",
           valence_score, coherence_score, activation_score, satisfaction,
           cc->satisfaction_threshold);

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
        cc->patterns = (typeof(cc->patterns))
            calloc(cc->pattern_capacity, sizeof(*cc->patterns));
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
                    cc->patterns = (typeof(cc->patterns))
                        realloc(cc->patterns, cc->pattern_capacity * sizeof(*cc->patterns));
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
                            cc->patterns = (typeof(cc->patterns))
                                realloc(cc->patterns, cc->pattern_capacity * sizeof(*cc->patterns));
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

    return created;
}

int cognitive_controller_pattern_count(CognitiveController* cc) {
    if (!cc) return 0;
    int count = 0;
    for (int i = 0; i < cc->pattern_count; i++) {
        if (cc->patterns[i].composite_id >= 0) count++;
    }
    return count;
}
