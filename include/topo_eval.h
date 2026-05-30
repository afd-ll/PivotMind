#ifndef TOPO_EVAL_H
#define TOPO_EVAL_H

#include "multi_topology.h"
#include "huarong_topology.h"
#include "memory_system.h"
#include "autonomic_learner.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 数据结构 ====================

/** 路径步长统计 */
typedef struct {
    float mean;          // 平均步长
    float median;        // 中位数步长
    float stddev;        // 标准差
    int min;
    int max;
    int* histogram;      // 步长分布直方图
    int hist_bins;
} PathStepStats;

/** 在线运行时知识质量跟踪（滑动窗口） */
typedef struct {
    float avg_path_length;       // 平均路径长度
    float avg_path_confidence;   // 路径平均置信度
    float avg_cross_topo_ratio;  // 跨拓扑占比
    float success_rate;          // 成功率
    int window_size;            // 滑动窗口大小
    int count;                  // 当前有效记录数
} KnowledgeQualityTrack;

/** 综合拓扑健康度报告 */
typedef struct {
    // 训练时指标
    float edge_growth_rate;
    float confidence_entropy;
    float node_coverage;
    float edge_density;
    
    // 推理时指标
    float jump_success_rate;
    float associative_diversity;
    float semantic_coherence;
    float cross_topo_ratio;
    float inference_stability;
    PathStepStats path_stats;
    
    // 长期指标
    float forgetting_rate;
    float knowledge_generalization;
    float memory_promotion_rate;
    float domain_interference;
    
    // 健康总分
    float health_score;          // 0-100
    const char* health_label;    // "健康"/"注意"/"警告"/"危险"
} TopoHealthReport;

/** 训练前后状态快照（用于 delta 计算） */
typedef struct {
    int total_edges;
    int total_nodes;
    float avg_confidence;
    float avg_activation;
    int cross_link_count;
    time_t timestamp;
} TopoEvalSnapshot;

// ==================== 训练时评估 API ====================

/**
 * T1: 边增长率
 * rate = (current_edges - prev_edges) / prev_edges
 */
float topo_edge_growth_rate(MasterTopology* master, const TopoEvalSnapshot* prev);

/**
 * T2: 置信度分布熵 (Topological Perplexity 的基础)
 * H = -Σ(p_i * log2(p_i)), p_i = 置信度落入第 i 个区间的比例
 * @param bins 分箱数 (推荐 10)
 * @return 熵值 [0, log2(bins)]
 */
float topo_confidence_entropy(MasterTopology* master, int bins);

/**
 * Topological Perplexity = 2^H
 * 量化拓扑的"知识确定度"，越高表示知识越不确定/多样化
 */
float topo_perplexity(MasterTopology* master, int bins);

/**
 * T3: 节点覆盖度
 * 最近 N 轮训练中被激活的节点占比
 * @param recent_count 检查最近多少轮训练
 */
float topo_node_coverage(MasterTopology* master, int recent_count);

/**
 * T4: 边密度
 * 指定拓扑内的边密度 = total_edges / (n*(n-1)/2)
 * @param topo_id -1 表示所有拓扑的平均值
 */
float topo_edge_density(MasterTopology* master, int topo_id);

/**
 * T5: 快照间置信度变化均值
 * 取两个快照的 avg_confidence 之差
 */
float topo_conf_delta(const TopoEvalSnapshot* before, const TopoEvalSnapshot* after);

/**
 * 创建当前拓扑的评估快照
 */
TopoEvalSnapshot topo_eval_snapshot_take(MasterTopology* master);

// ==================== 推理时评估 API ====================

/**
 * I1: 路径步长统计
 * 在拓扑上随机采样走边 N 次，统计路径长度分布
 */
PathStepStats topo_path_step_stats(MasterTopology* master, int num_trials);

/**
 * 释放 PathStepStats 内部资源
 */
void topo_path_step_stats_free(PathStepStats* stats);

/**
 * I2: 跳转成功率
 * 直接复用 MasterTopology 的统计字段
 */
static inline float topo_jump_success_rate(MasterTopology* master) {
    if (!master || master->total_inferences == 0) return 0.0f;
    return (float)master->successful_inferences / (float)master->total_inferences;
}

/**
 * I3: 联想多样性 (Topological Associative Entropy, TAE)
 * 对同一输入多次走边，计算节点出现频率的信息熵
 * TAE = -Σ(p_i * log2(p_i)), p_i = 节点 i 在 N 次走边中出现频率
 */
float topo_associative_diversity(MasterTopology* master,
                                 int start_topo_id, int start_node_id,
                                 int num_samples, int max_path_len);

