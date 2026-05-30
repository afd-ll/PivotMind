#ifndef NODE_IMPORTANCE_H
#define NODE_IMPORTANCE_H

#include "huarong_topology.h"
#include "multi_topology.h"
#include "topo_snapshot.h"
#include <stdbool.h>

// ==================== 节点重要性评估 ====================

/**
 * 节点重要性评估器
 * 用于评估拓扑网络中节点的重要性，支持多种评估算法
 */
typedef struct NodeImportanceEvaluator {
    // 基础统计
    float total_degree;           // 总度数
    float avg_degree;             // 平均度数
    float max_degree;             // 最大度数
    int node_count;               // 节点总数

    // PageRank 相关
    float damping_factor;         // 阻尼因子 (默认 0.85)
    int pagerank_iterations;      // 迭代次数
    float pagerank_convergence;    // 收敛阈值

    // 重要性阈值
    float importance_threshold;   // 重要性阈值 (默认 0.1)
    float pruning_threshold;      // 剪枝阈值 (默认 0.01)

    // 统计信息
    long total_evaluations;       // 评估次数
    time_t last_evaluation;       // 上次评估时间
} NodeImportanceEvaluator;

// ==================== 重要性指标 ====================

/**
 * 单个节点的重要性指标
 */
typedef struct ImportanceMetrics {
    int node_id;                  // 节点ID

    // 度中心性
    float degree_centrality;       // 度中心性

    // 介数中心性
    float betweenness_centrality; // 介数中心性

    // PageRank
    float page_rank;              // PageRank 值

    // 特征向量中心性
    float eigenvector_centrality; // 特征向量中心性

    // 激活中心性 (基于激活历史)
    float activation_centrality;  // 激活中心性

    // 综合评分
    float composite_score;        // 综合重要性评分

    // 元信息
    time_t first_seen;           // 首次出现时间
    time_t last_updated;          // 最后更新时间
    int access_count;             // 访问次数
} ImportanceMetrics;

/**
 * 评估结果摘要
 */
typedef struct ImportanceSummary {
    int high_importance_count;    // 高重要性节点数
    int medium_importance_count;  // 中重要性节点数
    int low_importance_count;     // 低重要性节点数
    int prune_candidate_count;    // 可剪枝节点数

    float avg_importance;         // 平均重要性
    float variance;               // 重要性方差
    float skewness;              // 重要性偏度

    float coverage;               // 覆盖率 (高重要性节点的连接覆盖)
} ImportanceSummary;

// ==================== API 函数声明 ====================

// ========== 评估器管理 ==========

/**
 * 创建节点重要性评估器
 * @param damping_factor 阻尼因子 (0-1)
 * @param iterations 迭代次数
 * @return 评估器实例
 */
NodeImportanceEvaluator* node_importance_create(float damping_factor, int iterations);

/**
 * 销毁评估器
 */
void node_importance_destroy(NodeImportanceEvaluator* evaluator);

// ========== 核心评估算法 ==========

/**
 * 计算度中心性
 * @param net 拓扑网络
 * @param node_id 节点ID
 * @return 度中心性值 (0-1)
 */
float compute_degree_centrality(HuarongTopologyNet* net, int node_id);

/**
 * 计算所有节点的度中心性
 * @param net 拓扑网络
 * @param scores 输出数组 (需预先分配)
 * @param count 节点数量
 */
void compute_all_degree_centrality(HuarongTopologyNet* net, float* scores, int count);

/**
 * 计算介数中心性
 * @param net 拓扑网络
 * @param node_id 节点ID
 * @return 介数中心性值
 */
float compute_betweenness_centrality(HuarongTopologyNet* net, int node_id);

/**
 * 计算所有节点的介数中心性
 * @param net 拓扑网络
 * @param scores 输出数组
 * @param count 节点数量
 */
void compute_all_betweenness_centrality(HuarongTopologyNet* net, float* scores, int count);

/**
 * 计算 PageRank
 * @param net 拓扑网络
 * @param node_id 节点ID (或 -1 获取所有)
 * @param damping 阻尼因子
 * @param iterations 迭代次数
 * @param convergence 收敛阈值
 * @return PageRank 值 (单节点) 或存储在 scores 中
 */
