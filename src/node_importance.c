#include "../include/node_importance.h"
#include "../include/huarong_topology.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

// ==================== 宏定义 ====================

#define DEFAULT_DAMPING_FACTOR 0.85f
#define DEFAULT_ITERATIONS 100
#define DEFAULT_CONVERGENCE 1e-6f
#define DEFAULT_IMPORTANCE_THRESHOLD 0.1f
#define DEFAULT_PRUNING_THRESHOLD 0.01f

// 中心性权重 (默认)
#define DEGREE_WEIGHT 0.25f
#define BETWEENNESS_WEIGHT 0.25f
#define PAGERANK_WEIGHT 0.25f
#define ACTIVATION_WEIGHT 0.25f

// ==================== 评估器管理 ====================

NodeImportanceEvaluator* node_importance_create(float damping_factor, int iterations) {
    NodeImportanceEvaluator* eval = 
        (NodeImportanceEvaluator*)malloc(sizeof(NodeImportanceEvaluator));
    if (!eval) return NULL;

    eval->damping_factor = (damping_factor > 0 && damping_factor < 1) 
        ? damping_factor : DEFAULT_DAMPING_FACTOR;
    eval->pagerank_iterations = (iterations > 0) ? iterations : DEFAULT_ITERATIONS;
    eval->pagerank_convergence = DEFAULT_CONVERGENCE;
    eval->importance_threshold = DEFAULT_IMPORTANCE_THRESHOLD;
    eval->pruning_threshold = DEFAULT_PRUNING_THRESHOLD;
    eval->total_evaluations = 0;
    eval->last_evaluation = 0;
    eval->total_degree = 0;
    eval->avg_degree = 0;
    eval->max_degree = 0;
    eval->node_count = 0;

    return eval;
}

void node_importance_destroy(NodeImportanceEvaluator* evaluator) {
    if (evaluator) {
        free(evaluator);
    }
}

// ==================== 度中心性 ====================

float compute_degree_centrality(HuarongTopologyNet* net, int node_id) {
    if (!net || node_id < 0 || node_id >= net->node_count) return 0.0f;

    ReasoningNode* node = net->nodes[node_id];
    if (!node) return 0.0f;

    // 度中心性 = degree / (n - 1)
    int degree = node->connection_count;
    int max_degree = net->node_count - 1;
    
    return (max_degree > 0) ? (float)degree / max_degree : 0.0f;
}

void compute_all_degree_centrality(HuarongTopologyNet* net, float* scores, int count) {
    if (!net || !scores) return;

    int n = net->node_count;
    int max_degree = (n > 1) ? n - 1 : 1;

    for (int i = 0; i < count && i < n; i++) {
        ReasoningNode* node = net->nodes[i];
        if (node) {
            scores[i] = (float)node->connection_count / max_degree;
        } else {
            scores[i] = 0.0f;
        }
    }
}

// ==================== 介数中心性 ====================

float compute_betweenness_centrality(HuarongTopologyNet* net, int node_id) {
    if (!net || node_id < 0 || node_id >= net->node_count) return 0.0f;

    int n = net->node_count;
    if (n <= 2) return 0.0f;

    // 简化的介数中心性计算
    // 使用 BFS 近似计算
    float betweenness = 0.0f;
    int* stack = (int*)malloc(n * sizeof(int));
    int** preds = (int**)malloc(n * sizeof(int*));
    int* distances = (int*)malloc(n * sizeof(int));
    int* sigma = (int*)malloc(n * sizeof(int));
    float* delta = (float*)malloc(n * sizeof(float));

    if (!stack || !preds || !distances || !sigma || !delta) {
        free(stack); free(preds); free(distances); free(sigma); free(delta);
        return 0.0f;
    }

    // 初始化 preds
    for (int i = 0; i < n; i++) {
        preds[i] = (int*)malloc(n * sizeof(int));
        memset(preds[i], 0, n * sizeof(int));
        preds[i][0] = -1;  // 标记
    }

    // 对所有源节点进行 BFS
    for (int source = 0; source < n; source++) {
        if (source == node_id) continue;

        // BFS
        memset(distances, -1, n * sizeof(int));
        memset(sigma, 0, n * sizeof(int));
        for (int i = 0; i < n; i++) delta[i] = 0.0f;

        int stack_top = -1;
        int* queue = (int*)malloc(n * sizeof(int));
        int queue_front = 0, queue_back = 0;

        distances[source] = 0;
        sigma[source] = 1;
        queue[queue_back++] = source;

        while (queue_front < queue_back) {
            int v = queue[queue_front++];
            stack[++stack_top] = v;

            ReasoningNode* node = net->nodes[v];
            if (!node) continue;

            for (int i = 0; i < node->connection_count; i++) {
                int w = node->connections[i]->node_id;
                if (distances[w] < 0) {
                    distances[w] = distances[v] + 1;
                    queue[queue_back++] = w;
                }
                if (distances[w] == distances[v] + 1) {
                    sigma[w] += sigma[v];
                    preds[w][++preds[w][0]] = v;  // preds[w][0] 存储计数
                }
            }
        }

        // accumulation
        for (int i = 0; i < n; i++) delta[i] = 0.0f;
        while (stack_top >= 0) {
            int w = stack[stack_top--];
            for (int i = 1; i <= preds[w][0]; i++) {
                int v = preds[w][i];
                delta[v] += (float)sigma[v] / sigma[w] * (1.0f + delta[w]);
            }
            if (w != source) {
                betweenness += delta[w];
            }
        }

        free(queue);
    }

    // 归一化
    betweenness = betweenness / (((float)n - 1) * ((float)n - 2));

    // 清理
    for (int i = 0; i < n; i++) {
        free(preds[i]);
    }
    free(stack); free(preds); free(distances); free(sigma); free(delta);

    return betweenness;
}