/**
 * I4: 语义连贯性评分
 * 路径中相邻节点的特征向量余弦相似度均值
 * 若节点无特征向量，使用字面 bigram 相似度（复用 calculate_semantic_similarity）
 */
float topo_semantic_coherence(SubTopology* sub, 
                              const int* path, int path_len);

/**
 * I5: 跨拓扑跳转比
 */
static inline float topo_cross_ratio(const int* path_topos, int path_len) {
    if (!path_topos || path_len <= 1) return 0.0f;
    int cross_count = 0;
    for (int i = 1; i < path_len; i++) {
        if (path_topos[i] != path_topos[i-1]) cross_count++;
    }
    return (float)cross_count / (float)(path_len - 1);
}

/**
 * I6: 推理稳定度
 * 同一输入多次推理，路径重复度（最长公共子序列比例）
 */
float topo_inference_stability(MasterTopology* master,
                               int start_topo_id, int start_node_id,
                               int num_repeats, int max_path_len);

// ==================== 长期评估 API ====================

/**
 * L1: 遗忘率
 * 使用遗忘前后的边数量统计
 */
float topo_forgetting_rate(int edges_before, int edges_after);

/**
 * L2: 知识泛化度
 * acc_train - acc_val，检测过拟合
 * @param master 拓扑
 * @param train_qa 训练集 QA 对
 * @param train_n 训练集大小
 * @param val_qa 验证集 QA 对
 * @param val_n 验证集大小
 * @return 泛化差距（越小越好，>0.15 表示严重过拟合）
 */
float topo_knowledge_generalization(MasterTopology* master,
                                    const char** train_questions,
                                    const char** train_answers, int train_n,
                                    const char** val_questions,
                                    const char** val_answers, int val_n);

/**
 * L3: 记忆迁移效率
 */
float topo_memory_promotion_rate(MemorySystem* memory, int since_hours);

/**
 * L4: 拓扑级域间干扰（使用已有 catastrophic_forgetting）
 */
float topo_domain_interference_score(MasterTopology* master);

// ==================== 在线运行时评估 API ====================

/**
 * R1: 每轮对话学习效率
 * = (after.conf - before.conf) / max(1, after.edges - before.edges)
 */
float online_learning_efficiency(const TopoEvalSnapshot* before,
                                  const TopoEvalSnapshot* after);

/**
 * R2: 响应相关度
 * 生成的回复路径中，与用户输入共现的概念占比
 */
float online_response_relevance(MasterTopology* master,
                                const int* path_topos, const int* path_nodes,
                                int path_len, const char* user_input);

/**
 * R3: 探索 vs 利用比率（复用 cognitive_params）
 */
static inline float online_explore_exploit_ratio(CognitiveState* state) {
    if (!state) return 0.5f;
    return state->explore_rate;
}

/**
 * R4: 知识质量跟踪（滑动窗口）
 * 每次推理后调用，更新滑动窗口统计
 */
void online_kq_update(KnowledgeQualityTrack* track,
                      int path_len, float avg_conf,
                      int cross_count, int total_steps,
                      int success);

/**
 * 创建知识质量跟踪器
 */
void online_kq_init(KnowledgeQualityTrack* track, int window_size);

/**
 * 获取当前综合质量评分 [0, 1]
 */
float online_kq_score(KnowledgeQualityTrack* track);

// ==================== 综合健康度 API ====================

/**
 * R5: 综合拓扑健康度评分 [0, 100]
 * 加权计算所有可用指标
 */
TopoHealthReport topo_health_report(MasterTopology* master);

/**
 * 打印健康度报告
 */
void topo_health_report_print(const TopoHealthReport* report);

/**
 * 打印所有拓扑评估指标（一站式输出）
 */
void topo_eval_print_all(MasterTopology* master);

// ==================== 辅助工具 ====================

/**
 * 从概念名查找节点（跨拓扑）
 * @return 找到的节点在拓扑中的索引，-1 未找到
 */
int topo_find_node_by_concept(MasterTopology* master, 
                               const char* concept, int* out_topo_id);

/**
 * 计算概念间走边可达性（判断 QA 对是否被"理解"）
 * @return 0 不可达，>0 最短路径长度
 */
int topo_path_exists(MasterTopology* master,
                     int from_topo, int from_node,
                     int to_topo, int to_node, int max_depth);

/**
 * 计算路径的 Jaccard 相似度（用于两路径比较）
 */
float topo_path_jaccard(const int* path1, int len1,
                        const int* path2, int len2);

/**
 * 计算路径的最长公共子序列比例
 */
float topo_path_lcs_ratio(const int* path1, int len1,
                          const int* path2, int len2);

#ifdef __cplusplus
}
#endif

#endif // TOPO_EVAL_H
