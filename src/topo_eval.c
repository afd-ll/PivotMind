/**
 * @file topo_eval.c
 * @brief 拓扑训练评估指标体系实现
 *
 * 为玄枢 (PivotMind) 纯C认知AI框架设计的完整拓扑评估体系。
 * 包含训练时、推理时、长期、在线运行时四大类指标。
 *
 * 设计原则：
 * - 零改动已有代码（仅读取结构体字段）
 * - 复用已有的 calculate_semantic_similarity, cosine_similarity 等函数
 * - 每个指标独立，按需调用
 */
#include "topo_eval.h"
#include "common.h"
#include "associative_reasoning.h"
#include "node_hash.h"
#include "utf8_tokenizer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

// ==================== 内部辅助函数 ====================

// 余弦相似度（声明在 common.h）
// 语义相似度（本地实现，不依赖 multi_topology.c 的 static 函数）
static int local_bigram_similarity(const char* concept1, const char* concept2) {
    if (!concept1 || !concept2) return 0;
    int len1 = strlen(concept1);
    int len2 = strlen(concept2);
    if (len1 < 2 || len2 < 2) return 0;
    int common_chars = 0;
    for (int i = 0; i < len1 - 1; i++) {
        for (int j = 0; j < len2 - 1; j++) {
            if (concept1[i] == concept2[j] && concept1[i+1] == concept2[j+1]) {
                common_chars++;
                break;
            }
        }
    }
    int min_len = (len1 < len2) ? len1 : len2;
    return (common_chars * 100) / min_len;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static int int_max(int a, int b) { return a > b ? a : b; }

// ==================== T: 训练时评估 ====================

float topo_edge_growth_rate(MasterTopology* master, const TopoEvalSnapshot* prev) {
    if (!master || !prev || prev->total_edges == 0) return 0.0f;

    int current_edges = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (node) current_edges += node->edge_count;
        }
    }

    float delta = (float)(current_edges - prev->total_edges);
    return delta / (float)prev->total_edges;
}

float topo_confidence_entropy(MasterTopology* master, int bins) {
    if (!master || bins <= 1) return 0.0f;

    int* hist = (int*)calloc(bins, sizeof(int));
    if (!hist) return 0.0f;

    int total = 0;

    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (!node) continue;
            for (int e = 0; e < node->edge_count; e++) {
                float conf = node->edges[e].confidence;
                if (conf < 0.0f) conf = 0.0f;
                if (conf > 1.0f) conf = 1.0f;
                int idx = (int)(conf * (bins - 1));
                if (idx >= bins) idx = bins - 1;
                hist[idx]++;
                total++;
            }
        }
    }

    if (total == 0) {
        free(hist);
        return 0.0f;
    }

    float entropy = 0.0f;
    for (int i = 0; i < bins; i++) {
        if (hist[i] > 0) {
            float p = (float)hist[i] / (float)total;
            entropy -= p * log2f(p);
        }
    }

    free(hist);
    return entropy;  // 范围 [0, log2(bins)]
}

float topo_perplexity(MasterTopology* master, int bins) {
    float ent = topo_confidence_entropy(master, bins);
    return powf(2.0f, ent);
}

float topo_node_coverage(MasterTopology* master, int recent_count) {
    if (!master || recent_count <= 0) return 0.0f;
    (void)recent_count;  // 当前用 avg_activation > 0 表示被激活过

    int total_nodes = 0;
    int active_nodes = 0;

    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (!node) continue;
            total_nodes++;
            // 使用 sub 的 avg_activation_value 作为活跃指示
            // 也可用 node->activation > 0.01f
            if (node->activation > 0.01f || sub->avg_activation_value > 0.01f) {
                active_nodes++;
            }
        }
    }

    return total_nodes > 0 ? (float)active_nodes / (float)total_nodes : 0.0f;
}

