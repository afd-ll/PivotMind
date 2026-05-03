#ifndef CATASTROPHIC_FORGETTING_H
#define CATASTROPHIC_FORGETTING_H

#include "huarong_topology.h"
#include "multi_topology.h"
#include <stdbool.h>

// ==================== 弹性权重固化 (EWC) ====================

/**
 * 费雪信息矩阵条目
 * 存储对角近似的费雪信息
 */
typedef struct FisherInfoEntry {
    int node_id;                  // 节点ID
    float* fisher_diag;           // 对角费雪信息 (每个参数一个)
    int param_count;              // 参数量
    float importance;             // 综合重要性
    time_t computed_at;           // 计算时间
} FisherInfoEntry;

/**
 * 费雪信息矩阵
 * 使用对角近似避免 O(n²) 存储
 */
typedef struct FisherInfoMatrix {
    FisherInfoEntry** entries;    // 费雪信息条目数组
    int entry_count;              // 条目数量
    int capacity;                 // 容量
    float damping;                // 阻尼因子 (防止数值不稳定)
    time_t last_update;           // 最后更新时间
} FisherInfoMatrix;

/**
 * EWC 配置
 */
typedef struct EWCConfig {
    float lambda;                 // EWC 惩罚强度
    float damping;                 // 阻尼因子
    int fisher_update_interval;    // 费雪信息更新间隔 (步数)
    float importance_threshold;    // 重要性阈值
    bool online_ewc;              // 是否使用在线 EWC
    float gamma;                  // 在线 EWC 的衰减因子
} EWCConfig;

// ==================== 记忆回放 ====================

/**
 * 记忆样本
 */
typedef struct MemorySample {
    int node_id;                 // 节点ID
    float* features;              // 特征向量
    int feature_dim;              // 特征维度
    float activation;             // 激活值
    float priority;               // 优先级 (用于 PER)
    time_t timestamp;             // 时间戳
    int access_count;             // 访问次数
} MemorySample;

/**
 * 记忆回放缓冲区
 * 支持优先级回放 (Prioritized Experience Replay)
 */
typedef struct ReplayBuffer {
    MemorySample** samples;       // 样本数组
    int capacity;                 // 缓冲区容量
    int size;                     // 当前大小
    int head;                     // 头指针 (环形缓冲区)

    // PER (优先级回放) 参数
    float alpha;                  // 优先级指数
    float beta;                   // 重要性采样指数
    float* cumulative_sum;        // 累积和 (用于分段树)
    int* segment_tree;           // 线段树 (快速采样)

    // 统计
    long total_additions;         // 总添加次数
    long total_replays;           // 总回放次数
} ReplayBuffer;

// ==================== 知识隔离 ====================

/**
 * 知识域
 */
typedef struct KnowledgeDomain {
    int domain_id;                // 域ID
    const char* name;             // 域名
    const char* description;     // 描述

    // 节点分配
    int* node_ids;                // 属于此域的节点ID
    int node_count;               // 节点数量
    int capacity;                 // 容量

    // 域特定参数
    float* domain_weights;        // 域特定权重
    float isolation_strength;     // 隔离强度

    // 域统计
    float avg_importance;         // 平均重要性
    float activity_level;         // 活跃度
    time_t created_at;            // 创建时间
    time_t last_accessed;         // 最后访问时间
} KnowledgeDomain;

/**
 * 隔离策略
 */
typedef enum {
    ISOLATION_NONE = 0,           // 无隔离
    ISOLATION_HARD,              // 硬隔离 (物理分离)
    ISOLATION_SOFT,              // 软隔离 (惩罚跨域连接)
    ISOLATION_ADAPTIVE            // 自适应隔离
} DomainIsolationPolicy;

// ==================== 任务快照（EWC旧参数存储） ====================

/**
 * 任务参数快照
 * 存储任务结束时的网络参数，用于EWC保护
 */
typedef struct TaskSnapshot {
    int task_id;                    // 任务ID
    time_t created_at;             // 创建时间
    int node_count;                // 快照时的节点数

    // 按节点存储的权重快照
    void** node_params;

    int snapshot_size;             // 快照的节点参数数量
    float avg_loss;                // 快照时的平均损失
} TaskSnapshot;

// ==================== 任务边界检测 ====================

/**
 * 任务边界检测器
 */
