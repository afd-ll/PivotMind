#ifndef HUARONG_TOPOLOGY_H
#define HUARONG_TOPOLOGY_H

#include "tensor.h"
#include "cognitive_params.h"

// ==================== 华容道拓扑神经网络核心结构 ==================== 

/**
 * 华容道状态节点
 * 代表推理过程中的一个状态
 */
typedef struct HuarongState {
    int* board_state;          // 当前棋盘状态数组
    int state_size;            // 状态数组大小
    struct HuarongState* prev; // 前一个状态（实现可逆性）
    struct HuarongState* next; // 下一个状态
    float* reasoning_path;     // 推理路径权重
    int move_count;            // 从初始状态到当前状态的步数
    float confidence;          // 状态置信度 (0.0-1.0)
    int move_direction;        // 到达此状态的最后移动方向
    unsigned long hash;        // 状态哈希值（用于快速查重）
} HuarongState;

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
    HuarongState* current_state; // 当前推理状态
    HuarongState* initial_state; // 初始状态
    HuarongState* target_state;  // 目标状态
    int max_state_history;     // 最大状态历史记录数
    float learning_rate;       // 学习率
    int is_training;           // 是否处于训练模式
} HuarongTopologyNet;

// ==================== 核心API函数 ==================== 

/**
 * 创建华容道拓扑网络
 */
HuarongTopologyNet* huarong_net_create(int max_nodes, int max_state_history);

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
 * 前向推理（状态转换）
 */
HuarongState* huarong_net_forward(HuarongTopologyNet* net, 
                                 HuarongState* current_state, 
                                 int* action);

/**
 * 反向推理（状态回滚）
 */
HuarongState* huarong_net_backward(HuarongTopologyNet* net, 
                                  HuarongState* current_state);

/**
 * 拓扑排序推理
 */
int* huarong_net_topological_sort(HuarongTopologyNet* net, int* path_length);

/**
 * 状态哈希计算（用于快速查重）
 */
unsigned long huarong_state_hash(HuarongState* state);

/**
 * 状态比较
 */
int huarong_state_equal(HuarongState* state1, HuarongState* state2);

/**
 * 创建新状态
 */
HuarongState* huarong_state_create(int* board, int size);

/**
 * 销毁状态
 */
void huarong_state_destroy(HuarongState* state);

/**
 * 状态空间搜索（A*算法）
 */
HuarongState** huarong_state_search(HuarongTopologyNet* net,
                                   HuarongState* initial,
                                   HuarongState* target,
                                   int* path_length);

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
 * 推理路径可视化
 */
void huarong_net_visualize_path(HuarongState** path, int path_length);

#endif // HUARONG_TOPOLOGY_H