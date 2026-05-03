#ifndef CONCEPT_ABSTRACTION_H
#define CONCEPT_ABSTRACTION_H

#include "huarong_topology.h"
#include "multi_topology.h"
#include <stdbool.h>

// ==================== 常量定义 ====================

#define CONCEPT_LEVEL_COUNT 7  // 概念层级数量

// ==================== 概念层级 ====================

/**
 * 概念层级
 */
typedef enum {
    CONCRETE = 0,          // 具体层：具体实例
    SENSORIMTOR,          // 感觉运动层：直接感知
    SPATIAL,              // 空间层：位置、方向
    TEMPORAL,             // 时间层：时序、持续
    CATEGORICAL,          // 分类层：类别、概念
    CAUSAL,               // 因果层：原因、结果
    INTENTIONAL           // 意图层：目标、计划
} ConceptLevel;

/**
 * 概念节点 - 表示抽象概念
 */
typedef struct ConceptNode {
    int concept_id;                // 概念ID
    const char* name;              // 概念名称
    ConceptLevel level;            // 概念层级

    // 关联的具体节点
    int* concrete_nodes;           // 具体节点ID数组
    int concrete_count;            // 具体节点数量
    int concrete_capacity;          // 具体节点容量

    // 概念属性
    float abstraction_strength;    // 抽象强度 (0-1)
    float generalization_degree;   // 泛化程度
    float typicality;               // 典型性 (0-1)

    // 层级关系
    int parent_id;                 // 父概念ID
    int* child_ids;                // 子概念ID数组
    int child_count;               // 子概念数量
    int child_capacity;             // 子概念容量

    // 统计
    float activation;              // 激活值
    time_t created_at;            // 创建时间
    time_t last_accessed;         // 最后访问
} ConceptNode;

/**
 * 概念层次结构
 */
typedef struct ConceptHierarchy {
    ConceptNode** nodes;           // 概念节点数组
    int node_count;               // 节点数量
    int capacity;                 // 容量

    // ID 到索引的映射（加速查找）
    int* id_index;                // id_index[concept_id] = nodes 数组索引

    // 层级信息
    int level_counts[CONCEPT_LEVEL_COUNT];          // 每层节点数

    // 根概念
    int root_concept_id;          // 根概念ID
} ConceptHierarchy;

// ==================== 抽象规则 ====================

/**
 * 抽象规则类型
 */
typedef enum {
    RULE_CLUSTERING = 0,          // 聚类抽象
    RULE_GENERALIZATION,           // 泛化抽象
    RULE_ONTOLOGICAL,             // 本体抽象
    RULE_META_ABSTRACTION          // 元抽象
} AbstractionRuleType;

/**
 * 抽象规则
 */
typedef struct AbstractionRule {
    AbstractionRuleType type;      // 规则类型
    float threshold;              // 触发阈值
    float strength;               // 规则强度
    bool enabled;                 // 是否启用
} AbstractionRule;

// ==================== API 函数声明 ====================

// ========== 概念层级管理 ==========

/**
 * 创建概念层次结构
 * @param capacity 初始容量
 * @return 概念层次结构
 */
ConceptHierarchy* concept_hierarchy_create(int capacity);

/**
 * 销毁概念层次结构
 */
void concept_hierarchy_destroy(ConceptHierarchy* hierarchy);

/**
 * 创建概念节点
 * @param name 概念名称
 * @param level 概念层级
 * @return 概念节点
 */
ConceptNode* concept_node_create(const char* name, ConceptLevel level);

/**
 * 销毁概念节点
 */
void concept_node_destroy(ConceptNode* node);

/**
 * 添加概念节点到层次结构
 * @param hierarchy 层次结构
 * @param concept 概念节点
 * @return 概念ID, -1 失败
 */
int concept_hierarchy_add(ConceptHierarchy* hierarchy, ConceptNode* concept);

/**
 * 获取概念节点
 * @param hierarchy 层次结构
 * @param concept_id 概念ID
 * @return 概念节点
 */
ConceptNode* concept_hierarchy_get(ConceptHierarchy* hierarchy, int concept_id);

/**
 * 设置父子关系
 * @param hierarchy 层次结构
 * @param child_id 子概念ID
 * @param parent_id 父概念ID
 * @return 0 成功
 */
int concept_set_parent(ConceptHierarchy* hierarchy, int child_id, int parent_id);

// ========== 模式提取 ==========

/**
 * 查找共同模式
 * @param net 拓扑网络
 * @param node_ids 节点ID数组
 * @param count 节点数量
 * @param output_patterns 输出模式数组
 * @return 发现的模式数量
 */
int find_common_patterns(HuarongTopologyNet* net, int* node_ids, int count,
                        char*** output_patterns);

/**
 * 提取节点间的共同特征
 * @param net 拓扑网络
 * @param node_ids 节点ID数组
 * @param count 节点数量
 * @param feature_weights 输出特征权重数组
 * @return 共同特征数量
 */
