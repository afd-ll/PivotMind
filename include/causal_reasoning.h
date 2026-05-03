#ifndef CAUSAL_REASONING_H
#define CAUSAL_REASONING_H

#include "huarong_topology.h"
#include "multi_topology.h"
#include <stdbool.h>

// ==================== 因果图结构 ====================

/**
 * 因果边类型
 */
typedef enum {
    CAUSAL_DIRECT = 0,        // 直接因果
    CAUSAL_INDIRECT,          // 间接因果
    CAUSAL_SPURIOUS,         // 伪因果（混淆）
    CAUSAL_CONDITIONAL,       // 条件因果
    CAUSAL_FEEDBACK           // 反馈因果
} CausalEdgeType;

/**
 * 因果置信度级别
 */
typedef enum {
    CAUSAL_CONF_CONTEXT = 0,   // 上下文记忆 (< 0.3)
    CAUSAL_CONF_SHORT_TERM = 1, // 短期记忆 (0.3 - 0.6)
    CAUSAL_CONF_PERMANENT = 2,  // 永久记忆 (0.6 - 0.8)
    CAUSAL_CONF_CORE = 3        // 核心知识 (>= 0.8)
} CausalConfidenceLevel;

/**
 * 因果置信度详细信息
 */
typedef struct CausalConfidence {
    float base_score;          // 基础置信度 (0-1)
    int observation_count;     // 观察次数
    int valid_scenarios;      // 不同有效场景数
    int total_scenarios;      // 总场景数
    int consistent_count;      // 方向一致次数
    int total_tests;           // 总测试次数
    time_t first_observed;     // 首次观察时间
    time_t last_confirmed;     // 最后确认时间
} CausalConfidence;

/**
 * 因果边
 */
typedef struct CausalEdge {
    int edge_id;              // 边ID
    int cause_node_id;        // 原因节点ID
    int effect_node_id;       // 效果节点ID
    CausalEdgeType type;      // 边类型
    float strength;            // 因果强度 (0-1)
    float probability;         // 条件概率 P(effect|cause)
    bool bidirectional;        // 是否双向

    // 条件信息
    int* condition_node_ids;  // 条件节点ID数组
    int condition_count;      // 条件数量

    // 统计
    float confidence;          // 置信度
    time_t observed_at;       // 首次观察时间
    time_t last_confirmed;     // 最后确认时间
} CausalEdge;

/**
 * 因果图
 */
typedef struct CausalGraph {
    // 节点映射 (从原始拓扑网络)
    int* node_mapping;        // 因果图节点 -> 拓扑节点
    int node_count;           // 因果图节点数
    int topo_node_count;      // 原始拓扑节点数

    // 边
    CausalEdge** edges;       // 边数组
    int edge_count;          // 边数量
    int edge_capacity;        // 边容量

    // 邻接表
    int** outgoing;           // 出边邻接表 [from] -> [to1, to2, ...]
    int* outgoing_count;      // 每个节点的出边数
    int** incoming;           // 入边邻接表 [to] -> [from1, from2, ...]
    int* incoming_count;      // 每个节点的入边数

    // 结构信息
    bool is_dag;             // 是否为有向无环图
    int* topological_order;  // 拓扑序
    int order_length;         // 拓扑序长度

    // 统计
    float avg_causal_strength; // 平均因果强度
    time_t last_updated;      // 最后更新时间

    // 内存池（用于优化邻接表分配）
    void* adj_pool;          // 邻接表内存池
    void* edge_pool;          // 边内存池
} CausalGraph;

// ==================== 干预操作 ====================

/**
 * 干预操作类型
 */
typedef enum {
    INTERVENTION_DO = 0,       // do(X=x): 强制设置X为x
    INTERVENTION_SET,         // set(X=x): 观察性设置
    INTERVENTION_REMOVE,       // remove(X): 移除X的影响
    INTERVENTION_ADD           // add(X->Y): 添加因果边
} InterventionType;

/**
 * 干预操作
 */
typedef struct Intervention {
    InterventionType type;    // 干预类型
    int target_node_id;       // 目标节点
    float new_value;         // 新值 (用于 do/set)
    int source_node_id;       // 源节点 (用于 add)
    float strength;           // 强度 (用于 add)

    // 条件
    int* condition_node_ids;  // 条件节点数组
    int condition_count;      // 条件数量
    float* condition_values;  // 条件值数组
} Intervention;

