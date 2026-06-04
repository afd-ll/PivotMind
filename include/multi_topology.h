#ifndef MULTI_TOPOLOGY_H
#define MULTI_TOPOLOGY_H

#include "huarong_topology.h"
#include "node_hash.h"
#include "string_pool.h"
#include "thread_pool.h"
#include <pthread.h>
#include <time.h>

/* 前向声明 (避免头文件循环依赖) */
struct PathFrequencyTable;

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
    TOPO_MASTER = 9,       // 主拓扑
    TOPO_TEMPLATE = 10     // 模板拓扑（路径编码递归抽象）
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
    float recent_activation;     // leaky integrator for recency-based novelty (0.0-1.0)
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

/** 动态跨拓扑建边跟踪表大小 */
#define CROSS_HIT_TABLE_SIZE PM_CROSS_HIT_TABLE

/** 每拓扑最大节点数（用于跨拓扑邻接表索引编码） */
#define MAX_NODES_PER_TOPO PM_MAX_NODES_PER_TOPO

// ==================== 动态跨拓扑建边跟踪 ====================

/**
 * 跨拓扑 hits 记录（用于运行时自动发现频繁跨拓扑激活组合）
 * 当某对跨拓扑节点连续激活 >= threshold 次，自动创建跨拓扑边。
 * 使用开放寻址哈希表存储，CROSS_HIT_TABLE_SIZE 在 .c 中定义。
 */
typedef struct {
    int from_topo, from_node;     // 源节点
    int to_topo, to_node;         // 目标节点
    int hit_count;                // 在 round 窗口内的命中次数
    int last_round;               // 最后一次命中的推理轮次
    int is_used;                  // 1 = 有效条目
} CrossTopoHitRecord;

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
    int _pad_parallel_mode;           // 保留：二进制兼容（原 parallel_mode）

    // ========== 线程安全 ==========
    pthread_rwlock_t rwlock;          // 读写锁：读=快照/推理 写=对话修改

    // ========== 动态跨拓扑建边跟踪 ==========
    CrossTopoHitRecord cross_hit_records[CROSS_HIT_TABLE_SIZE];  // 开放寻址哈希表
    int cross_hit_round;                                          // 当前推理轮次（防伪共享）

    // ========== 路径编码递归抽象 ==========
    struct PathFrequencyTable* freq_table;   // 路径三元组频率表
    int use_template_voting;                  // 是否启用模板跨拓扑投票
    int template_decay_round;                 // 冷路径稀释轮次计数
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

/**
 * 快速查重：使用邻接表索引 O(出度)
 * @return 1 存在, 0 不存在
 */
int cross_link_exists(MasterTopology* master,
                      int from_topo, int from_node,
                      int to_topo, int to_node);

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

// ========== 模板锚点匹配 ==========

/**
 * 查找 (node_a, node_b) 匹配的模板节点
 *
 * 模板系统建立了 vocab(node_a) → template 和 vocab(node_b) → template 的
 * 跨拓扑连接。此函数查找同时连接两个 anchor 的模板节点，用于：
 * - 走边时优先选择符合模板的候选
 * - 输出时用模板节点概念结构化回复
 *
 * @param master 主拓扑
 * @param vocab_topo_id 词汇拓扑 ID
 * @param node_a 第一个 anchor 节点 ID
 * @param node_b 第二个 anchor 节点 ID
 * @return 匹配的模板节点 ID，-1 表示无匹配
 */
int master_find_template_for_pair(MasterTopology* master,
                                   int vocab_topo_id,
                                   int node_a, int node_b);

/**
 * 无锁版本 — 调用方需已持 rwlock 或确保单线程访问
 */
int master_find_template_for_pair_nolock(MasterTopology* master,
                                          int vocab_topo_id,
                                          int node_a, int node_b);

/**
 * 获取词汇节点对应的 POS 标签
 * 通过跨拓扑连接 vocab → TOPO_SYNTAX 查找
 */
int master_get_node_pos_tag(MasterTopology* master,
                             int vocab_topo_id, int node_id);

/**
 * 获取模板节点槽位间的连接词
 * @param slot 槽位索引 (0=s0→s1, 1=s1→s2, 2=s2→s3)
 * @return 连接词字符串，无则返回 ""
 */
const char* template_get_connector(MasterTopology* master,
                                    int tpl_node_id, int slot);

/**
 * 内置 POS 对 → 连接词映射
 * 用于模板构建时自动生成连接词
 */
const char* pos_connector_map(int pos_a, int pos_b);

// ========== 走边路径生成 ==========
/**
 * 贪心走边路径生成
 *
 * 从起始节点出发，沿边选取最优下一步，生成有序路径。
 * 混合评分：加法(边权重+边置信+边动机+目标激活+目标置信) × 乘法(效价调节因子)
 *
 * 路径多样性机制：
 * - 热度衰减：节点被选次数越多，其综合评分越低，给冷门节点留机会
 * - 功能词(的、了、是) 衰减慢，专有名词衰减快，鼓励替换
 * - 动态剪枝阈值：前3步低保期几乎不剪，后续逐步收紧
 *
 * @param sub 子拓扑
 * @param start_node_id 起始节点ID
 * @param path_out 输出路径节点ID数组（长度≥max_len）
 * @param scores_out 输出每步综合评分（长度≥max_len），可传NULL
 * @param max_len 最大路径长度
 * @param visited 已访问标记位图，可传NULL（内部临时分配）
 * @param intent_weight 意图权重
 * @param master 主拓扑，可传NULL（退化为无跨拓扑投票的原行为）
 * @return 路径长度，0表示无有效路径
 */