int extract_common_features(HuarongTopologyNet* net, int* node_ids, int count,
                           float* feature_weights);

/**
 * 识别层次模式
 * @param net 拓扑网络
 * @param level 目标层级
 * @param patterns 输出模式数组
 * @return 模式数量
 */
int recognize_hierarchical_patterns(HuarongTopologyNet* net, ConceptLevel level,
                                   char*** patterns);

// ========== 概念层次构建 ==========

/**
 * 构建概念层次
 * @param net 拓扑网络
 * @param hierarchy 概念层次结构
 * @param config 配置参数
 * @return 0 成功
 */
int build_concept_hierarchy(HuarongTopologyNet* net, ConceptHierarchy* hierarchy,
                           void* config);

/**
 * 抽象到高层概念
 * @param net 拓扑网络
 * @param hierarchy 概念层次结构
 * @param concrete_node_ids 具体节点ID数组
 * @param count 节点数量
 * @param target_level 目标层级
 * @return 新概念的ID, -1 失败
 */
int abstract_to_higher_level(HuarongTopologyNet* net, ConceptHierarchy* hierarchy,
                            int* concrete_node_ids, int count,
                            ConceptLevel target_level);

/**
 * 实例化抽象概念
 * @param net 拓扑网络
 * @param hierarchy 概念层次结构
 * @param concept_id 概念ID
 * @param output_node_ids 输出节点ID数组
 * @return 实例化节点数量
 */
int instantiate_concept(HuarongTopologyNet* net, ConceptHierarchy* hierarchy,
                      int concept_id, int** output_node_ids);

/**
 * 细化高层概念
 * @param hierarchy 概念层次结构
 * @param concept_id 概念ID
 * @param specificity 增加的特异性
 * @return 0 成功
 */
int specialize_concept(ConceptHierarchy* hierarchy, int concept_id, float specificity);

// ========== 抽象推理 ==========

/**
 * 基于抽象层次推理
 * @param hierarchy 概念层次结构
 * @param source_concept_id 源概念ID
 * @param target_level 目标层级
 * @return 推理得到的目标概念ID
 */
int reason_via_abstraction(ConceptHierarchy* hierarchy, int source_concept_id,
                          ConceptLevel target_level);

/**
 * 检查概念包含关系
 * @param hierarchy 概念层次结构
 * @param container_id 容器概念ID
 * @param contained_id 被包含概念ID
 * @return true 如果 container 包含 contained
 */
bool concept_contains(ConceptHierarchy* hierarchy, int container_id, int contained_id);

/**
 * 找到最具体的公共祖先
 * @param hierarchy 概念层次结构
 * @param concept_a_id 概念A ID
 * @param concept_b_id 概念B ID
 * @return 公共祖先ID, -1 如果没有
 */
int find_lowest_common_ancestor(ConceptHierarchy* hierarchy, 
                               int concept_a_id, int concept_b_id);

// ========== 抽象规则引擎 ==========

/**
 * 创建抽象规则
 * @param type 规则类型
 * @param threshold 触发阈值
 * @param strength 规则强度
 * @return 抽象规则
 */
AbstractionRule* abstraction_rule_create(AbstractionRuleType type,
                                       float threshold, float strength);

/**
 * 销毁抽象规则
 */
void abstraction_rule_destroy(AbstractionRule* rule);

/**
 * 应用抽象规则
 * @param net 拓扑网络
 * @param hierarchy 概念层次结构
 * @param rule 抽象规则
 * @param node_ids 涉及节点ID
 * @param count 节点数量
 * @return 0 成功应用
 */
int apply_abstraction_rule(HuarongTopologyNet* net, ConceptHierarchy* hierarchy,
                          AbstractionRule* rule, int* node_ids, int count);

// ========== 便捷函数 ==========

/**
 * 获取概念层级名称
 * @param level 概念层级
 * @return 层级名称
 */
const char* concept_level_name(ConceptLevel level);

/**
 * 获取节点的概念层级
 * @param hierarchy 概念层次结构
 * @param node_id 节点ID
 * @return 概念层级
 */
ConceptLevel get_node_concept_level(ConceptHierarchy* hierarchy, int node_id);

/**
 * 计算抽象程度
 * @param hierarchy 概念层次结构
 * @param concept_id 概念ID
 * @return 抽象程度 (0-1)
 */
float compute_abstraction_degree(ConceptHierarchy* hierarchy, int concept_id);

/**
 * 获取层次统计
 * @param hierarchy 概念层次结构
 * @param level_counts 每层节点数数组 (输出)
 * @param avg_abstraction 平均抽象程度 (输出)
 */
void concept_hierarchy_get_stats(ConceptHierarchy* hierarchy, int* level_counts,
                                float* avg_abstraction);

#endif // CONCEPT_ABSTRACTION_H