// ==================== 因果效应 ====================

/**
 * 因果效应类型
 */
typedef enum {
    EFFECT_ATE = 0,          // Average Treatment Effect (ATE)
    EFFECT_ATT,              // Average Treatment Effect on Treated (ATT)
    EFFECT_CATE,             // Conditional Average Treatment Effect (CATE)
    EFFECT_DIRECT,            // 直接效应
    EFFECT_INDIRECT,         // 间接效应
    EFFECT_TOTAL             // 总效应
} CausalEffectType;

/**
 * 因果效应
 */
typedef struct CausalEffect {
    CausalEffectType type;    // 效应类型
    int cause_node_id;       // 原因节点
    int effect_node_id;       // 效果节点

    // 效应值
    float effect_size;       // 效应大小
    float standard_error;     // 标准误差
    float confidence_interval_low;  // 置信区间下限
    float confidence_interval_high; // 置信区间上限

    // 统计
    float p_value;           // p值
    float confidence;         // 置信度
    int sample_size;          // 样本量
} CausalEffect;

// ==================== 反事实推理 ====================

/**
 * 反事实查询
 */
typedef struct CounterfactualQuery {
    int query_id;            // 查询ID
    int outcome_node_id;      // 结果节点
    float observed_outcome;   // 观察到的结果
    float hypothetical_action; // 假设动作
    int action_node_id;       // 动作作用节点

    // 背景上下文
    int* context_node_ids;   // 上下文节点数组
    float* context_values;   // 上下文值数组
    int context_count;       // 上下文数量

    // 预期结果
    float expected_outcome;  // 预期结果
    float probability;       // 概率
} CounterfactualQuery;

/**
 * 反事实结果
 */
typedef struct CounterfactualResult {
    int query_id;            // 查询ID
    float counterfactual_value;      // 反事实值
    float probability;       // 概率
    float explanation_score;  // 解释分数
    char* explanation;       // 解释文本
} CounterfactualResult;

// ==================== 因果路径 ====================

/**
 * 因果路径
 */
typedef struct CausalPath {
    int* node_ids;           // 路径节点ID数组
    int length;              // 路径长度
    float total_strength;    // 总因果强度
    float* edge_strengths;   // 各边强度

    // 路径类型
    bool is_direct;          // 是否直接路径
    bool is_backdoor;        // 是否为后门路径
    bool has_confounder;      // 是否有混淆因素
} CausalPath;

// ==================== 因果规则泛化 ====================

/**
 * 因果模式（用于泛化）
 */
typedef struct CausalPattern {
    char* cause_template;       // 原因模板，如 "*负载高"
    char* effect_template;      // 效果模板，如 "*温度"
    float strength;             // 模式强度
    float confidence;           // 模式置信度
    int instance_count;         // 支持实例数
    char** instance_cause;     // 支持的原因实例
    char** instance_effect;    // 支持的效果实例
    int max_instances;          // 最大实例数
} CausalPattern;

/**
 * 泛化查询
 */
typedef struct {
    char* cause;               // 具体原因
    char* effect;              // 具体效果
    float matched_strength;    // 匹配到的强度
    CausalPattern* matched_pattern;  // 匹配到的模式
} GeneralizationMatch;

// ==================== 因果置信度函数 ====================

/**
 * 创建因果置信度结构
 * @param base_score 基础置信度
 * @return 因果置信度结构
 */
CausalConfidence* causal_confidence_create(float base_score);

/**
 * 计算因果置信度
 * @param cc 置信度结构
 * @return 置信度值 (0-1)
 */
float compute_causal_confidence(CausalConfidence* cc);

/**
 * 更新因果置信度
 * @param cc 置信度结构
 * @param supports 新观察是否支持该因果
 * @param scenario_id 场景ID
 */
void update_causal_confidence(CausalConfidence* cc, bool supports, int scenario_id);

/**
 * 获取置信度级别
 * @param confidence 置信度值
 * @return 置信度级别
 */
CausalConfidenceLevel get_confidence_level(float confidence);