float topo_edge_density(MasterTopology* master, int topo_id) {
    if (!master) return 0.0f;

    if (topo_id >= 0 && topo_id < master->sub_topo_count) {
        SubTopology* sub = master->sub_topologies[topo_id];
        if (!sub || !sub->net || sub->net->node_count <= 1) return 0.0f;

        int n = sub->net->node_count;
        int edges = 0;
        for (int i = 0; i < n; i++) {
            ReasoningNode* node = sub->net->nodes[i];
            if (node) edges += node->edge_count;
        }
        float max_edges = (float)n * (float)(n - 1) / 2.0f;
        return max_edges > 0 ? (float)edges / max_edges : 0.0f;
    }

    // topo_id == -1: 所有拓扑平均
    float sum_density = 0.0f;
    int count = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net || sub->net->node_count <= 1) continue;
        int n = sub->net->node_count;
        int edges = 0;
        for (int i = 0; i < n; i++) {
            ReasoningNode* node = sub->net->nodes[i];
            if (node) edges += node->edge_count;
        }
        float max_edges = (float)n * (float)(n - 1) / 2.0f;
        sum_density += max_edges > 0 ? (float)edges / max_edges : 0.0f;
        count++;
    }
    return count > 0 ? sum_density / (float)count : 0.0f;
}

float topo_conf_delta(const TopoEvalSnapshot* before, const TopoEvalSnapshot* after) {
    if (!before || !after) return 0.0f;
    return after->avg_confidence - before->avg_confidence;
}

TopoEvalSnapshot topo_eval_snapshot_take(MasterTopology* master) {
    TopoEvalSnapshot snap = {0};
    if (!master) return snap;

    int total_edges = 0;
    int total_nodes = 0;
    float sum_conf = 0.0f;
    float sum_act = 0.0f;

    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        total_nodes += sub->net->node_count;
        sum_act += sub->avg_activation_value * (float)sub->net->node_count;

        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (!node) continue;
            for (int e = 0; e < node->edge_count; e++) {
                total_edges++;
                sum_conf += node->edges[e].confidence;
            }
        }
    }

    snap.total_edges = total_edges;
    snap.total_nodes = total_nodes;
    snap.avg_confidence = total_edges > 0 ? sum_conf / total_edges : 0.0f;
    snap.avg_activation = total_nodes > 0 ? sum_act / total_nodes : 0.0f;
    snap.cross_link_count = master->cross_link_count;
    snap.timestamp = time(NULL);

    return snap;
}

// ==================== I: 推理时评估 ====================

static void sort_floats(float* arr, int n) {
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            if (arr[i] > arr[j]) {
                float t = arr[i]; arr[i] = arr[j]; arr[j] = t;
            }
        }
    }
}

PathStepStats topo_path_step_stats(MasterTopology* master, int num_trials) {
    PathStepStats stats = {0};
    if (!master || num_trials <= 0) return stats;

    // 分配路径长度记录
    int* lengths = (int*)calloc(num_trials, sizeof(int));
    if (!lengths) return stats;

    int max_score_len = 32;  // 每次走边的最大路径长度

    // 为每次走边分配路径数组
    int* path_topos = (int*)malloc(max_score_len * sizeof(int));
    int* path_nodes = (int*)malloc(max_score_len * sizeof(int));
    if (!path_topos || !path_nodes) {
        free(lengths); free(path_topos); free(path_nodes);
        return stats;
    }

    int total_steps = 0;
    int max_len = 0;
    int min_len = 999;

    for (int i = 0; i < num_trials; i++) {
        // 随机选择一个子拓扑和起始节点
        int topo_id = rand() % master->sub_topo_count;
        SubTopology* sub = master->sub_topologies[topo_id];
        if (!sub || !sub->net || sub->net->node_count == 0) {
            lengths[i] = 0;
            continue;
        }

        int start_node = rand() % sub->net->node_count;
        if (!sub->net->nodes[start_node]) {
            lengths[i] = 0;
            continue;
        }

        // 使用跨拓扑走边
        int len = topology_walk_cross(master, topo_id, start_node,
                                      path_topos, path_nodes, NULL,
                                      max_score_len, NULL, NULL, NULL, NULL, NULL);

        if (len < 1) len = 1;  // 至少包含起始节点
        lengths[i] = len;
        total_steps += len;
        if (len > max_len) max_len = len;
        if (len < min_len) min_len = len;
    }

    // 计算统计
    stats.min = min_len;
    stats.max = max_len;
    stats.mean = num_trials > 0 ? (float)total_steps / (float)num_trials : 0.0f;

    // 中位数
    sort_floats((float*)lengths, num_trials);  // 用 float 排序（int 强转）
    stats.median = (float)lengths[num_trials / 2];

    // 标准差
    float variance = 0.0f;
    for (int i = 0; i < num_trials; i++) {
        float diff = (float)lengths[i] - stats.mean;
        variance += diff * diff;
    }
    stats.stddev = num_trials > 0 ? sqrtf(variance / num_trials) : 0.0f;

    // 直方图（10个区间）
    stats.hist_bins = 10;
    stats.histogram = (int*)calloc(stats.hist_bins, sizeof(int));
    if (stats.histogram && max_len > min_len) {
        float range = (float)(max_len - min_len);
        for (int i = 0; i < num_trials; i++) {
            int idx = (int)((float)(lengths[i] - min_len) / range * (stats.hist_bins - 1));
            if (idx >= stats.hist_bins) idx = stats.hist_bins - 1;
            stats.histogram[idx]++;
        }
    }

    free(lengths);
    free(path_topos);
    free(path_nodes);

    return stats;
}

