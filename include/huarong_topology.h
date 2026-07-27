#ifndef HUARONG_TOPOLOGY_H
#define HUARONG_TOPOLOGY_H

#include "constants.h"
#include "cognitive_params.h"
#include <stdint.h>
#include <pthread.h>

/** 前置声明（避免循环包含 node_hash.h） */
typedef struct NodeHashTable NodeHashTable;

// ==================== 节点类型枚举 ==================== 
typedef enum {
    NODE_TYPE_FUNCTION_WORD = 0,  // 功能词（的、了、是）— 衰减慢
    NODE_TYPE_COMMON_WORD  = 1,  // 普通词 — 正常衰减
    NODE_TYPE_PROPER_NOUN  = 2,  // 专有名词 — 衰减快，鼓励替换
    NODE_TYPE_VISUAL       = 3   // 视觉概念节点 — 多模态特征（CLIP编码）
} NodeType;

// ==================== 华容道拓扑神经网络核心结构 ==================== 

typedef struct {
    void* target;    /* 目标节点指针 */
    int   index;     /* 在 edges[] 数组中的下标 */
    int   is_deleted;/* 墓碑标记: 1=已删除（查找时跳过，插入时可复用） */
} ConnHashEntry;

/**
 * 连接边结构体 — 封装目标节点指针 + 边属性
 * 
 * 替代原四个平行数组（connections / connection_weights /
 * connection_motivational_bias / connection_confidences），
 * 避免扩容/删除/排序时的同步维护灾难。
 */
typedef struct Edge {
    struct ReasoningNode* target;           // 目标节点指针
    float weight;                           // 连接权重 (logical_strength)
    float motivational_bias;                // 动机倾向
    float confidence;                       // 边置信度
} Edge;

/** 模板节点 — 槽位间连接词缓冲区大小（UTF-8 中文约 10 字） */
#define TPL_CONNECTOR_BUF 32

/**
 * 推理节点（类比华容道的滑块）
 * 代表知识图谱中的一个概念节点
 * 
 * 集成认知架构参数:
 * - confidence: 三维置信度 (predictive_accuracy, user_satisfaction, novelty_bonus)
 * - valence: 效价 (-1.0 ~ +1.0)
 * - edges[].weight: logical_strength (逻辑强度)
 * - edges[].motivational_bias: 动机倾向
 *
 * ========== 线程安全锁序（重要，违反会导致死锁）==========
 * 三层锁必须严格按以下顺序获取：
 *   ① MasterTopology.rwlock (全局读写锁，推理线程读取时只拿读锁)
 *   ② HuarongTopologyNet.mutex (网络级互斥锁，保护扩容/节点创建)
 *   ③ HuarongTopologyNet.node_locks[] (节点级锁池，按 node_id 升序加锁)
 *
 * 反向顺序（如：持节点锁再请求网络锁）会导致 ABBA 死锁。
 * 多节点加锁始终按 node_id 升序。
 * ============================================================
 */
typedef struct ReasoningNode {
    int node_id;               // 节点唯一标识
    char* concept;             // 概念名称/描述
    float* features;           // 特征向量
    int feature_dim;           // 特征维度
    
    // 连接边 — 单个 Edge 数组替代原四个平行数组
    Edge* edges;               // 边数组（包含 target/weight/bias/confidence）
    int edge_capacity;         // 边数组容量
    int edge_count;            // 边数量
    
    // 连接哈希表 — O(1) 查找目标节点在 connections[] 中的索引
    // 开放寻址，key=目标节点指针, value=connections[]数组下标
    ConnHashEntry* conn_hash;    /* 连接哈希表: O(1)查找目标节点 index */
    int conn_hash_mask;        // 桶数-1 (2^n-1), 0=未初始化
    int conn_hash_entries;     // 当前条目数
    
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
    
    // 涌现词类系统 — 自主学习的词性标注（与硬编码 POS 并行）
    // 一个词可以属于多个涌现词类（支持多义词如"计划"既是名词也是动词）
    int   emergent_class_count;              // 实际词类数
    int   emergent_class_ids[4];             // 涌现词类 ID（值同 POSTag 枚举，但来源为特征向量聚类）
    float emergent_class_confs[4];           // 各词类置信度

    // 模板节点元数据 — 语法句式模板（仅 TOPO_TEMPLATE 节点有效）
    // 编码 POS 序列 + 槽位间连接词，如 [N]的[V]是[Adj] 表示定中+系表
    int   tpl_pos_len;             // POS 序列长度 (2-4)
    int   tpl_pos_seq[4];          // 各槽位的 POSTag（值为 POSTag 枚举）
    char  tpl_connectors[4][TPL_CONNECTOR_BUF]; // 槽位间连接词 (UTF-8 中文约10字)
    // 涌现槽位 — 以涌现词类 ID 编码模板槽位，支持多义词软匹配
    int   tpl_emergent_slot[4];    // 各槽位的涌现词类 ID（-1=未设置）
    float tpl_emergent_conf[4];    // 各涌现槽位的置信度

    // 元信息
    int is_reversible;         // 是否支持可逆操作
    int is_visited;            // 搜索过程中是否已访问
    volatile int is_cooled;    // 大脑模式: 连接数据已冻入文件 (0=热 1=冷)
} ReasoningNode;