void compute_all_betweenness_centrality(HuarongTopologyNet* net, float* scores, int count) {
    if (!net || !scores) return;

    for (int i = 0; i < count && i < net->node_count; i++) {
        scores[i] = compute_betweenness_centrality(net, i);
    }
}

// ==================== PageRank ====================

float compute_page_rank(HuarongTopologyNet* net, int node_id,
                        float damping, int iterations, float convergence) {
    if (!net || node_id < -1 || node_id >= net->node_count) return 0.0f;

    int n = net->node_count;
    if (n == 0) return 0.0f;

    // 如果请求单个节点，先计算所有再返回
    if (node_id >= 0) {
        float* scores = (float*)malloc(n * sizeof(float));
        if (!scores) return 0.0f;

        compute_all_page_rank(net, scores, damping, iterations, convergence);
        float result = scores[node_id];
        free(scores);
        return result;
    }

    return 0.0f;  // 不应该到达这里
}

void compute_all_page_rank(HuarongTopologyNet* net, float* scores,
                          float damping, int iterations, float convergence) {
    if (!net || !scores) return;

    int n = net->node_count;
    if (n == 0) return;

    float* pr = (float*)calloc(n, sizeof(float));
    float* pr_new = (float*)calloc(n, sizeof(float));
    if (!pr || !pr_new) {
        free(pr); free(pr_new);
        return;
    }

    // 初始化
    float init_val = 1.0f / n;
    for (int i = 0; i < n; i++) {
        pr[i] = init_val;
    }

    int* out_degrees = (int*)malloc(n * sizeof(int));
    if (!out_degrees) {
        free(pr); free(pr_new);
        return;
    }

    // 计算出度
    for (int i = 0; i < n; i++) {
        if (net->nodes[i]) {
            out_degrees[i] = net->nodes[i]->connection_count;
        } else {
            out_degrees[i] = 0;
        }
    }

    // 迭代计算
    for (int iter = 0; iter < iterations; iter++) {
        float max_diff = 0.0f;

        // 计算新的 PageRank
        for (int i = 0; i < n; i++) {
            pr_new[i] = (1.0f - damping) / n;
        }

        // 传播 PageRank
        for (int i = 0; i < n; i++) {
            if (net->nodes[i] && out_degrees[i] > 0) {
                float contribution = damping * pr[i] / out_degrees[i];
                ReasoningNode* node = net->nodes[i];
                for (int j = 0; j < node->connection_count; j++) {
                    int target = node->connections[j]->node_id;
                    if (target >= 0 && target < n) {
                        pr_new[target] += contribution;
                    }
                }
            }
        }

        // 检查收敛
        for (int i = 0; i < n; i++) {
            float diff = fabsf(pr_new[i] - pr[i]);
            if (diff > max_diff) max_diff = diff;
            pr[i] = pr_new[i];
        }

        if (max_diff < convergence) break;
    }

    memcpy(scores, pr, n * sizeof(float));

    free(pr); free(pr_new); free(out_degrees);
}

// ==================== 激活中心性 ====================

float compute_activation_centrality(HuarongTopologyNet* net, int node_id,
                                   float* activation_history, int history_length) {
    if (!net || node_id < 0 || node_id >= net->node_count) return 0.0f;

    ReasoningNode* node = net->nodes[node_id];
    if (!node || history_length <= 0) return 0.0f;

    // 基于激活历史的中心性
    float total_activation = 0.0f;
    for (int i = 0; i < history_length; i++) {
        total_activation += activation_history[i];
    }

    // 考虑节点的连接数作为放大因子
    float connection_factor = 1.0f + logf(1.0f + node->connection_count);

    return total_activation * connection_factor / history_length;
}

// ==================== 综合评分 ====================