void topo_path_step_stats_free(PathStepStats* stats) {
    if (stats && stats->histogram) {
        free(stats->histogram);
        stats->histogram = NULL;
    }
}

float topo_associative_diversity(MasterTopology* master,
                                 int start_topo_id, int start_node_id,
                                 int num_samples, int max_path_len) {
    if (!master || num_samples <= 0 || max_path_len <= 0) return 0.0f;

    // 收集所有子拓扑节点数的总和作为最大节点 ID 上限
    int max_nodes = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (sub && sub->net) max_nodes += sub->net->node_count;
    }
    if (max_nodes == 0) return 0.0f;

    // 节点出现频率统计（扁平化索引：topo_id * 100000 + node_id）
    int* freq = (int*)calloc(max_nodes, sizeof(int));
    int total_occurrences = 0;

    int* path_topos = (int*)malloc(max_path_len * sizeof(int));
    int* path_nodes = (int*)malloc(max_path_len * sizeof(int));

    if (!freq || !path_topos || !path_nodes) {
        free(freq); free(path_topos); free(path_nodes);
        return 0.0f;
    }

    for (int s = 0; s < num_samples; s++) {
        // 每次走边使用不同的随机种子（通过激活衰减等自然产生变化）
        int len = topology_walk_cross(master, start_topo_id, start_node_id,
                                      path_topos, path_nodes, NULL,
                                      max_path_len, NULL, NULL, NULL, NULL, NULL);

        if (len <= 0) continue;

        // 统计路径中节点的出现
        for (int i = 0; i < len && i < max_path_len; i++) {
            int node_id = path_nodes[i];
            if (node_id >= 0 && node_id < max_nodes) {
                freq[node_id]++;
                total_occurrences++;
            }
        }
    }

    if (total_occurrences == 0) {
        free(freq); free(path_topos); free(path_nodes);
        return 0.0f;
    }

    // 计算 TAE = -Σ(p_i * log2(p_i))
    float entropy = 0.0f;
    for (int i = 0; i < max_nodes; i++) {
        if (freq[i] > 0) {
            float p = (float)freq[i] / (float)total_occurrences;
            entropy -= p * log2f(p);
        }
    }

    free(freq);
    free(path_topos);
    free(path_nodes);

    return entropy;
}

float topo_semantic_coherence(SubTopology* sub,
                              const int* path, int path_len) {
    if (!sub || !path || path_len < 2) return 0.0f;

    float total_sim = 0.0f;
    int pairs = 0;

    for (int i = 0; i < path_len - 1; i++) {
        ReasoningNode* node_a = NULL;
        ReasoningNode* node_b = NULL;

        if (path[i] >= 0 && path[i] < sub->net->node_count)
            node_a = sub->net->nodes[path[i]];
        if (path[i+1] >= 0 && path[i+1] < sub->net->node_count)
            node_b = sub->net->nodes[path[i+1]];

        if (!node_a || !node_b || !node_a->concept || !node_b->concept) continue;

        float sim = 0.0f;
        if (node_a->features && node_b->features &&
            node_a->feature_dim > 0 && node_a->feature_dim == node_b->feature_dim) {
            sim = cosine_similarity(node_a->features, node_b->features,
                                     node_a->feature_dim);
        } else {
            // 回退到字面 bigram 相似度
            int bigram = local_bigram_similarity(node_a->concept, node_b->concept);
            sim = (float)bigram / 100.0f;
        }

        total_sim += sim;
        pairs++;
    }

    return pairs > 0 ? total_sim / (float)pairs : 0.0f;
}

