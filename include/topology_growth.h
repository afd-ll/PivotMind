#ifndef TOPOLOGY_GROWTH_H
#define TOPOLOGY_GROWTH_H

#include "multi_topology.h"
#include "node_importance.h"
#include <stdbool.h>

// ==================== 拓扑动态演化配置 ====================

/**
 * 增长触发条件
 */
typedef enum {
    GROWTH_TRIGGER_NONE = 0,         // 不触发
    GROWTH_TRIGGER_NODE_COUNT,       // 节点数量阈值
    GROWTH_TRIGGER_LINK_DENSITY,     // 连接密度
    GROWTH_TRIGGER_LOAD_FACTOR,      // 负载因子
    GROWTH_TRIGGER_ACTIVATION_PRESSURE  // 激活压力
} GrowthTriggerType;

/**
 * 增长触发条件配置
 */
typedef struct GrowthTrigger {
    GrowthTriggerType type;         // 触发类型
    float threshold;                // 阈值
    float hysteresis;               // 滞后系数 (防止震荡)
    int cooldown_ticks;            // 冷却时间 (防止频繁触发)
    time_t last_triggered;          // 上次触发时间
} GrowthTrigger;

/**
 * 增长配置
 */
typedef struct TopologyGrowthConfig {
    // 节点容量
    int max_nodes_per_topology;     // 每拓扑最大节点数
    int growth_increment;           // 每次增长数量

    // 连接容量
    int max_connections_per_node;   // 每节点最大连接数
    float min_connection_weight;    // 最小连接权重

    // 触发条件
    GrowthTrigger node_count_trigger;
    GrowthTrigger density_trigger;
    GrowthTrigger load_trigger;

    // 增长限制
    float max_growth_rate;          // 最大增长率 (节点/秒)
    int max_nodes_per_hour;         // 每小时最大新增节点数
    int max_topologies;             // 最大拓扑数

    // 自适应参数
    float auto_shrink_enabled;      // 是否启用自动收缩
    float shrink_threshold;         // 收缩阈值
    int idle_before_shrink;         // 收缩前空闲时间

    // 学习率
    float learning_rate;            // 动态权重学习率
    float connection_decay;         // 连接衰减率
} TopologyGrowthConfig;

/**
 * 权值更新策略
 */
typedef enum {
    WEIGHT_UPDATE_HEBBIAN = 0,      // 赫布规则
    WEIGHT_UPDATE_GRADIENT,         // 梯度下降
    WEIGHT_UPDATE_RULE_BASED,       // 规则驱动
    WEIGHT_UPDATE_HYBRID            // 混合策略
} WeightUpdatePolicy;

/**
 * 动态演化统计
 */
typedef struct GrowthStats {
    long total_node_insertions;     // 总节点插入数
    long total_node_removals;       // 总节点删除数
    long total_edge_insertions;     // 总边插入数
    long total_edge_removals;       // 总边删除数
    long total_growth_events;       // 总增长事件数
    long total_shrink_events;       // 总收缩事件数
    int current_node_count;         // 当前节点数
    int peak_node_count;            // 峰值节点数
    float current_density;          // 当前密度
    float avg_importance;           // 平均重要性
    time_t last_growth;             // 上次增长时间
    time_t last_shrink;             // 上次收缩时间
} GrowthStats;

// ==================== API 函数声明 ====================

// ========== 配置管理 ==========

/**
 * 创建默认增长配置
 * @return 新配置 (需调用者释放)
 */
TopologyGrowthConfig* topology_growth_config_create(void);

/**
 * 创建自定义增长配置
 * @param max_nodes 最大节点数
 * @param max_connections 最大连接数
 * @param growth_increment 每次增长数量
 * @return 新配置 (需调用者释放)
 */
TopologyGrowthConfig* topology_growth_config_create_custom(
    int max_nodes, int max_connections, int growth_increment);

/**
 * 销毁增长配置
 */
void topology_growth_config_destroy(TopologyGrowthConfig* config);

/**
 * 获取默认全局配置
 */
TopologyGrowthConfig* topology_growth_get_default_config(void);

/**
 * 设置全局配置
 */
void topology_growth_set_default_config(TopologyGrowthConfig* config);

// ========== 动态节点操作 ==========

/**
 * 动态插入节点
 * @param master 主拓扑
 * @param topo_id 目标拓扑ID
 * @param concept 概念名称
 * @param features 特征向量 (可为 NULL)
 * @param feature_dim 特征维度
 * @return 新节点ID, -1 表示失败
 */
int insert_node_dynamic(MasterTopology* master, int topo_id,
                       const char* concept, float* features, int feature_dim);

/**
 * 动态删除节点
 * @param master 主拓扑
 * @param topo_id 拓扑ID
 * @param node_id 节点ID
 * @param force 是否强制删除 (即使有连接)
 * @return 0 成功, -1 失败
 */
int remove_node_dynamic(MasterTopology* master, int topo_id,
                      int node_id, bool force);

/**
 * 批量插入节点
 * @param master 主拓扑
 * @param topo_id 目标拓扑ID
 * @param concepts 概念名称数组
 * @param count 数量
 * @return 成功插入数量
 */
int insert_nodes_batch(MasterTopology* master, int topo_id,
                     const char** concepts, int count);

// ========== 动态边操作 ==========

/**
 * 动态添加边
 * @param master 主拓扑
 * @param topo_id 拓扑ID
 * @param from_node_id 起始节点
 * @param to_node_id 目标节点
 * @param weight 权重
 * @return 0 成功, -1 失败
 */