typedef struct TaskBoundaryDetector {
    // 检测阈值
    float knowledge_gain_threshold;   // 知识增益阈值
    float activation_drift_threshold;  // 激活漂移阈值
    float connection_change_threshold; // 连接变化阈值

    // 历史记录
    float* recent_knowledge_gains;     // 最近知识增益
    int gain_history_size;             // 历史大小
    int gain_history_idx;              // 当前索引

    // 统计
    int total_task_boundaries;         // 检测到的任务边界数
    float avg_task_duration;           // 平均任务持续时间
    time_t last_boundary;              // 上次边界时间
} TaskBoundaryDetector;

/**
 * 任务信息
 */
typedef struct TaskInfo {
    int task_id;                  // 任务ID
    const char* name;             // 任务名称
    int start_step;               // 开始步数
    int end_step;                 // 结束步数 (-1 表示未结束)
    float knowledge_gain;         // 知识增益
    int samples_processed;        // 处理样本数
} TaskInfo;

// ==================== API 函数声明 ====================

// ========== EWC 配置管理 ==========

/**
 * 创建默认 EWC 配置
 */
EWCConfig* ewc_config_create(void);

/**
 * 创建自定义 EWC 配置
 */
EWCConfig* ewc_config_create_custom(float lambda, float damping, int update_interval);

/**
 * 销毁 EWC 配置
 */
void ewc_config_destroy(EWCConfig* config);

/**
 * 获取默认 EWC 配置
 */
EWCConfig* ewc_get_default_config(void);

/**
 * 设置默认 EWC 配置
 */
void ewc_set_default_config(EWCConfig* config);

// ========== 费雪信息矩阵 ==========

/**
 * 创建费雪信息矩阵
 */
FisherInfoMatrix* fisher_info_create(int capacity, float damping);

/**
 * 销毁费雪信息矩阵
 */
void fisher_info_destroy(FisherInfoMatrix* matrix);

/**
 * 计算单个节点的费雪信息
 * @param net 拓扑网络
 * @param node_id 节点ID
 * @param fisher_diag 输出数组 (需预先分配)
 */
void compute_fisher_info(HuarongTopologyNet* net, int node_id, float* fisher_diag);

/**
 * 更新费雪信息
 * @param matrix 费雪信息矩阵
 * @param net 拓扑网络
 * @param node_id 节点ID
 */
int fisher_info_update(FisherInfoMatrix* matrix, HuarongTopologyNet* net, int node_id);

/**
 * 获取节点费雪信息
 * @param matrix 费雪信息矩阵
 * @param node_id 节点ID
 * @return 费雪信息数组 (只读), NULL 表示不存在
 */
float* fisher_info_get(FisherInfoMatrix* matrix, int node_id);

// ========== EWC 核心 ==========

/**
 * 计算 EWC 惩罚项
 * @param matrix 费雪信息矩阵
 * @param net 拓扑网络
 * @param old_params 旧参数 (从优化状态获取)
 * @param node_id 节点ID (-1 表示所有)
 * @return EWC 惩罚值
 */
float ewc_penalty(FisherInfoMatrix* matrix, HuarongTopologyNet* net,
                void* old_params, int node_id);

/**
 * 带 EWC 的梯度更新
 * @param matrix 费雪信息矩阵
 * @param net 拓扑网络
 * @param gradients 梯度数组
 * @param old_params 旧参数
 * @param learning_rate 学习率
 * @param node_id 节点ID (-1 表示所有)
 * @return 0 成功
 */
int ewc_gradient_update(FisherInfoMatrix* matrix, HuarongTopologyNet* net,
                      float* gradients, void* old_params,
                      float learning_rate, int node_id);

// ========== 记忆回放缓冲区 ==========

/**
 * 创建记忆回放缓冲区
 * @param capacity 缓冲区容量
 * @param alpha 优先级指数 (0=均匀, 1=完全优先级)
 * @param beta 重要性采样指数 (0=无校正, 1=完全校正)
 */
ReplayBuffer* replay_buffer_create(int capacity, float alpha, float beta);

/**
 * 销毁回放缓冲区
 */
void replay_buffer_destroy(ReplayBuffer* buffer);

/**
 * 添加记忆样本
 * @param buffer 回放缓冲区
 * @param sample 样本
 * @return 0 成功, -1 缓冲区满
 */
int add_to_replay_buffer(ReplayBuffer* buffer, MemorySample* sample);

