#ifndef MULTI_TOPOLOGY_H
#define MULTI_TOPOLOGY_H

#include "huarong_topology.h"
#include "node_hash.h"
#include "string_pool.h"
#include "thread_pool.h"
#include <time.h>

// ==================== 多拓扑嵌套架构 ====================

/**
 * 拓扑类型枚举
 */
typedef enum {
    TOPO_VOCABULARY = 0,   // 词汇拓扑
    TOPO_SEMANTIC = 1,     // 语义拓扑
    TOPO_EMOTION = 2,      // 情绪拓扑
    TOPO_SYNTAX = 3,       // 语法拓扑
    TOPO_CONTEXT = 4,      // 上下文拓扑
    TOPO_DOMAIN = 5,       // 领域拓扑
    TOPO_PRAGMA = 6,       // 语用拓扑
    TOPO_CULTURE = 7,      // 文化拓扑
    TOPO_CONCEPT = 8,      // 概念拓扑（数值、规则、实体）
    TOPO_MASTER = 9        // 主拓扑
} TopologyType;

/**
 * 拓扑类型名称映射
 * (在 multi_topology.c 中定义)
 */
extern const char* TOPOLOGY_TYPE_NAMES[];

/**
 * 子拓扑结构
 */
typedef struct SubTopology {
    int topo_id;                      // 拓扑唯一ID
    TopologyType type;                // 拓扑类型
    const char* name;                 // 拓扑名称（字符串池）
    const char* description;          // 拓扑描述（字符串池）
    
    HuarongTopologyNet* net;          // 底层拓扑网络
    NodeHashTable* node_hash;         // 节点哈希表（加速查找）
    
    int priority;                     // 推理优先级 (1-10)
    float weight;                     // 在主拓扑中的权重 (0.0-1.0)
    int is_active;                    // 是否激活
    
    // 统计信息
    int total_activations;
    float avg_activation_value;
    time_t last_used;
} SubTopology;

/**
 * 跨拓扑连接
 */
typedef struct CrossTopologyLink {
    int link_id;
    int from_topo_id;
    int from_node_id;
    int to_topo_id;
    int to_node_id;
    
    float weight;
    const char* relation;             // 关系类型（字符串池）
    
    int bidirectional;
    float transfer_rate;
    time_t created_time;
    int use_count;                    // 使用次数（用于动态权重学习）
} CrossTopologyLink;

/**
 * 跨拓扑连接邻接表条目
 */
typedef struct CrossTopoAdjEntry {
    int link_index;                      // 在 cross_links 中的索引
    struct CrossTopoAdjEntry* next;     // 下一个条目
} CrossTopoAdjEntry;

/**
 * 主拓扑结构
 */
typedef struct MasterTopology {
    // 字符串池（共享）
    StringPool* string_pool;

    // 子拓扑管理
    SubTopology** sub_topologies;
    int sub_topo_count;
    int sub_topo_capacity;

    // 跨拓扑连接
    CrossTopologyLink** cross_links;
    int cross_link_count;
    int cross_link_capacity;

    // 跨拓扑连接邻接表索引（加速激活传播 O(N) -> O(k)）
    CrossTopoAdjEntry** cross_adj;       // 扁平数组索引
    int cross_adj_count;                 // 索引条目数

    // 当前激活状态
    int active_topo_id;
    int* active_node_ids;
    float* activation_levels;

    // 推理控制
    float global_learning_rate;
    int inference_depth;
    int max_inference_depth;

    // 工作模式
    int parallel_inference;
    int auto_switch_topo;

    // 统计信息
    long total_inferences;
    long successful_inferences;
    int training_data_count;
    time_t created_time;

    // ========== 线程池（并行推理引擎核心） ==========
    struct ThreadPool* thread_pool;   // 共享线程池，首次并行时懒创建
    int parallel_mode;                // 0=串行（默认） 1=拓扑级并行 2=节点级并行
} MasterTopology;

// ==================== API函数声明 ====================

// ========== 主拓扑管理 ==========

MasterTopology* master_topology_create(int max_sub_topos);
void master_topology_destroy(MasterTopology* master);

int master_add_sub_topology(MasterTopology* master, 
                           TopologyType type, 
                           const char* name,
                           int initial_capacity,
                           int priority);

SubTopology* master_get_sub_topology(MasterTopology* master, int topo_id);
SubTopology* master_get_sub_topology_by_type(MasterTopology* master, 
                                             TopologyType type);

// ========== 跨拓扑连接管理 ==========

int master_add_cross_link(MasterTopology* master,
                         int from_topo_id, int from_node_id,
                         int to_topo_id, int to_node_id,
                         float weight,
                         const char* relation);

// ========== 推理与激活 ==========

int master_activate_node(MasterTopology* master,
                        int topo_id,
                        int node_id,
                        float activation_value);

int master_set_node_confidence(MasterTopology* master,
                             int topo_id,
                             int node_id,
                             float confidence);

int master_set_edge_confidence(MasterTopology* master,
                              int topo_id,
                              int from_node_id,
                              int to_node_id,
                              float confidence);

int master_propagate_activation(MasterTopology* master,
                               int source_topo_id,
                               int source_node_id);

/**
 * 增强版并行激活传播 — 拓扑级并行
 *
 * 原理：
 * - 检测所有活跃子拓扑（有节点 activation >= threshold）
 * - 每个活跃子拓扑作为一个独立任务提交到线程池
 * - 线程数 = min(活跃拓扑数, CPU核数)
 * - 未来新增子拓扑（语音/图像）自动进入线程池
 *
 * @param master 主拓扑
 * @param threshold 激活阈值（通常0.1f）
 * @return 总传播节点数
 */
int master_propagate_parallel_topology(MasterTopology* master, float threshold);

/**
 * 获取或创建线程池
 * 首次调用时自动检测CPU核数并创建
 */
ThreadPool* master_get_thread_pool(MasterTopology* master);

void master_reset_activations(MasterTopology* master);

void master_decay_activations(MasterTopology* master, float decay_rate);

void master_consolidate_confidence(MasterTopology* master, float boost_factor);

void knowledge_self_verify(MasterTopology* master, int topo_id, int node_id);

void batch_self_verify(MasterTopology* master);

// ========== 生成式推理 ==========

/**
 * 基于多拓扑网络的生成式推理
 * 
 * @param master 主拓扑
 * @param input_text 输入文本
 * @param max_output_len 最大输出长度
 * @return 生成的回复（需调用者释放）
 */
char* master_generate_response(MasterTopology* master,
                              const char* input_text,
                              int max_output_len);

// ========== 状态查询 ==========

void master_get_system_status(MasterTopology* master,
                             int* total_nodes,
                             int* total_links,
                             float* avg_activation);

void master_visualize_topology(MasterTopology* master, int topo_id);
void master_visualize_cross_links(MasterTopology* master);

// ========== 状态持久化 ==========

int master_save_state(MasterTopology* master, const char* file_path);
int master_load_state(MasterTopology* master, const char* file_path);

#endif // MULTI_TOPOLOGY_H