float topo_inference_stability(MasterTopology* master,
                               int start_topo_id, int start_node_id,
                               int num_repeats, int max_path_len) {
    if (!master || num_repeats < 2 || max_path_len <= 0) return 0.0f;

    // 存储每次推理的路径
    int** paths = (int**)calloc(num_repeats, sizeof(int*));
    int* lens = (int*)calloc(num_repeats, sizeof(int));

    if (!paths || !lens) {
        free(paths); free(lens);
        return 0.0f;
    }

    for (int r = 0; r < num_repeats; r++) {
        paths[r] = (int*)malloc(max_path_len * sizeof(int));
        int* path_topos = (int*)malloc(max_path_len * sizeof(int));
        if (!paths[r] || !path_topos) {
            for (int k = 0; k <= r; k++) {
                free(paths[k]);
            }
            free(paths); free(lens);
            return 0.0f;
        }

        lens[r] = topology_walk_cross(master, start_topo_id, start_node_id,
                                       path_topos, paths[r], NULL,
                                       max_path_len, NULL, NULL, NULL, NULL, NULL);
        free(path_topos);
    }

    // 计算两两 LCS 比例的平均值
    float total_sim = 0.0f;
    int comparisons = 0;

    for (int i = 0; i < num_repeats; i++) {
        for (int j = i + 1; j < num_repeats; j++) {
            total_sim += topo_path_lcs_ratio(paths[i], lens[i],
                                              paths[j], lens[j]);
            comparisons++;
        }
    }

    // 清理
    for (int i = 0; i < num_repeats; i++) {
        free(paths[i]);
    }
    free(paths);
    free(lens);

    return comparisons > 0 ? total_sim / (float)comparisons : 0.0f;
}

// ==================== L: 长期评估 ====================

float topo_forgetting_rate(int edges_before, int edges_after) {
    if (edges_before <= 0) return 0.0f;
    return (float)(edges_before - edges_after) / (float)edges_before;
}

/**
 * 对单个 QA 对检查是否可达（辅助知识泛化度计算）
 */
static int check_qa_reachable(MasterTopology* master,
                               const char* question, const char* answer,
                               int max_depth) {
    if (!master || !question || !answer) return 0;

    // 找到问题和答案中的概念节点
    // 简化处理：对问题/答案中的每个字尝试找节点
    int q_topo = -1, q_node = -1;
    int a_topo = -1, a_node = -1;

    // 遍历所有子拓扑，找 concept 包含问题或答案中字词的节点
    // 先用简单策略：找第一个有该字的节点
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;

        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (!node || !node->concept) continue;

            if (strstr(question, node->concept) != NULL && q_topo < 0) {
                q_topo = t;
                q_node = n;
            }
            if (strstr(answer, node->concept) != NULL && a_topo < 0) {
                a_topo = t;
                a_node = n;
            }
        }
        if (q_topo >= 0 && a_topo >= 0) break;
    }

    if (q_topo < 0 || a_topo < 0) return 0;

    return topo_path_exists(master, q_topo, q_node,
                            a_topo, a_node, max_depth);
}

float topo_knowledge_generalization(MasterTopology* master,
                                    const char** train_questions,
                                    const char** train_answers, int train_n,
                                    const char** val_questions,
                                    const char** val_answers, int val_n) {
    if (!master || !train_questions || !train_answers || train_n <= 0) return 0.0f;

    int max_depth = 10;

    // 训练集可达率
    int train_reachable = 0;
    for (int i = 0; i < train_n; i++) {
        if (check_qa_reachable(master, train_questions[i],
                                train_answers[i], max_depth)) {
            train_reachable++;
        }
    }
    float train_acc = (float)train_reachable / (float)train_n;

    // 验证集可达率（如果有）
    if (!val_questions || !val_answers || val_n <= 0) {
        return 1.0f - train_acc;  // 无验证集时，高训练 acc 不一定好
    }

    int val_reachable = 0;
    for (int i = 0; i < val_n; i++) {
        if (check_qa_reachable(master, val_questions[i],
                                val_answers[i], max_depth)) {
            val_reachable++;
        }
    }
    float val_acc = val_n > 0 ? (float)val_reachable / (float)val_n : 0.0f;

    // 泛化差距 = train_acc - val_acc
    float gap = train_acc - val_acc;
    return clampf(gap, 0.0f, 1.0f);
}