/**
 * 采样回放批次
 * @param buffer 回放缓冲区
 * @param batch_size 批次大小
 * @param output_samples 输出样本数组 (需预先分配)
 * @return 实际采样数量
 */
int sample_replay_batch(ReplayBuffer* buffer, int batch_size,
                       MemorySample** output_samples);

/**
 * 更新样本优先级
 * @param buffer 回放缓冲区
 * @param index 样本索引
 * @param priority 新优先级
 */
void replay_buffer_update_priority(ReplayBuffer* buffer, int index, float priority);

/**
 * 回放旧记忆
 * @param buffer 回放缓冲区
 * @param net 拓扑网络
 * @param batch_size 批次大小
 * @return 回放的记忆数
 */
int replay_old_memories(ReplayBuffer* buffer, HuarongTopologyNet* net, int batch_size);

/**
 * 获取缓冲区统计
 */
void replay_buffer_get_stats(ReplayBuffer* buffer, int* size, int* capacity,
                            long* total_additions, long* total_replays);

// ========== 知识域管理 ==========

/**
 * 创建知识域
 * @param name 域名
 * @param description 描述
 * @param initial_capacity 初始容量
 */
KnowledgeDomain* knowledge_domain_create(const char* name, const char* description,
                                        int initial_capacity);

/**
 * 销毁知识域
 */
void knowledge_domain_destroy(KnowledgeDomain* domain);

/**
 * 添加节点到域
 */
int knowledge_domain_add_node(KnowledgeDomain* domain, int node_id);

/**
 * 从域移除节点
 */
int knowledge_domain_remove_node(KnowledgeDomain* domain, int node_id);

/**
 * 隔离域权重
 * @param master 主拓扑
 * @param domain 知识域
 * @param policy 隔离策略
 * @param strength 隔离强度
 */
int isolate_domain_weights(MasterTopology* master, KnowledgeDomain* domain,
                          DomainIsolationPolicy policy, float strength);

/**
 * 跨域知识迁移
 * @param master 主拓扑
 * @param from_domain 源域
 * @param to_domain 目标域
 * @param transfer_ratio 迁移比例
 */
int cross_domain_transfer(MasterTopology* master, KnowledgeDomain* from_domain,
                        KnowledgeDomain* to_domain, float transfer_ratio);

/**
 * 计算域间干扰
 * @param master 主拓扑
 * @param domain1 域1
 * @param domain2 域2
 * @return 干扰分数 (0-1, 越高干扰越大)
 */
float compute_domain_interference(MasterTopology* master,
                                 KnowledgeDomain* domain1,
                                 KnowledgeDomain* domain2);

// ========== 任务边界检测 ==========

/**
 * 创建任务边界检测器
 */
TaskBoundaryDetector* task_boundary_detector_create(float gain_threshold,
                                                   float drift_threshold,
                                                   float change_threshold);

/**
 * 销毁任务边界检测器
 */
void task_boundary_detector_destroy(TaskBoundaryDetector* detector);

/**
 * 检测任务边界
 * @param detector 检测器
 * @param current_gain 当前知识增益
 * @param activation_drift 激活漂移
 * @param connection_change 连接变化
 * @return true 表示检测到任务边界
 */
bool detect_task_boundary(TaskBoundaryDetector* detector,
                         float current_gain,
                         float activation_drift,
                         float connection_change);

/**
 * 估计知识增益
 * @param net 拓扑网络
 * @param old_importance 旧重要性数组
 * @param new_importance 新重要性数组
 * @param count 节点数量
 * @return 知识增益值
 */
float estimate_knowledge_gain(HuarongTopologyNet* net,
                             float* old_importance,
                             float* new_importance,
                             int count);

/**
 * 是否应触发巩固
 * @param detector 检测器
 * @param consecutive_gains 连续知识增益次数
 * @return true 表示应该触发巩固
 */
bool should_trigger_consolidation(TaskBoundaryDetector* detector,
                                  int consecutive_gains);

/**
 * 获取任务信息
 */
TaskInfo* task_boundary_get_current_task(TaskBoundaryDetector* detector);

// ========== 任务快照管理 ==========

/**
 * 创建任务快照（从当前网络状态）
 * @param net 拓扑网络
 * @param task_id 任务ID
 * @return 快照结构，NULL 失败
 */