float compute_page_rank(HuarongTopologyNet* net, int node_id,
                        float damping, int iterations, float convergence);

/**
 * 计算所有节点的 PageRank
 * @param net 拓扑网络
 * @param scores 输出数组
 * @param damping 阻尼因子
 * @param iterations 迭代次数
 * @param convergence 收敛阈值
 */
void compute_all_page_rank(HuarongTopologyNet* net, float* scores,
                          float damping, int iterations, float convergence);

/**
 * 计算激活中心性 (基于节点激活历史)
 * @param net 拓扑网络
 * @param node_id 节点ID
 * @param activation_history 激活历史数组
 * @param history_length 历史长度
 * @return 激活中心性值
 */
float compute_activation_centrality(HuarongTopologyNet* net, int node_id,
                                   float* activation_history, int history_length);

/**
 * 计算综合重要性评分
 * @param metrics 各项指标
 * @param weights 各指标权重 (可为 NULL 使用默认权重)
 * @return 综合评分 (0-1)
 */
float composite_importance_score(ImportanceMetrics* metrics, float* weights);

/**
 * 评估单个节点
 * @param evaluator 评估器
 * @param net 拓扑网络
 * @param node_id 节点ID
 * @return 重要性指标结构 (需调用者释放)
 */
ImportanceMetrics* evaluate_node(NodeImportanceEvaluator* evaluator,
                                 HuarongTopologyNet* net,
                                 int node_id);

/**
 * 评估所有节点
 * @param evaluator 评估器
 * @param net 拓扑网络
 * @param count 输出节点数量
 * @return 重要性指标数组 (需调用者释放)
 */
ImportanceMetrics** evaluate_all_nodes(NodeImportanceEvaluator* evaluator,
                                      HuarongTopologyNet* net,
                                      int* count);

// ========== 摘要与报告 ==========

/**
 * 生成评估摘要
 * @param evaluator 评估器
 * @param metrics 评估结果数组
 * @param count 节点数量
 * @return 摘要结构
 */
ImportanceSummary* generate_importance_summary(NodeImportanceEvaluator* evaluator,
                                              ImportanceMetrics** metrics,
                                              int count);

/**
 * 获取可剪枝节点列表
 * @param evaluator 评估器
 * @param metrics 评估结果数组
 * @param count 节点数量
 * @param output_count 输出符合条件节点数量
 * @return 节点ID数组 (需调用者释放)
 */
int* get_prune_candidates(NodeImportanceEvaluator* evaluator,
                         ImportanceMetrics** metrics,
                         int count,
                         int* output_count);

/**
 * 在快照上评估节点重要性
 * 计算度中心性+介数中心性+PageRank的复合评分
 * 不访问主拓扑，线程安全
 * @param evaluator 评估器
 * @param snap 拓扑快照
 * @return 评分数组 (float[snap->node_count], 需free释放)
 */
float* evaluate_on_snapshot(NodeImportanceEvaluator* evaluator,
                            TopoSnapshot* snap);

/**
 * 获取高重要性节点列表
 * @param evaluator 评估器
 * @param metrics 评估结果数组
 * @param count 节点数量
 * @param output_count 输出高重要性节点数量
 * @return 节点ID数组 (需调用者释放)
 */
int* get_high_importance_nodes(NodeImportanceEvaluator* evaluator,
                              ImportanceMetrics** metrics,
                              int count,
                              int* output_count);

// ========== 便捷函数 ==========

/**
 * 快速获取节点重要性 (使用默认参数)
 * @param net 拓扑网络
 * @param node_id 节点ID
 * @return 重要性评分 (0-1)
 */
float quick_node_importance(HuarongTopologyNet* net, int node_id);

/**
 * 批量获取节点重要性
 * @param net 拓扑网络
 * @param node_ids 节点ID数组
 * @param count 节点数量
 * @param scores 输出评分数组 (需预先分配)
 */
void batch_node_importance(HuarongTopologyNet* net, int* node_ids,
                          int count, float* scores);

#endif // NODE_IMPORTANCE_H