/**
 * 华容道拓扑网络主结构
 */
typedef struct HuarongTopologyNet {
    ReasoningNode** nodes;     // 所有推理节点数组
    int node_count;            // 节点总数 (int 兼容存量; 超 2G 时需升级)
    size_t max_nodes;          // 最大节点容量
    void* _pad_state1;         // 保留：二进制兼容（原 current_state）
    void* _pad_state2;         // 保留：二进制兼容（原 initial_state）
    void* _pad_state3;         // 保留：二进制兼容（原 target_state）
    int _pad_state_hist;       // 保留：二进制兼容（原 max_state_history）
    float learning_rate;       // 学习率
    int is_training;           // 是否处于训练模式

    // 线程安全
    pthread_rwlock_t mutex;         // 读写锁：find_concept 用读锁，add_node/concept_hash 扩容用写锁
    pthread_mutex_t node_locks[PM_NODE_LOCK_COUNT]; // 节点级锁池
    
    // 延迟释放链表 — 扩容旧数组暂存，epoch 结束统一清理
    void* retired_conns;           // 链表头节点
    int retired_pending;            // 是否有待清理的旧数组
    int epoch;                      // 当前代次（扩容时推进）
    volatile int active_readers;    // 当前活跃读线程计数
    pthread_mutex_t retire_mutex;   // 退役链表锁

    /* 概念名→节点ID 哈希表 (O(1) 查找) */
    struct { const char* name; int node_id; }* concept_hash;
    int concept_hash_mask;         /* 桶数-1 */
    int concept_hash_count;        /* 条目数 */
} HuarongTopologyNet;

/** 节点默认连接初始容量 */
#define DEFAULT_CONNECTION_CAPACITY PM_DEFAULT_CONN_CAP

/* ================================================================
 *  节点连接哈希表 API — O(1) 查找目标节点在 connections[] 中的索引
 * ================================================================ */

/** 查找目标节点在 node->edges[] 中的位置 */
static inline int node_conn_find(ReasoningNode* node, ReasoningNode* target) {
    if (!node || node->is_cooled || !node->conn_hash || node->conn_hash_mask < 0 || !target) return -1;
    uintptr_t h = ((uintptr_t)target >> 3);
    int mask = node->conn_hash_mask;
    for (int i = 0; i <= mask; i++) {
        int idx = (int)((h + (uintptr_t)i) & (uintptr_t)mask);
        if (node->conn_hash[idx].target == target && !node->conn_hash[idx].is_deleted)
            return node->conn_hash[idx].index;
        if (!node->conn_hash[idx].target && !node->conn_hash[idx].is_deleted)
            return -1;  /* 真·空槽 = 不存在 */
    }
    return -1;  /* 表满遍历完毕仍未找到 */
}