/**
 * 因果置信度衰减
 * @param cc 置信度结构
 * @return 衰减后的置信度
 */
float decay_causal_confidence(CausalConfidence* cc);

/**
 * 释放因果置信度结构
 * @param cc 置信度结构
 */
void causal_confidence_destroy(CausalConfidence* cc);

// ========== 因果规则泛化 API ==========

/**
 * 从具体因果关系创建因果模式
 * @param cause 具体原因
 * @param effect 具体效果
 * @param strength 因果强度
 * @return 因果模式
 */
CausalPattern* causal_pattern_create(const char* cause, const char* effect,
                                   float strength);

/**
 * 释放因果模式
 * @param pattern 因果模式
 */
void causal_pattern_destroy(CausalPattern* pattern);

/**
 * 添加实例到因果模式
 * @param pattern 因果模式
 * @param cause 具体原因
 * @param effect 具体效果
 * @param strength 因果强度
 * @return 0 成功
 */
int causal_pattern_add_instance(CausalPattern* pattern, const char* cause,
                               const char* effect, float strength);

/**
 * 匹配具体因果关系到模式
 * @param pattern 模式
 * @param cause 具体原因
 * @param effect 具体效果
 * @return 匹配得分 0-1
 */
float causal_pattern_match(CausalPattern* pattern, const char* cause,
                         const char* effect);

/**
 * 泛化查询
 * @param patterns 模式数组
 * @param pattern_count 模式数量
 * @param cause 具体原因
 * @param effect 具体效果
 * @param match 输出匹配结果
 * @return 匹配数量
 */
int causal_generalize(CausalPattern** patterns, int pattern_count,
                     const char* cause, const char* effect,
                     GeneralizationMatch* match);

/**
 * 从示例学习因果模式
 * @param causes 原因数组
 * @param effects 效果数组
 * @param strengths 强度数组
 * @param count 示例数量
 * @param out_pattern_count 输出模式数量
 * @return 模式数组
 */
CausalPattern** learn_causal_patterns_from_examples(const char** causes,
                                                 const char** effects,
                                                 float* strengths,
                                                 int count,
                                                 int* out_pattern_count);

/**
 * 释放泛化匹配结果
 * @param matches 匹配结果
 * @param count 匹配数量
 */
void free_generalization_matches(GeneralizationMatch* matches, int count);

// ==================== API 函数声明 ====================

// ========== 因果图管理 ==========

/**
 * 创建因果图
 * @param node_count 初始节点数
 * @param edge_capacity 初始边容量
 * @return 因果图
 */
CausalGraph* causal_graph_create(int node_count, int edge_capacity);

/**
 * 销毁因果图
 */
void causal_graph_destroy(CausalGraph* graph);

/**
 * 添加因果边
 * @param graph 因果图
 * @param cause_id 原因节点ID
 * @param effect_id 效果节点ID
 * @param type 边类型
 * @param strength 因果强度
 * @return 边ID, -1 失败
 */
int add_causal_edge(CausalGraph* graph, int cause_id, int effect_id,
                  CausalEdgeType type, float strength);

/**
 * 移除因果边
 * @param graph 因果图
 * @param cause_id 原因节点ID
 * @param effect_id 效果节点ID
 * @return 0 成功
 */
int remove_causal_edge(CausalGraph* graph, int cause_id, int effect_id);

/**
 * 获取因果边
 * @param graph 因果图
 * @param cause_id 原因节点ID
 * @param effect_id 效果节点ID
 * @return 因果边, NULL 未找到
 */
CausalEdge* get_causal_edge(CausalGraph* graph, int cause_id, int effect_id);

/**
 * 检查边是否存在
 * @param graph 因果图
 * @param cause_id 原因节点ID
 * @param effect_id 效果节点ID
 * @return true 存在
 */
bool causal_edge_exists(CausalGraph* graph, int cause_id, int effect_id);

// ========== 从拓扑网络构建因果图 ==========

/**
 * 从拓扑网络推断因果图
 * @param topo_net 拓扑网络
 * @param min_strength 最小因果强度阈值
 * @return 因果图, NULL 失败
 */
CausalGraph* infer_causal_graph_from_topology(HuarongTopologyNet* topo_net,
                                             float min_strength);