float topo_memory_promotion_rate(MemorySystem* memory, int since_hours) {
    if (!memory) return 0.0f;

    // 使用已有系统的时间字段
    time_t cutoff = time(NULL) - since_hours * 3600;

    if (memory->last_consolidation == 0) return 0.0f;
    if (memory->last_consolidation < cutoff) return 0.0f;

    // 简化估算：对比三级记忆的数量变化
    int context_count = memory->context_memory ? memory->context_memory->size : 0;
    int short_count = memory->short_term ? memory->short_term->size : 0;
    int perm_count = memory->permanent_memory ? memory->permanent_memory->size : 0;

    int total = context_count + short_count + perm_count;
    if (total == 0) return 0.0f;

    // 永久记忆占比作为 promotion rate 的近似
    return (float)perm_count / (float)total;
}

float topo_domain_interference_score(MasterTopology* master) {
    if (!master) return 0.0f;
    // 简化：使用跨拓扑连接比例作为域间干扰的代理
    if (master->cross_link_count == 0) return 0.0f;

    int total_edges = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (node) total_edges += node->edge_count;
        }
    }

    if (total_edges == 0) return 0.0f;
    float ratio = (float)master->cross_link_count / (float)total_edges;
    return clampf(ratio, 0.0f, 1.0f);
}

// ==================== R: 在线运行时评估 ====================

float online_learning_efficiency(const TopoEvalSnapshot* before,
                                  const TopoEvalSnapshot* after) {
    if (!before || !after) return 0.0f;

    int edge_delta = after->total_edges - before->total_edges;
    float conf_delta = after->avg_confidence - before->avg_confidence;

    if (edge_delta <= 0) {
        // 没有新建边，直接返回置信度变化
        return conf_delta;
    }

    return conf_delta / (float)edge_delta;
}

float online_response_relevance(MasterTopology* master,
                                const int* path_topos, const int* path_nodes,
                                int path_len, const char* user_input) {
    if (!master || !path_topos || !path_nodes || path_len <= 0 || !user_input) {
        return 0.0f;
    }

    if (strlen(user_input) == 0) return 0.0f;

    int relevant_count = 0;

    for (int i = 0; i < path_len; i++) {
        int t = path_topos[i];
        int n = path_nodes[i];
        if (t < 0 || t >= master->sub_topo_count) continue;

        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        if (n < 0 || n >= sub->net->node_count) continue;

        ReasoningNode* node = sub->net->nodes[n];
        if (!node || !node->concept) continue;

        // 检查节点概念是否出现在用户输入中
        if (strstr(user_input, node->concept) != NULL) {
            relevant_count++;
        }
    }

    return (float)relevant_count / (float)path_len;
}

void online_kq_init(KnowledgeQualityTrack* track, int window_size) {
    if (!track) return;
    memset(track, 0, sizeof(KnowledgeQualityTrack));
    track->window_size = window_size > 0 ? window_size : 10;
    track->count = 0;
}

void online_kq_update(KnowledgeQualityTrack* track,
                      int path_len, float avg_conf,
                      int cross_count, int total_steps,
                      int success) {
    if (!track) return;

    // 滑动平均更新
    int n = track->count;
    float alpha = n < track->window_size ?
                  (float)n / (float)(n + 1) :
                  (float)(track->window_size - 1) / (float)track->window_size;

    track->avg_path_length = alpha * track->avg_path_length +
                             (1.0f - alpha) * (float)path_len;
    track->avg_path_confidence = alpha * track->avg_path_confidence +
                                 (1.0f - alpha) * avg_conf;
    track->avg_cross_topo_ratio = alpha * track->avg_cross_topo_ratio +
                                  (1.0f - alpha) *
                                  (total_steps > 0 ? (float)cross_count / (float)total_steps : 0.0f);
    track->success_rate = alpha * track->success_rate +
                          (1.0f - alpha) * (success ? 1.0f : 0.0f);

    track->count++;
}