/**
 * 按 node_id 升序对两个节点加锁（消除 ABBA 死锁风险）
 *
 * 锁序约定：锁池 HutangTopologyNet.node_locks[] 遵循以下层级：
 *   Level 1: MasterTopology.rwlock      — 全局读写锁
 *   Level 2: HuarongTopologyNet.mutex   — 网络级互斥锁
 *   Level 3: HuarongTopologyNet.node_locks[] — 节点级锁池，按 node_id 升序
 *
 * 本函数处理 Level 3 的双锁获取，确保按照 (node_id_a & mask) 升序取锁。
 *
 * @param net          拓扑网络
 * @param node_id_a    第一个节点 ID
 * @param node_id_b    第二个节点 ID
 * @param locked_both  输出：两把锁是否不同，传 NULL 不关心
 */
static inline void lock_two_nodes_by_id(HuarongTopologyNet* net,
                                       int node_id_a, int node_id_b,
                                       int* locked_both) {
    int li_a = node_id_a & (PM_NODE_LOCK_COUNT - 1);
    int li_b = node_id_b & (PM_NODE_LOCK_COUNT - 1);
    if (li_a == li_b) {
        pthread_mutex_lock(&net->node_locks[li_a]);
        if (locked_both) *locked_both = 0;
    } else if (li_a < li_b) {
        pthread_mutex_lock(&net->node_locks[li_a]);
        pthread_mutex_lock(&net->node_locks[li_b]);
        if (locked_both) *locked_both = 1;
    } else {
        pthread_mutex_lock(&net->node_locks[li_b]);
        pthread_mutex_lock(&net->node_locks[li_a]);
        if (locked_both) *locked_both = 1;
    }
}

/** 释放 lock_two_nodes_by_id 获得的锁 */
static inline void unlock_two_nodes_by_id(HuarongTopologyNet* net,
                                          int node_id_a, int node_id_b,
                                          int locked_both) {
    int li_a = node_id_a & (PM_NODE_LOCK_COUNT - 1);
    int li_b = node_id_b & (PM_NODE_LOCK_COUNT - 1);
    pthread_mutex_unlock(&net->node_locks[li_a]);
    if (locked_both && li_a != li_b)
        pthread_mutex_unlock(&net->node_locks[li_b]);
}

/** 插入 (target, index) 映射 — net 用于延迟释放旧哈希表 */
int node_conn_hash_insert(HuarongTopologyNet* net, ReasoningNode* node,
                           ReasoningNode* target, int index);

/** 释放节点的连接哈希表 */
void node_conn_hash_free(ReasoningNode* node);

// ==================== 核心API函数 ==================== 

/**
 * 创建华容道拓扑网络
 */
HuarongTopologyNet* huarong_net_create(size_t max_nodes);

/**
 * 销毁华容道拓扑网络
 */
void huarong_net_destroy(HuarongTopologyNet* net);

/** 清理训练期间延迟释放的扩容旧数组（epoch 结束时调用） */
void huarong_net_cleanup_retired(HuarongTopologyNet* net);

/** 将任意堆指针挂入退役链表，epoch 结束时安全释放（防并发 use-after-free） */
void huarong_net_retire_blob(HuarongTopologyNet* net, void* ptr);

/** 推理线程进入读取临界区（记录活跃读者） */
static inline void huarong_net_enter_reader(HuarongTopologyNet* net) {
    if (net) __sync_fetch_and_add(&net->active_readers, 1);
}

/** 推理线程离开读取临界区（递减活跃读者，最后一人触发清理） */
static inline void huarong_net_leave_reader(HuarongTopologyNet* net) {
    if (net && __sync_sub_and_fetch(&net->active_readers, 1) == 0)
        huarong_net_cleanup_retired(net);
}

struct MasterTopology;  /* 前向声明 — 避免与 multi_topology.h 循环依赖 */

/** 批量清理所有子拓扑的延迟释放数组 */
void huarong_net_cleanup_retired_batch(struct MasterTopology* master);

/**
 * 惰性特征分配：仅在 features==NULL 时按需分配并用概念哈希初始化
 * 返回 0=成功/已分配, -1=失败
 * 调用方无需持锁；内部无竞争操作
 */
int lazy_alloc_node_features(ReasoningNode* node);

/**
 * 添加推理节点
 */
ReasoningNode* huarong_net_add_node(HuarongTopologyNet* net, 
                                   const char* concept, 
                                   float* features, 
                                   int feature_dim);

/** 概念名→节点ID O(1)查找 (概念哈希表) */
int huarong_net_find_concept(HuarongTopologyNet* net, const char* concept);

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