/**
 * 从观察数据学习因果边
 * @param graph 因果图
 * @param observations 观察数据数组
 * @param obs_count 观察数量
 * @param feature_dim 特征维度
 * @return 学习到的边数
 */
int learn_causal_edges(CausalGraph* graph, float** observations,
                      int obs_count, int feature_dim);

// ========== 因果效应计算 ==========

/**
 * 执行干预操作
 * @param graph 因果图
 * @param intervention 干预操作
 * @param output_values 输出值数组 (需预先分配)
 * @return 0 成功
 */
int do_intervention(CausalGraph* graph, Intervention* intervention,
                  float* output_values);

/**
 * 计算因果效应
 * @param graph 因果图
 * @param cause_id 原因节点
 * @param effect_id 效果节点
 * @param effect_type 效应类型
 * @return 因果效应结构
 */
CausalEffect* compute_causal_effect(CausalGraph* graph, int cause_id,
                                   int effect_id, CausalEffectType effect_type);

/**
 * 后门调整
 * @param graph 因果图
 * @param cause_id 原因节点
 * @param effect_id 效果节点
 * @param confounders 混淆因素节点数组
 * @param confounder_count 混淆因素数量
 * @return 调整后的因果效应
 */
float backdoor_adjustment(CausalGraph* graph, int cause_id, int effect_id,
                         int* confounders, int confounder_count);

/**
 * 前门调整
 * @param graph 因果图
 * @param cause_id 原因节点
 * @param effect_id 效果节点
 * @param mediator_id 中介节点
 * @return 调整后的因果效应
 */
float frontdoor_adjustment(CausalGraph* graph, int cause_id, int effect_id,
                          int mediator_id);

// ========== 因果路径分析 ==========

/**
 * 找到所有因果路径
 * @param graph 因果图
 * @param source 源节点
 * @param target 目标节点
 * @param max_length 最大路径长度
 * @param path_count 输出路径数量
 * @return 路径数组 (需调用者释放)
 */
CausalPath** find_all_causal_paths(CausalGraph* graph, int source,
                                   int target, int max_length, int* path_count);

/**
 * 找到直接因果路径
 * @param graph 因果图
 * @param source 源节点
 * @param target 目标节点
 * @return 直接路径, NULL 无直接路径
 */
CausalPath* find_direct_causal_path(CausalGraph* graph, int source, int target);

/**
 * 使用 A* 算法查找因果路径（优化版）
 * @param graph 因果图
 * @param source 源节点
 * @param target 目标节点
 * @param max_length 最大路径长度
 * @param max_paths 最大返回路径数量
 * @param path_count 输出路径数量
 * @return 路径数组 (需调用者释放)
 */
CausalPath** find_causal_paths_astar(CausalGraph* graph, int source, int target,
                                     int max_length, int max_paths, int* path_count);

/**
 * 使用 A* 查找最短因果路径
 * @param graph 因果图
 * @param source 源节点
 * @param target 目标节点
 * @param max_length 最大路径长度
 * @return 最短路径, NULL 未找到
 */
CausalPath* find_shortest_causal_path(CausalGraph* graph, int source, int target,
                                     int max_length);

/**
 * 计算路径因果效应
 * @param graph 因果图
 * @param path 因果路径
 * @return 路径效应
 */
float compute_path_effect(CausalGraph* graph, CausalPath* path);

/**
 * 检测混淆因素
 * @param graph 因果图
 * @param cause_id 原因节点
 * @param effect_id 效果节点
 * @param confounders 输出混淆因素数组
 * @return 混淆因素数量
 */
int detect_confounders(CausalGraph* graph, int cause_id, int effect_id,
                      int** confounders);

// ========== 反事实推理 ==========

/**
 * 计算反事实
 * @param graph 因果图
 * @param query 反事实查询
 * @return 反事实结果
 */
CounterfactualResult* compute_counterfactual(CausalGraph* graph,
                                          CounterfactualQuery* query);

/**
 * 推断-行动-更新 (Abduction-Action-Update)
 * @param graph 因果图
 * @param observed 观察值
 * @param action 动作
 * @param outcome_var 结果变量
 * @return 预测结果
 */