float online_kq_score(KnowledgeQualityTrack* track) {
    if (!track || track->count == 0) return 0.0f;

    float score = 0.0f;
    int components = 0;

    // 成功率高 → 好
    score += track->success_rate * 0.4f;
    components += 40;

    // 路径置信度高 → 好
    score += track->avg_path_confidence * 0.25f;
    components += 25;

    // 路径长度在合理范围 [3, 8]
    float len_score = 1.0f - fabsf(track->avg_path_length - 5.0f) / 10.0f;
    score += clampf(len_score, 0.0f, 1.0f) * 0.2f;
    components += 20;

    // 跨拓扑比在健康范围 [0.05, 0.25]
    float cross_score = 1.0f - fabsf(track->avg_cross_topo_ratio - 0.15f) / 0.5f;
    score += clampf(cross_score, 0.0f, 1.0f) * 0.15f;
    components += 15;

    return components > 0 ? score * 100.0f / (float)components : 0.0f;
}

// ==================== H: 综合健康度 ====================

TopoHealthReport topo_health_report(MasterTopology* master) {
    TopoHealthReport report = {0};
    if (!master) return report;

    // ---- 训练时指标 ----
    report.confidence_entropy = topo_confidence_entropy(master, 10);
    report.node_coverage = topo_node_coverage(master, 1);
    report.edge_density = topo_edge_density(master, -1) * 100.0f;  // 转换为百分比

    // ---- 推理时指标 ----
    report.jump_success_rate = topo_jump_success_rate(master);
    report.cross_topo_ratio = master->cross_link_count > 0 ?
        (float)master->cross_link_count /
        (float)(master->cross_link_count + 1) : 0.0f;

    // 路径统计（轻量采样）
    report.path_stats = topo_path_step_stats(master, 20);

    // 语义连贯性（采样评估）
    {
        int* sample_path = (int*)malloc(32 * sizeof(int));
        int* sample_topos = (int*)malloc(32 * sizeof(int));
        if (sample_path && sample_topos) {
            // 找第一个有节点的拓扑
            int found = 0;
            for (int t = 0; t < master->sub_topo_count && !found; t++) {
                SubTopology* sub = master->sub_topologies[t];
                if (sub && sub->net && sub->net->node_count > 0) {
                    int len = topology_walk_cross(master, t, 0,
                                                   sample_topos, sample_path,
                                                   NULL, 32, NULL, NULL, NULL, NULL, NULL);
                    if (len > 1) {
                        report.semantic_coherence =
                            topo_semantic_coherence(sub, sample_path, len);
                        found = 1;
                    }
                }
            }
        }
        free(sample_path);
        free(sample_topos);
    }

    // ---- 长期指标 ----
    report.forgetting_rate = 0.0f;  // 需要调用者提供前后统计
    report.knowledge_generalization = 0.0f;
    report.memory_promotion_rate = 0.0f;

    // ---- 健康分计算 ----
    float score = 0.0f;
    int total_weight = 0;

    // 跳转成功率 (25%)
    if (master->total_inferences > 0) {
        score += report.jump_success_rate * 25.0f;
    } else {
        score += 12.5f;  // 无推理数据时给中值
    }
    total_weight += 25;

    // 置信度分布熵 (20%) — 理想值在 [2.5, 3.5] for bins=10
    {
        float max_ent = log2f(10.0f);  // ~3.32
        float ent_score = report.confidence_entropy / max_ent;
        // 不是越高越好，中间最佳
        float penalty = fabsf(ent_score - 0.75f) * 2.0f;  // 0.75附近最佳
        score += (1.0f - clampf(penalty, 0.0f, 1.0f)) * 20.0f;
    }
    total_weight += 20;

    // 路径步长合理性 (15%) — 均值在 3-8 之间
    {
        float step_score = 1.0f - fabsf(report.path_stats.mean - 5.0f) / 10.0f;
        score += clampf(step_score, 0.0f, 1.0f) * 15.0f;
    }
    total_weight += 15;

    // 节点覆盖度 (15%)
    score += report.node_coverage * 15.0f;
    total_weight += 15;

    // 泛化度 (10%) — 无数据给中值
    score += 5.0f;
    total_weight += 10;

    // 跨拓扑比 (10%)
    {
        float cross_score = 1.0f - fabsf(report.cross_topo_ratio - 0.15f) / 0.5f;
        score += clampf(cross_score, 0.0f, 1.0f) * 10.0f;
    }
    total_weight += 10;

    // 语义连贯性 (5%)
    score += report.semantic_coherence * 5.0f;
    total_weight += 5;

    report.health_score = total_weight > 0 ?
                          clampf(score * 100.0f / (float)total_weight, 0.0f, 100.0f) :
                          0.0f;

    // 等级标签
    if (report.health_score >= 80) report.health_label = "健康";
    else if (report.health_score >= 60) report.health_label = "注意";
    else if (report.health_score >= 40) report.health_label = "警告";
    else report.health_label = "危险";

    return report;
}

