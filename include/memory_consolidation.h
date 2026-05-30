#ifndef MEMORY_CONSOLIDATION_H
#define MEMORY_CONSOLIDATION_H

#include "huarong_topology.h"
#include "multi_topology.h"
#include <stdbool.h>

// ==================== 相似度度量 ====================

/**
 * 相似度度量方法
 */
typedef enum {
    SIMILARITY_COSINE = 0,       // 余弦相似度
    SIMILARITY_EUCLIDEAN,         // 欧几里得距离
    SIMILARITY_MANHATTAN,         // 曼哈顿距离
    SIMILARITY_JACCARD,           // Jaccard 相似度
    SIMILARITY_TVERSIFY            // Tversky 相似度
} SimilarityMetric;

/**
 * 相似度比较结果
 */
typedef struct SimilarityResult {
    int node_a_id;               // 节点A ID
    int node_b_id;               // 节点B ID
    float score;                 // 相似度分数 (0-1)
    float confidence;             // 置信度
} SimilarityResult;

// ==================== 巩固配置 ====================

/**
 * 记忆巩固配置
 */
typedef struct ConsolidationConfig {
    // 相似度阈值
    float similarity_threshold;    // 相似度阈值 (超过则合并)
    float merge_ratio;           // 合并强度 (0-1)

    // 频率阈值
    int min_access_count;         // 最小访问次数
    int max_memories_per_cluster; // 每簇最大记忆数

    // 时间衰减
    float decay_rate;             // 衰减率
    float consolidation_threshold; // 巩固阈值

    // 新近度权重
    float recency_weight;         // 新近度权重
    float frequency_weight;       // 频率权重
    float utility_weight;         // 实用性权重

    // 压缩配置
    bool enable_compression;      // 是否启用压缩
    float compression_ratio;      // 压缩比
    int min_cluster_size;         // 最小簇大小
} ConsolidationConfig;

// ==================== 记忆簇 ====================

/**
 * 记忆簇 - 存储相似记忆的组
 */
typedef struct MemoryCluster {
    int cluster_id;              // 簇ID
    int* node_ids;               // 簇内节点ID数组
    int node_count;               // 节点数量
    int capacity;                 // 容量

    // 簇特征
    float* centroid;              // 质心特征向量
    int centroid_dim;             // 质心维度

    // 簇统计
    float avg_importance;         // 平均重要性
    float avg_activation;         // 平均激活值
    int total_access_count;       // 总访问次数
    time_t first_created;         // 最早创建时间
    time_t last_updated;          // 最后更新时间

    // 层次
    int level;                   // 层次 (0=底层)
} MemoryCluster;

/**
 * 记忆簇管理器
 */
typedef struct ClusterManager {
    MemoryCluster** clusters;     // 簇数组
    int cluster_count;           // 簇数量
    int capacity;                 // 容量
    SimilarityMetric metric;      // 相似度度量
    float similarity_threshold;    // 相似度阈值
} ClusterManager;

// ==================== API 函数声明 ====================

// ========== 巩固配置 ==========

/**
 * 创建默认巩固配置
 */
ConsolidationConfig* consolidation_config_create(void);

/**
 * 创建自定义巩固配置
 */
ConsolidationConfig* consolidation_config_create_custom(
    float similarity_threshold, int min_access_count, float decay_rate);

/**
 * 销毁巩固配置
 */
void consolidation_config_destroy(ConsolidationConfig* config);

/**
 * 获取默认配置
 */
ConsolidationConfig* consolidation_get_default_config(void);

/**
 * 设置默认配置
 */
void consolidation_set_default_config(ConsolidationConfig* config);

// ========== 相似度计算 ==========

/**
 * 计算两个节点的相似度
 * @param net 拓扑网络
 * @param node_a_id 节点A ID
 * @param node_b_id 节点B ID
 * @param metric 相似度度量方法
 * @return 相似度分数 (0-1)
 */
float compute_similarity(HuarongTopologyNet* net, int node_a_id, int node_b_id,
                        SimilarityMetric metric);

/**
 * 批量计算相似度
 * @param net 拓扑网络
 * @param node_ids 节点ID数组
 * @param count 节点数量
 * @param metric 相似度度量方法
 * @param results 输出结果数组 (需预先分配)
 * @return 实际结果数量
 */
int compute_similarity_batch(HuarongTopologyNet* net, int* node_ids, int count,
                           SimilarityMetric metric, SimilarityResult* results);

/**
 * 找到最相似的节点对
 * @param net 拓扑网络
 * @param node_id 参考节点ID
 * @param metric 相似度度量方法
 * @param threshold 阈值
 * @param max_results 最大结果数
 * @param output_count 输出结果数
 * @return 相似节点ID数组 (需调用者释放)
 */
int* find_similar_nodes(HuarongTopologyNet* net, int node_id,
                      SimilarityMetric metric, float threshold,
                      int max_results, int* output_count);

// ========== 多维度重要性评估 ==========

/**
 * 计算新近度评分
 * @param node 节点
 * @param current_time 当前时间
 * @param half_life 半衰期 (秒)
 * @param last_used_field 节点的 last_used 字段 (如果有的话)
 * @return 新近度分数 (0-1)
 */
float compute_recency_score(ReasoningNode* node, time_t current_time, float half_life, time_t last_used_field);

/**
 * 计算频率评分
 * @param node 节点
 * @param max_count 历史最大访问次数
 * @return 频率分数 (0-1)
 */
float compute_frequency_score(ReasoningNode* node, int max_count);

/**
 * 计算实用性评分
 * @param node 节点
 * @param avg_activation 平均激活值基线
 * @return 实用性分数 (0-1)
 */
float compute_utility_score(ReasoningNode* node, float avg_activation);