float abduction_action_update(CausalGraph* graph, float* observed,
                            Intervention* action, int outcome_var);

/**
 * 生成反事实解释
 * @param graph 因果图
 * @param result 反事实结果
 * @param query 反事实查询
 * @return 解释文本 (需调用者释放)
 */
char* generate_counterfactual_explanation(CausalGraph* graph,
                                       CounterfactualResult* result,
                                       CounterfactualQuery* query);

// ========== 因果结构学习 ==========

/**
 * PC 算法 (简化版)
 * @param graph 因果图
 * @param data 数据矩阵
 * @param n 样本数
 * @param d 维度
 * @param alpha 显著性水平
 * @return 0 成功
 */
int pc_algorithm(CausalGraph* graph, float** data, int n, int d, float alpha);

/**
 * PC 算法并行版本 (使用 OpenMP)
 * @param graph 因果图
 * @param data 数据矩阵 [n x d]
 * @param n 样本数
 * @param d 变量数
 * @param alpha 显著性水平
 * @return 0 成功
 */
int pc_algorithm_parallel(CausalGraph* graph, float** data, int n, int d, float alpha);

/**
 * 获取并行线程数
 * @return CPU 核心数或 OpenMP 最大线程数
 */
int get_parallel_thread_count(void);

/**
 * PCG 算法 (用于大规模图)
 * @param graph 因果图
 * @param graph_prior 先验图结构 (可为 NULL)
 * @param data 数据矩阵
 * @param n 样本数
 * @param d 维度
 * @return 0 成功
 */
int pcg_algorithm(CausalGraph* graph, CausalGraph* graph_prior,
                 float** data, int n, int d);

// ========== 便捷函数 ==========

/**
 * 检查因果图是否为 DAG
 * @param graph 因果图
 * @return true 如果是 DAG
 */
bool is_dag(CausalGraph* graph);

/**
 * 计算两个节点间的因果效应
 * @param graph 因果图
 * @param source 源节点
 * @param target 目标节点
 * @return 因果效应值
 */
float total_causal_effect(CausalGraph* graph, int source, int target);

/**
 * 获取因果图的邻接矩阵
 * @param graph 因果图
 * @param matrix 输出矩阵 (需预先分配)
 */
void causal_graph_to_adjacency_matrix(CausalGraph* graph, float* matrix);

/**
 * 获取节点的父母节点
 * @param graph 因果图
 * @param node_id 节点ID
 * @param parents 输出父母节点数组
 * @return 父母节点数量
 */
int get_parent_nodes(CausalGraph* graph, int node_id, int** parents);

/**
 * 获取节点的子女节点
 * @param graph 因果图
 * @param node_id 节点ID
 * @param children 输出子女节点数组
 * @return 子女节点数量
 */
int get_child_nodes(CausalGraph* graph, int node_id, int** children);

/**
 * 获取因果图统计信息
 * @param graph 因果图
 * @param node_count 节点数 (输出)
 * @param edge_count 边数 (输出)
 * @param avg_degree 平均度 (输出)
 * @param density 密度 (输出)
 */
void causal_graph_get_stats(CausalGraph* graph, int* node_count, int* edge_count,
                          float* avg_degree, float* density);

// ========== 因果知识固化 ==========

/**
 * 保存因果图到文件（JSON格式）
 * @param graph 因果图
 * @param filepath 文件路径
 * @return 0 成功
 */
int causal_graph_save_to_file(CausalGraph* graph, const char* filepath);

/**
 * 从文件加载因果图（JSON格式）
 * @param filepath 文件路径
 * @return 因果图，NULL 失败
 */
CausalGraph* causal_graph_load_from_file(const char* filepath);

/**
 * 保存因果知识到 SQLite 数据库
 * @param graph 因果图
 * @param db_path 数据库路径
 * @return 0 成功
 */
int causal_graph_save_to_db(CausalGraph* graph, const char* db_path);

/**
 * 从 SQLite 数据库加载因果知识
 * @param db_path 数据库路径
 * @param node_count 节点数量
 * @return 因果图，NULL 失败
 */
CausalGraph* causal_graph_load_from_db(const char* db_path, int node_count);

#endif // CAUSAL_REASONING_H