void topo_health_report_print(const TopoHealthReport* report) {
    if (!report) return;

    printf("╔═══════════════════════════════════════════╗\n");
    printf("║  玄枢拓扑健康度报告                        ║\n");
    printf("╠═══════════════════════════════════════════╣\n");
    printf("║  综合健康分: %.1f / 100  [%s]           ║\n",
           report->health_score, report->health_label ? report->health_label : "?");
    printf("╠═══════════ 训练时指标 ═══════════╣\n");
    printf("║  边增长率:       %.2f                   ║\n", report->edge_growth_rate);
    printf("║  置信度分布熵:   %.4f                   ║\n", report->confidence_entropy);
    printf("║  节点覆盖度:     %.1f%%                  ║\n", report->node_coverage * 100.0f);
    printf("║  边密度:         %.4f%%                 ║\n", report->edge_density);
    printf("╠═══════════ 推理时指标 ═══════════╣\n");
    printf("║  跳转成功率:     %.1f%%                  ║\n", report->jump_success_rate * 100.0f);
    printf("║  联想多样性:     %.4f                   ║\n", report->associative_diversity);
    printf("║  语义连贯性:     %.4f                   ║\n", report->semantic_coherence);
    printf("║  跨拓扑比:       %.2f%%                  ║\n", report->cross_topo_ratio * 100.0f);
    printf("║  推理稳定度:     %.2f%%                  ║\n", report->inference_stability * 100.0f);
    printf("║  平均步长:       %.1f (±%.1f)          ║\n",
           report->path_stats.mean, report->path_stats.stddev);
    printf("╠═══════════ 长期指标 ═════════════╣\n");
    printf("║  遗忘率:         %.2f%%                  ║\n", report->forgetting_rate * 100.0f);
    printf("║  知识泛化度:     %.2f%%                  ║\n", report->knowledge_generalization * 100.0f);
    printf("║  记忆迁移率:     %.2f%%                  ║\n", report->memory_promotion_rate * 100.0f);
    printf("║  域间干扰:       %.2f                    ║\n", report->domain_interference);
    printf("╚═══════════════════════════════════════════╝\n");
}

void topo_eval_print_all(MasterTopology* master) {
    if (!master) {
        printf("[错误] master 为空\n");
        return;
    }

    printf("\n===== 玄枢拓扑评估报告 =====\n");
    printf("统计信息:\n");
    printf("  子拓扑数: %d\n", master->sub_topo_count);
    printf("  跨拓扑连接: %d\n", master->cross_link_count);
    printf("  总推理次数: %ld\n", master->total_inferences);
    printf("  成功推理: %ld\n", master->successful_inferences);

    printf("\n--- 每个子拓扑详情 ---\n");
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub) continue;

        int n = sub->net ? sub->net->node_count : 0;
        int e = 0;
        float avg_conf = 0.0f;
        for (int i = 0; i < n; i++) {
            ReasoningNode* node = sub->net->nodes[i];
            if (!node) continue;
            for (int j = 0; j < node->edge_count; j++) {
                e++;
                avg_conf += node->edges[j].confidence;
            }
        }
        avg_conf = e > 0 ? avg_conf / e : 0.0f;

        printf("  [%d] %s: %d节点 %d边 置信度%.3f 激活%.3f 优先%d\n",
               t, sub->name ? sub->name : "?", n, e, avg_conf,
               sub->avg_activation_value, sub->priority);
    }

    // 计算并打印完整健康度报告
    TopoHealthReport report = topo_health_report(master);
    topo_health_report_print(&report);
    topo_path_step_stats_free(&report.path_stats);
}

// ==================== 辅助工具 ====================