/**
 * 计算显著性评分 (基于激活异常)
 * @param node 节点
 * @param historical_avg 历史平均激活
 * @param std_dev 标准差
 * @return 显著性分数 (0-1)
 */
float compute_salience_score(ReasoningNode* node, float historical_avg, float std_dev);

/**
 * 计算综合遗忘优先级
 * @param node 节点
 * @param current_time 当前时间
 * @param config 巩固配置
 * @return 遗忘优先级 (越高越应该遗忘)
 */
float compute_forgetting_priority(ReasoningNode* node, time_t current_time,
                                  ConsolidationConfig* config);

// ========== 记忆簇管理 ==========

/**
 * 创建簇管理器
 * @param capacity 初始容量
 * @param metric 相似度度量方法
 * @param threshold 相似度阈值
 */
ClusterManager* cluster_manager_create(int capacity, SimilarityMetric metric,
                                     float threshold);

/**
 * 销毁簇管理器
 */
void cluster_manager_destroy(ClusterManager* manager);

/**
 * 创建记忆簇
 */
MemoryCluster* memory_cluster_create(int cluster_id, int initial_capacity);

/**
 * 销毁记忆簇
 */
void memory_cluster_destroy(MemoryCluster* cluster);

/**
 * 添加节点到簇
 * @param cluster 簇
 * @param node_id 节点ID
 * @return 0 成功, -1 失败
 */
int memory_cluster_add_node(MemoryCluster* cluster, int node_id);

/**
 * 从簇移除节点
 * @param cluster 簇
 * @param node_id 节点ID
 * @return 0 成功, -1 未找到
 */
int memory_cluster_remove_node(MemoryCluster* cluster, int node_id);

/**
 * 更新簇质心
 * @param cluster 簇
 * @param net 拓扑网络
 */
void memory_cluster_update_centroid(MemoryCluster* cluster, HuarongTopologyNet* net);

/**
 * 合并两个簇
 * @param manager 簇管理器
 * @param cluster_a_id 簇A ID
 * @param cluster_b_id 簇B ID
 * @return 新簇ID, -1 失败
 */
int memory_cluster_merge(ClusterManager* manager, int cluster_a_id, int cluster_b_id);

/**
 * 拆分簇
 * @param manager 簇管理器
 * @param cluster_id 簇ID
 * @param split_indices 拆分点
 * @param split_count 拆分点数量
 * @return 0 成功
 */
int memory_cluster_split(ClusterManager* manager, int cluster_id,
                        int* split_indices, int split_count);

// ========== 记忆巩固 ==========

/**
 * 查找可合并的相似记忆
 * @param net 拓扑网络
 * @param manager 簇管理器
 * @param threshold 相似度阈值
 * @param output_count 输出结果数
 * @return 相似对数组 (需调用者释放)
 */
SimilarityResult* find_similar_memories(HuarongTopologyNet* net,
                                       ClusterManager* manager,
                                       float threshold,
                                       int* output_count);

/**
 * 合并两个记忆节点
 * @param net 拓扑网络
 * @param node_a_id 节点A ID
 * @param node_b_id 节点B ID
 * @param merge_ratio 合并强度
 * @return 合并后节点ID, -1 失败
 */
int merge_memories(HuarongTopologyNet* net, int node_a_id, int node_b_id,
                  float merge_ratio);

/**
 * 压缩记忆簇
 * @param net 拓扑网络
 * @param manager 簇管理器
 * @param cluster_id 簇ID
 * @param compression_ratio 压缩比
 * @return 压缩后保留的节点数
 */
int compress_memory_cluster(HuarongTopologyNet* net, ClusterManager* manager,
                           int cluster_id, float compression_ratio);

/**
 * 执行记忆巩固
 * @param net 拓扑网络
 * @param manager 簇管理器
 * @param config 巩固配置
 * @return 巩固的簇数量
 */
int execute_consolidation(HuarongTopologyNet* net, ClusterManager* manager,
                         ConsolidationConfig* config);

// ========== 遗忘机制 ==========

/**
 * 选择遗忘候选节点
 * @param net 拓扑网络
 * @param config 巩固配置
 * @param count 输出节点数量
 * @return 遗忘候选节点ID数组 (需调用者释放)
 */
int* select_forgetting_candidates(HuarongTopologyNet* net,
                                 ConsolidationConfig* config,
                                 int* count);

/**
 * 执行遗忘
 * @param net 拓扑网络
 * @param node_id 节点ID
 * @param forgetting_strength 遗忘强度 (0-1)
 * @return 0 成功
 */
int execute_forgetting(HuarongTopologyNet* net, int node_id, float forgetting_strength);

/**
 * 自适应衰减调度
 * @param net 拓扑网络
 * @param base_decay 基础衰减率
 * @param importance 节点重要性
 * @param time_delta 时间间隔
 * @return 自适应衰减率
 */
float adaptive_decay_schedule(float base_decay, float importance, float time_delta);

// ========== 便捷函数 ==========

/**
 * 获取簇统计信息
 * @param cluster 簇
 * @param avg_importance 平均重要性 (输出)
 * @param avg_activation 平均激活值 (输出)
 * @param density 簇密度 (输出)
 */
void memory_cluster_get_stats(MemoryCluster* cluster, float* avg_importance,
                             float* avg_activation, float* density);

/**
 * 获取管理器统计
 * @param manager 簇管理器
 * @param total_clusters 总簇数
 * @param total_nodes 总节点数
 * @param avg_cluster_size 平均簇大小
 */
void cluster_manager_get_stats(ClusterManager* manager, int* total_clusters,
                             int* total_nodes, float* avg_cluster_size);

#endif // MEMORY_CONSOLIDATION_H