float composite_importance_score(ImportanceMetrics* metrics, float* weights) {
    if (!metrics) return 0.0f;

    // 默认权重
    float w_degree = DEGREE_WEIGHT;
    float w_between = BETWEENNESS_WEIGHT;
    float w_pr = PAGERANK_WEIGHT;
    float w_act = ACTIVATION_WEIGHT;

    // 使用自定义权重
    if (weights) {
        w_degree = weights[0];
        w_between = weights[1];
        w_pr = weights[2];
        w_act = weights[3];
    }

    // 归一化并计算加权平均
    float score = 
        w_degree * metrics->degree_centrality +
        w_between * metrics->betweenness_centrality +
        w_pr * metrics->page_rank +
        w_act * metrics->activation_centrality;

    return score;
}

// ==================== 评估函数 ====================

ImportanceMetrics* evaluate_node(NodeImportanceEvaluator* evaluator,
                                 HuarongTopologyNet* net,
                                 int node_id) {
    if (!evaluator || !net || node_id < 0 || node_id >= net->node_count) {
        return NULL;
    }

    ImportanceMetrics* metrics = (ImportanceMetrics*)malloc(sizeof(ImportanceMetrics));
    if (!metrics) return NULL;

    memset(metrics, 0, sizeof(ImportanceMetrics));
    metrics->node_id = node_id;

    // 计算各项指标
    metrics->degree_centrality = compute_degree_centrality(net, node_id);
    metrics->betweenness_centrality = compute_betweenness_centrality(net, node_id);
    metrics->page_rank = compute_page_rank(net, node_id, 
                                           evaluator->damping_factor,
                                           evaluator->pagerank_iterations,
                                           evaluator->pagerank_convergence);
    metrics->activation_centrality = net->nodes[node_id] ? 
                                     net->nodes[node_id]->activation : 0.0f;

    // 计算综合评分
    metrics->composite_score = composite_importance_score(metrics, NULL);

    evaluator->total_evaluations++;
    metrics->last_updated = time(NULL);

    return metrics;
}

ImportanceMetrics** evaluate_all_nodes(NodeImportanceEvaluator* evaluator,
                                      HuarongTopologyNet* net,
                                      int* count) {
    if (!evaluator || !net || !count) return NULL;

    int n = net->node_count;
    if (n <= 0) {
        *count = 0;
        return NULL;
    }

    ImportanceMetrics** results = (ImportanceMetrics**)malloc(n * sizeof(ImportanceMetrics*));
    if (!results) {
        *count = 0;
        return NULL;
    }

    int valid_count = 0;
    for (int i = 0; i < n; i++) {
        results[i] = evaluate_node(evaluator, net, i);
        if (results[i]) {
            valid_count++;
        }
    }

    *count = valid_count;
    evaluator->last_evaluation = time(NULL);

    return results;
}

// ==================== 摘要与报告 ====================

ImportanceSummary* generate_importance_summary(NodeImportanceEvaluator* evaluator,
                                              ImportanceMetrics** metrics,
                                              int count) {
    if (!evaluator || !metrics || count <= 0) return NULL;

    ImportanceSummary* summary = (ImportanceSummary*)malloc(sizeof(ImportanceSummary));
    if (!summary) return NULL;

    memset(summary, 0, sizeof(ImportanceSummary));

    float sum = 0.0f;
    float sum_sq = 0.0f;

    for (int i = 0; i < count; i++) {
        if (!metrics[i]) continue;

        float score = metrics[i]->composite_score;

        if (score >= evaluator->importance_threshold) {
            if (score >= 0.5f) {
                summary->high_importance_count++;
            } else {
                summary->medium_importance_count++;
            }
        } else {
            summary->low_importance_count++;
        }

        if (score < evaluator->pruning_threshold) {
            summary->prune_candidate_count++;
        }

        sum += score;
        sum_sq += score * score;
    }

    summary->avg_importance = sum / count;

    // 方差
    float variance = (sum_sq / count) - (summary->avg_importance * summary->avg_importance);
    summary->variance = variance > 0 ? variance : 0.0f;

    return summary;
}

int* get_prune_candidates(NodeImportanceEvaluator* evaluator,
                         ImportanceMetrics** metrics,
                         int count,
                         int* output_count) {
    if (!evaluator || !metrics || !output_count) {
        if (output_count) *output_count = 0;
        return NULL;
    }

    int* candidates = (int*)malloc(count * sizeof(int));
    if (!candidates) {
        *output_count = 0;
        return NULL;
    }

    int found = 0;
    for (int i = 0; i < count; i++) {
        if (metrics[i] && metrics[i]->composite_score < evaluator->pruning_threshold) {
            candidates[found++] = metrics[i]->node_id;
        }
    }

    *output_count = found;
    return candidates;
}