int add_edge_dynamic(MasterTopology* master, int topo_id,
                    int from_node_id, int to_node_id, float weight);

/**
 * 动态删除边
 * @param master 主拓扑
 * @param topo_id 拓扑ID
 * @param from_node_id 起始节点
 * @param to_node_id 目标节点
 * @return 0 成功, -1 失败
 */
int remove_edge_dynamic(MasterTopology* master, int topo_id,
                      int from_node_id, int to_node_id);

/**
 * 更新边权重
 * @param master 主拓扑
 * @param topo_id 拓扑ID
 * @param from_node_id 起始节点
 * @param to_node_id 目标节点
 * @param new_weight 新权重
 * @return 0 成功, -1 失败
 */
int update_edge_weight(MasterTopology* master, int topo_id,
                      int from_node_id, int to_node_id, float new_weight);

// ========== 自动拓扑增长 ==========

/**
 * 检查是否需要增长
 * @param master 主拓扑
 * @param topo_id 拓扑ID (-1 表示检查主拓扑)
 * @return true 需要增长
 */
bool check_growth_needed(MasterTopology* master, int topo_id);

/**
 * 执行自动拓扑增长
 * @param master 主拓扑
 * @param topo_id 拓扑ID
 * @return 增长后节点数, -1 失败
 */
int auto_extend_topology(MasterTopology* master, int topo_id);

/**
 * 执行自动拓扑收缩
 * @param master 主拓扑
 * @param topo_id 拓扑ID
 * @return 收缩后节点数, -1 失败
 */
int auto_shrink_topology(MasterTopology* master, int topo_id);

/**
 * 负载均衡
 * @param master 主拓扑
 * @return 0 成功, -1 失败
 */
int topology_load_balancing(MasterTopology* master);

// ========== 重要性剪枝 ==========

/**
 * 基于重要性的节点剪枝
 * @param master 主拓扑
 * @param topo_id 拓扑ID
 * @param min_importance 最低重要性阈值
 * @param dry_run 是否只分析不执行
 * @return 删除的节点数
 */
int prune_node_importance(MasterTopology* master, int topo_id,
                         float min_importance, bool dry_run);

/**
 * 移除低连接节点
 * @param master 主拓扑
 * @param topo_id 拓扑ID
 * @param min_connections 最小连接数
 * @return 删除的节点数
 */
int prune_low_connectivity(MasterTopology* master, int topo_id,
                          int min_connections);

/**
 * 移除孤立节点
 * @param master 主拓扑
 * @param topo_id 拓扑ID
 * @return 删除的节点数
 */
int prune_isolated_nodes(MasterTopology* master, int topo_id);

// ========== 动态权重更新 ==========

/**
 * 动态权重更新
 * @param master 主拓扑
 * @param topo_id 拓扑ID
 * @param policy 更新策略
 * @return 0 成功, -1 失败
 */
int dynamic_weight_update(MasterTopology* master, int topo_id,
                         WeightUpdatePolicy policy);

/**
 * 自适应学习率
 * @param master 主拓扑
 * @param topo_id 拓扑ID
 * @return 自适应学习率
 */
float adaptive_learning_rate(MasterTopology* master, int topo_id);

/**
 * 连接强度衰减
 * @param master 主拓扑
 * @param topo_id 拓扑ID
 * @param decay_factor 衰减因子
 * @return 衰减后平均连接强度
 */
float connection_strength_decay(MasterTopology* master, int topo_id,
                               float decay_factor);

// ========== 跨拓扑动态操作 ==========

/**
 * 动态添加跨拓扑链接
 * @param master 主拓扑
 * @param from_topo_id 源拓扑ID
 * @param from_node_id 源节点ID
 * @param to_topo_id 目标拓扑ID
 * @param to_node_id 目标节点ID
 * @param weight 权重
 * @param relation 关系类型
 * @return 0 成功, -1 失败
 */
int insert_cross_topology_link(MasterTopology* master,
                             int from_topo_id, int from_node_id,
                             int to_topo_id, int to_node_id,
                             float weight, const char* relation);

/**
 * 动态移除跨拓扑链接
 * @param master 主拓扑
 * @param from_topo_id 源拓扑ID
 * @param from_node_id 源节点ID
 * @param to_topo_id 目标拓扑ID
 * @param to_node_id 目标节点ID
 * @return 0 成功, -1 失败
 */
int remove_cross_topology_link(MasterTopology* master,
                              int from_topo_id, int from_node_id,
                              int to_topo_id, int to_node_id);

// ========== 统计与监控 ==========

/**
 * 获取增长统计
 * @param master 主拓扑
 * @return 统计信息 (只读)
 */
const GrowthStats* topology_growth_get_stats(MasterTopology* master);

/**
 * 重置统计
 * @param master 主拓扑
 */
void topology_growth_reset_stats(MasterTopology* master);

/**
 * 获取拓扑健康度
 * @param master 主拓扑
 * @param topo_id 拓扑ID
 * @return 健康度评分 (0-1)
 */
float topology_health_score(MasterTopology* master, int topo_id);

/**
 * 诊断拓扑状态
 * @param master 主拓扑
 * @param topo_id 拓扑ID
 * @param report 输出报告 (可为 NULL)
 * @return 状态码 (0=健康, 1=需增长, 2=需收缩, 3=异常)
 */
int diagnose_topology(MasterTopology* master, int topo_id, char* report);

#endif // TOPOLOGY_GROWTH_H