int topo_find_node_by_concept(MasterTopology* master,
                               const char* concept, int* out_topo_id) {
    if (!master || !concept || !out_topo_id) return -1;

    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;

        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (node && node->concept &&
                strcmp_null(node->concept, concept) == 0) {
                *out_topo_id = t;
                return n;
            }
        }
    }
    return -1;
}

/**
 * 简单的 BFS 路径搜索（用于判断 QA 可达性）
 * 搜索从 from 到 to 是否存在路径
 */
static int bfs_path_exists(SubTopology* sub, int from, int to, int max_depth) {
    if (!sub || !sub->net) return 0;
    if (from == to) return 1;
    if (from < 0 || from >= sub->net->node_count) return 0;
    if (to < 0 || to >= sub->net->node_count) return 0;

    int n = sub->net->node_count;
    int* queue = (int*)malloc(n * sizeof(int));
    int* depth = (int*)malloc(n * sizeof(int));
    int* visited = (int*)calloc(n, sizeof(int));
    if (!queue || !depth || !visited) {
        free(queue); free(depth); free(visited);
        return 0;
    }

    int head = 0, tail = 0;
    queue[tail++] = from;
    visited[from] = 1;
    depth[from] = 0;

    while (head < tail) {
        int cur = queue[head++];
        if (cur == to) {
            int result = depth[cur];
            free(queue); free(depth); free(visited);
            return result;
        }
        if (depth[cur] >= max_depth) continue;

        ReasoningNode* node = sub->net->nodes[cur];
        if (!node) continue;

        for (int e = 0; e < node->edge_count; e++) {
            ReasoningNode* next = node->edges[e].target;
            if (!next) continue;
            int next_id = next->node_id;
            if (next_id >= 0 && next_id < n && !visited[next_id]) {
                visited[next_id] = 1;
                depth[next_id] = depth[cur] + 1;
                queue[tail++] = next_id;
            }
        }
    }

    free(queue); free(depth); free(visited);
    return 0;
}

int topo_path_exists(MasterTopology* master,
                     int from_topo, int from_node,
                     int to_topo, int to_node, int max_depth) {
    if (!master) return 0;

    // 同拓扑内直接 BFS
    if (from_topo == to_topo) {
        SubTopology* sub = master_get_sub_topology(master, from_topo);
        return bfs_path_exists(sub, from_node, to_node, max_depth);
    }

    // 跨拓扑：需要走跨拓扑连接
    // 简化：先在各自拓扑内 BFS 到跨拓扑连接节点
    // 这是一个复杂问题，简化处理
    return 0;
}

float topo_path_jaccard(const int* path1, int len1,
                        const int* path2, int len2) {
    if (!path1 || !path2 || len1 <= 0 || len2 <= 0) return 0.0f;

    // 计算交集大小
    int intersection = 0;
    for (int i = 0; i < len1; i++) {
        for (int j = 0; j < len2; j++) {
            if (path1[i] == path2[j]) {
                intersection++;
                break;
            }
        }
    }

    int union_size = len1 + len2 - intersection;
    return union_size > 0 ? (float)intersection / (float)union_size : 0.0f;
}

float topo_path_lcs_ratio(const int* path1, int len1,
                          const int* path2, int len2) {
    if (!path1 || !path2 || len1 <= 0 || len2 <= 0) return 0.0f;

    // DP 求 LCS 长度
    int** dp = (int**)malloc((len1 + 1) * sizeof(int*));
    if (!dp) return 0.0f;
    for (int i = 0; i <= len1; i++) {
        dp[i] = (int*)calloc(len2 + 1, sizeof(int));
        if (!dp[i]) {
            for (int k = 0; k < i; k++) free(dp[k]);
            free(dp);
            return 0.0f;
        }
    }

    for (int i = 1; i <= len1; i++) {
        for (int j = 1; j <= len2; j++) {
            if (path1[i-1] == path2[j-1]) {
                dp[i][j] = dp[i-1][j-1] + 1;
            } else {
                dp[i][j] = int_max(dp[i-1][j], dp[i][j-1]);
            }
        }
    }

    int lcs_len = dp[len1][len2];

    for (int i = 0; i <= len1; i++) free(dp[i]);
    free(dp);

    int max_len = int_max(len1, len2);
    return max_len > 0 ? (float)lcs_len / (float)max_len : 0.0f;
}