TaskSnapshot* task_snapshot_create(HuarongTopologyNet* net, int task_id);

/**
 * 销毁任务快照
 */
void task_snapshot_destroy(TaskSnapshot* snapshot);

/**
 * 获取快照中指定节点的旧参数
 * @param snapshot 快照
 * @param node_id 节点ID
 * @param out_weight_count 输出权重数量
 * @return 权重数组，NULL 未找到
 */
float* task_snapshot_get_weights(TaskSnapshot* snapshot, int node_id, int* out_weight_count);

/**
 * 从快照计算EWC惩罚（基于真实参数差异）
 * @param snapshot 任务快照
 * @param net 当前网络
 * @param fisher_diag 费雪信息对角
 * @param lambda EWC惩罚强度
 * @return 惩罚值
 */
float compute_ewc_penalty_from_snapshot(TaskSnapshot* snapshot, HuarongTopologyNet* net,
                                       float* fisher_diag, float lambda);

// ========== 一站式持续学习接口 ==========

/**
 * 持续学习上下文
 */
typedef struct ContinualLearningContext {
    // 子系统
    FisherInfoMatrix* fisher_matrix;
    ReplayBuffer* replay_buffer;
    KnowledgeDomain** domains;
    int domain_count;
    TaskBoundaryDetector* boundary_detector;

    // EWC 配置
    EWCConfig* ewc_config;

    // 网络引用
    MasterTopology* master;           // 主拓扑网络引用

    // 任务快照（EWC旧参数）
    TaskSnapshot** task_snapshots;     // 各任务的参数快照数组
    int snapshot_count;                // 快照数量
    int snapshot_capacity;             // 快照容量

    // 状态
    int current_task_id;
    int step_count;
    bool consolidation_in_progress;
    time_t last_consolidation;
} ContinualLearningContext;

/**
 * 创建持续学习上下文
 */
ContinualLearningContext* continual_learning_create(void);

/**
 * 设置主拓扑网络引用
 * @param ctx 持续学习上下文
 * @param master 主拓扑网络
 */
void continual_learning_set_master(ContinualLearningContext* ctx, MasterTopology* master);

/**
 * 销毁持续学习上下文
 */
void continual_learning_destroy(ContinualLearningContext* ctx);

/**
 * 学习前准备 (处理任务边界检测)
 * @param ctx 持续学习上下文
 * @param net 拓扑网络
 * @return 新任务开始返回 true
 */
bool continual_learning_on_episode_start(ContinualLearningContext* ctx,
                                         HuarongTopologyNet* net);

/**
 * 学习步骤 (带 EWC 保护)
 * @param ctx 持续学习上下文
 * @param net 拓扑网络
 * @param gradients 梯度
 * @param learning_rate 学习率
 * @return EWC 惩罚值
 */
float continual_learning_on_gradients(ContinualLearningContext* ctx,
                                      HuarongTopologyNet* net,
                                      float* gradients,
                                      float learning_rate);

/**
 * 学习后处理 (记忆回放、统计更新)
 * @param ctx 持续学习上下文
 * @param net 拓扑网络
 * @param samples_processed 处理样本数
 */
void continual_learning_on_episode_end(ContinualLearningContext* ctx,
                                       HuarongTopologyNet* net,
                                       int samples_processed);

/**
 * 执行巩固
 * @param ctx 持续学习上下文
 * @param net 拓扑网络
 * @return 0 成功
 */
int continual_learning_consolidate(ContinualLearningContext* ctx,
                                   HuarongTopologyNet* net);

/**
 * 获取持续学习统计
 */
void continual_learning_get_stats(ContinualLearningContext* ctx,
                                 int* total_tasks,
                                 int* current_task,
                                 float* avg_retention);

/**
 * 保存当前任务参数快照（应在任务边界处调用）
 * @param ctx 持续学习上下文
 * @param net 拓扑网络
 * @return 0 成功
 */
int continual_learning_save_snapshot(ContinualLearningContext* ctx, HuarongTopologyNet* net);

/**
 * 计算所有历史任务的EWC惩罚
 * @param ctx 持续学习上下文
 * @param net 当前网络
 * @return 总惩罚值
 */
float continual_learning_compute_all_ewc_penalty(ContinualLearningContext* ctx, HuarongTopologyNet* net);

#endif // CATASTROPHIC_FORGETTING_H