int* get_high_importance_nodes(NodeImportanceEvaluator* evaluator,
                              ImportanceMetrics** metrics,
                              int count,
                              int* output_count) {
    if (!evaluator || !metrics || !output_count) {
        if (output_count) *output_count = 0;
        return NULL;
    }

    int* nodes = (int*)malloc(count * sizeof(int));
    if (!nodes) {
        *output_count = 0;
        return NULL;
    }

    int found = 0;
    for (int i = 0; i < count; i++) {
        if (metrics[i] && metrics[i]->composite_score >= evaluator->importance_threshold) {
            nodes[found++] = metrics[i]->node_id;
        }
    }

    *output_count = found;
    return nodes;
}

// ==================== 便捷函数 ====================

float quick_node_importance(HuarongTopologyNet* net, int node_id) {
    static NodeImportanceEvaluator* cached_eval = NULL;
    if (!cached_eval) {
        cached_eval = node_importance_create(DEFAULT_DAMPING_FACTOR, DEFAULT_ITERATIONS);
    }

    ImportanceMetrics* metrics = evaluate_node(cached_eval, net, node_id);
    if (!metrics) return 0.0f;

    float score = metrics->composite_score;
    free(metrics);
    return score;
}

void batch_node_importance(HuarongTopologyNet* net, int* node_ids,
                          int count, float* scores) {
    if (!net || !node_ids || !scores) return;

    NodeImportanceEvaluator* eval = node_importance_create(
        DEFAULT_DAMPING_FACTOR, DEFAULT_ITERATIONS);

    for (int i = 0; i < count; i++) {
        ImportanceMetrics* metrics = evaluate_node(eval, net, node_ids[i]);
        scores[i] = metrics ? metrics->composite_score : 0.0f;
        if (metrics) free(metrics);
    }

    node_importance_destroy(eval);
}

// ==================== 快照版重要性评估 ====================

/**
 * 在快照上计算简化的介数中心性（BFS版，无全对最短路径）
 * 只计算度中心性+PageRank轻量版，跳过O(n³)的介数
 */
static float snap_computed_importance(TopoSnapshot* snap, int node_id,
                                      float* pagerank) {
    if (!snap || node_id < 0 || node_id >= snap->node_count) return 0.0f;

    // 度中心性
    float deg_cent = (float)snap->degrees[node_id] / 
                     (snap->node_count > 1 ? snap->node_count - 1 : 1);

    // 综合：度中心性 50% + PageRank 50%
    return deg_cent * 0.5f + pagerank[node_id] * 0.5f;
}

float* evaluate_on_snapshot(NodeImportanceEvaluator* evaluator,
                            TopoSnapshot* snap) {
    if (!evaluator || !snap || snap->node_count < 2) return NULL;

    int n = snap->node_count;
    float* scores = (float*)calloc(n, sizeof(float));
    if (!scores) return NULL;

    // ——— 简化 PageRank ———
    float* pr = (float*)malloc(n * sizeof(float));
    if (!pr) { free(scores); return NULL; }

    for (int i = 0; i < n; i++)
        pr[i] = 1.0f / n;

    float damping = evaluator->damping_factor;
    int max_iter = evaluator->pagerank_iterations;
    float conv = evaluator->pagerank_convergence;

    for (int iter = 0; iter < max_iter; iter++) {
        float* new_pr = (float*)calloc(n, sizeof(float));
        if (!new_pr) { free(pr); free(scores); return NULL; }

        float dangling_sum = 0.0f;
        for (int i = 0; i < n; i++) {
            if (snap->degrees[i] == 0)
                dangling_sum += pr[i];
        }
        float base = (1.0f - damping) / n;
        float dangle_contrib = damping * dangling_sum / n;

        for (int i = 0; i < n; i++) {
            new_pr[i] = base + dangle_contrib;
            if (snap->adj_lists[i]) {
                for (int j = 0; j < snap->degrees[i]; j++) {
                    int to = snap->adj_lists[i][j];
                    if (to >= 0 && to < n && snap->degrees[to] > 0)
                        new_pr[to] += damping * pr[i] / snap->degrees[i];
                }
            }
        }

        float diff = 0.0f;
        for (int i = 0; i < n; i++) {
            diff += fabsf(new_pr[i] - pr[i]);
            pr[i] = new_pr[i];
        }
        free(new_pr);

        if (diff < conv) break;
    }

    // ——— 计算综合评分 ———
    for (int i = 0; i < n; i++) {
        scores[i] = snap_computed_importance(snap, i, pr);
    }

    free(pr);
    evaluator->total_evaluations++;
    evaluator->last_evaluation = time(NULL);

    // 输出简要统计
    int high = 0, med = 0, low = 0;
    for (int i = 0; i < n; i++) {
        if (scores[i] > 0.3f) high++;
        else if (scores[i] > 0.1f) med++;
        else low++;
    }
    printf("  [快照评估] 高=%d 中=%d 低=%d\n", high, med, low);

    return scores;
}
