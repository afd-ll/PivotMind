#ifndef HUARONG_TOPOLOGY_H
#define HUARONG_TOPOLOGY_H

#include "constants.h"
#include "cognitive_params.h"
#include <pthread.h>

/** 前置声明（避免循环包含 node_hash.h） */
typedef struct NodeHashTable NodeHashTable;

// ==================== 节点类型枚举 ==================== 
typedef enum {
    NODE_TYPE_FUNCTION_WORD = 0,  // 功能词（的、了、是）— 衰减慢
    NODE_TYPE_COMMON_WORD  = 1,  // 普通词 — 正常衰减
    NODE_TYPE_PROPER_NOUN  = 2   // 专有名词 — 衰减快，鼓励替换
} NodeType;

// ==================== 华容道拓扑神经网络核心结构 ==================== 

/**
 * 推理节点（类比华容道的滑块）
 * 代表知识图谱中的一个概念节点
 * 
 * 集成认知架构参数:
 * - confidence: 三维置信度 (predictive_accuracy, user_satisfaction, novelty_bonus)
 * - valence: 效价 (-1.0 ~ +1.0)
 * - connection_weights: logical_strength (逻辑强度)
 * - connection_motivational_bias: 动机倾向 (新增)
 */
typedef struct ReasoningNode {
    int node_id;               // 节点唯一标识
    char* concept;             // 概念名称/描述
    float* features;           // 特征向量
    int feature_dim;           // 特征维度
    
    // 连接边
    struct ReasoningNode** connections; // 连接的节点数组
    int connection_capacity;           // 连接数组容量
    float* connection_weights; // 连接权重 (logical_strength)
    float* connection_motivational_bias; // 动机倾向 (新增)
    float* connection_confidences; // 边置信度
    int connection_count;      // 连接数量
    
    // 节点状态
    float activation;          // 当前激活值
    float confidence;          // 节点置信度 (兼容旧代码)
    
    // 新增: 三维置信度
    CognitiveConfidence* cognitive_confidence;  // 认知+情感+探索
    
    // 新增: 效价
    float valence;            // 效价 (-1.0 ~ +1.0)
    
    // 新增: 热度衰减（路径多样性）
    float heat;                // 当前热度 (0.0-1.0)，被选次数越多值越低
    int selection_count;       // 被贪心走边选中的累计次数
    NodeType node_type;        // 节点类型（功能词/普通词/专有名词）
    
    // 元信息
    int is_reversible;         // 是否支持可逆操作
    int is_visited;            // 搜索过程中是否已访问
} ReasoningNode;

/**
 * 华容道拓扑网络主结构
 */
typedef struct HuarongTopologyNet {
    ReasoningNode** nodes;     // 所有推理节点数组
    int node_count;            // 节点总数
    int max_nodes;             // 最大节点容量
    void* _pad_state1;         // 保留：二进制兼容（原 current_state）
    void* _pad_state2;         // 保留：二进制兼容（原 initial_state）
    void* _pad_state3;         // 保留：二进制兼容（原 target_state）
    int _pad_state_hist;       // 保留：二进制兼容（原 max_state_history）
    float learning_rate;       // 学习率
    int is_training;           // 是否处于训练模式

    // 线程安全
    pthread_mutex_t mutex;     // 保护拓扑数据结构
} HuarongTopologyNet;

/** 节点默认连接初始容量 */
#define DEFAULT_CONNECTION_CAPACITY PM_DEFAULT_CONN_CAP

// ==================== 核心API函数 ==================== 

/**
 * 创建华容道拓扑网络
 */
HuarongTopologyNet* huarong_net_create(int max_nodes);

/**
 * 销毁华容道拓扑网络
 */
void huarong_net_destroy(HuarongTopologyNet* net);

/**
 * 添加推理节点
 */
ReasoningNode* huarong_net_add_node(HuarongTopologyNet* net, 
                                   const char* concept, 
                                   float* features, 
                                   int feature_dim);

/**
 * 线程安全的查找或创建节点（原子操作）
 * 先查哈希表，找到则返回已有节点；找不到则创建新节点并加入哈希表
 * 在 net->mutex 保护下完成，避免并发查找-创建竞态
 */
ReasoningNode* huarong_net_find_or_create_node(HuarongTopologyNet* net,
                                               const char* concept,
                                               float* features,
                                               int feature_dim,
                                               NodeHashTable* hash);

/**
 * 创建节点连接
 */
int huarong_net_add_connection(HuarongTopologyNet* net,
                              int from_node_id,
                              int to_node_id,
                              float weight);

/**
 * 创建双向连接（两个方向都有连接，支持双向联想）
 */
int huarong_net_add_bidirectional_connection(HuarongTopologyNet* net,
                                              int node_a_id, int node_b_id,
                                              float weight);

/**
 * 拓扑排序推理
 */
int* huarong_net_topological_sort(HuarongTopologyNet* net, int* path_length);

/**
 * 动态添加推理节点
 */
int huarong_net_dynamic_add_node(HuarongTopologyNet* net,
                                const char* concept,
                                float* features,
                                int feature_dim);

/**
 * 动态删除推理节点
 */
int huarong_net_dynamic_remove_node(HuarongTopologyNet* net, int node_id);

/**
 * 网络结构优化（去除冗余连接）
 */
void huarong_net_optimize(HuarongTopologyNet* net);

/**
 * 拓扑边剪枝：移除低置信度且低权重的边
 * @param net 拓扑网络
 * @param min_confidence 边置信度下限（低于此值且权重也低则删除）
 * @param min_weight 边权重下限（绝对值）
 * @return 移除的边数
 */
int huarong_net_prune_edges(HuarongTopologyNet* net, float min_confidence, float min_weight);

#endif // HUARONG_TOPOLOGY_H