int topology_walk_greedy(SubTopology* sub, int start_node_id,
                         int* path_out, float* scores_out,
                         int max_len, unsigned char* visited,
                         float intent_weight,
                         MasterTopology* master,
                         const float* query_anchor);

/**
 * Beam search 走边路径生成 (K=3)
 * 维护 top-3 并行路径避免贪心局部最优。
 * 签名与 topology_walk_greedy 兼容。
 */
int topology_walk_beam(SubTopology* sub, int start_node_id,
                       int* path_out, float* scores_out,
                       int max_len, unsigned char* visited,
                       float intent_weight,
                       MasterTopology* master,
                       const float* query_anchor);

/**
 * 跨拓扑走边路径生成
 *
 * 从起始节点出发，每一步评估 BOTH 本拓扑内连接 AND 跨拓扑连接，
 * 允许路径在拓扑之间自然跳转。
 * 混合评分与 topology_walk_greedy 保持一致，跨拓扑跳跃使用 link weight × transfer_rate
 * 作为"边权重"替代，其余四维（目标激活+目标置信+效价+边置信）从目标节点获取。
 *
 * @param master 主拓扑（包含所有子拓扑和跨拓扑连接）
 * @param start_topo_id 起始拓扑ID
 * @param start_node_id 起始节点ID
 * @param path_topos_out 输出路径的拓扑ID数组（长度≥max_len）
 * @param path_nodes_out 输出路径的节点ID数组（长度≥max_len）
 * @param scores_out 输出每步综合评分（长度≥max_len），可传NULL
 * @param max_len 最大路径长度
 * @param visited_bitmaps 已访问位图数组 per topology (master->sub_topo_count 个)
 * @param avoid_chars 可传 NULL；非空时路径中跳过包含这些字符的节点（防回声）
 * @param topo_act 可传 NULL；非空时每步额外奖励该拓扑累加的激活值
 * @return 路径长度（包括起点），0表示无有效路径
 */
int topology_walk_cross(MasterTopology* master,
                        int start_topo_id, int start_node_id,
                        int* path_topos_out, int* path_nodes_out,
                        float* scores_out,
                        int max_len,
                        unsigned char** visited_bitmaps,
                        const char* avoid_chars,
                        const float* topo_act,
                        const float* query_anchor);

// ========== 竞争队列生成（替代贪心走边） ==========

/**
 * 竞争队列路径生成 — 基于全局工作空间理论
 *
 * 与贪心走边的本质区别：每步看全图激活场而非局部边邻居。
 * 允许路径在图中任意跳跃，适合需要全局语义关联的场景。
 *
 * @param sub          工作子拓扑
 * @param master       主拓扑（用于跨拓扑激活扩散）
 * @param intent_weight 当前意图权重
 * @param query_anchor  输入锚点特征向量（NULL=无锚定）
 * @param max_len       最大路径长度
 * @param path_out      输出路径节点ID数组
 * @param scores_out    每步评分（可NULL）
 * @return              路径长度
 */
int competitive_queue_generate(SubTopology* sub,
                               MasterTopology* master,
                               float intent_weight,
                               const float* query_anchor,
                               int max_len,
                               int* path_out,
                               float* scores_out);

// ========== 状态持久化 ==========

int master_save_state(MasterTopology* master, const char* file_path);
int master_load_state(MasterTopology* master, const char* file_path);

// ========== 边重建 ==========

/**
 * 基于特征向量余弦相似度重建拓扑内部边
 *
 * 遍历所有节点，对每个节点找 top-N 语义最近邻并建边。
 * 仅对 features 非空的节点生效。
 *
 * @param master 主拓扑
 * @param threshold 余弦相似度阈值，低于此值不建边 (默认建议 0.35)
 * @param max_connections 每个节点最大连接数 (默认建议 8)
 * @return 创建的边总数
 */
int master_rebuild_edges_by_similarity(MasterTopology* master, float threshold, int max_connections);

// ========== 动态跨拓扑建边 ==========

/**
 * 记录一次跨拓扑命中
 * 在 topology_walk_cross 中每次跨拓扑跳跃时调用
 */
void master_record_cross_hit(MasterTopology* master, int from_topo, int from_node, int to_topo, int to_node);

/**
 * 扫描并处理跨拓扑命中记录
 * 命中 >= threshold 次的配对自动创建跨拓扑边
 * round_timeout: 超过此轮次未命中的条目重置（避免历史垃圾堆积）
 * @return 本次创建的跨拓扑边数
 */
int master_process_cross_hits(MasterTopology* master, int threshold, int round_timeout);

/**
 * 清空所有跨拓扑连接
 * 释放所有 CrossTopologyLink 对象、邻接表链表节点
 * 重置 cross_link_count = 0
 */
void master_clear_cross_links(MasterTopology* master);

/**
 * 跨拓扑连接剪枝：移除低权重且低频使用的跨拓扑连接
 * @param master 主拓扑
 * @param min_weight 边权重下限
 * @param min_use_count 最低使用次数
 * @return 移除的连接数
 */
int master_prune_cross_links(MasterTopology* master, float min_weight, int min_use_count);

#endif // MULTI_TOPOLOGY_H