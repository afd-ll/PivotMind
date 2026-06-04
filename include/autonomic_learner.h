#ifndef AUTONOMIC_LEARNER_H
#define AUTONOMIC_LEARNER_H

#include "multi_topology.h"
#include "huarong_topology.h"
#include <pthread.h>

/* 前向声明 (避免循环依赖) */
typedef struct MemorySystem MemorySystem;

/**
 * @file autonomic_learner.h
 * @brief 自主学习层 — 运行时同时激活→边置信度自动涨落
 *
 * 设计原则：
 * - 不需要外部反馈：同时激活就是学习信号
 * - 不需要独立线程：嵌在推理管线中同步执行
 * - 竞争衰退：没被激活的边自然弱化
 *
 * 与主动学习器(active_learner)的关系：
 * - 主动学习器 = 后台线程（拓扑增长/收缩/遗忘/PageRank）
 * - 自主学习器 = 嵌入推理管线的同步学习（置信度涨落）
 * - 两者互补不冲突：主动学习管结构调整，自主学习管权重微调
 */

// ==================== 配置参数 ====================

/** 同时激活时置信度涨幅 */
#define AUTONOMIC_LEARNING_RATE 0.05f

/** 边初始置信度 */
#define AUTONOMIC_INITIAL_CONFIDENCE 0.3f

/** 基础连接权重（新边） */
#define AUTONOMIC_BASE_WEIGHT 0.3f

/** 每次衰减调用的基础衰减率（0.95=每轮5%；中档边×importance调制，输家×0.85，赢家×0.9995） */
#define AUTONOMIC_DECAY_RATE 0.95f

/** 最大连接数上限（防止单个节点连接爆炸） */
#define AUTONOMIC_MAX_CONNECTIONS PM_AUTONOMIC_MAX_CONN

// ==================== 并发分片锁 ====================

/** 边哈希分片数（N个分片，减少全局锁竞争） */
#define AUTONOMIC_SHARD_COUNT PM_AUTONOMIC_SHARD

/**
 * 边哈希分片锁
 * 按 (min_node_id, max_node_id) 哈希分片
 * workers 记录时不需要锁（走 thread-local buffer）
 * barrier 后批量更新时只需要对应分片的锁
 */
typedef struct {
    pthread_mutex_t lock;
    int pending_count;                // 待更新数
} AutonomicShard;

// ==================== 线程本地缓冲区 ====================

/** 线程本地的一次对话中记录的一对同时激活节点 */
typedef struct {
    int input_node_id;       // 输入字节点ID
    int response_node_id;    // 回复字节点ID
    int topo_id;             // 所在拓扑ID
} AutonomicActivationPair;

/** 线程本地缓冲区大小 */
#define AUTONOMIC_BUFFER_SIZE 1024

// ==================== 刷盘机制 ====================

/**
 * 学习状态累积器
 * 每次 autonomic_learn_from_dialog 调用更新此结构体，
 * 达到触发条件时刷盘（保存 topology_state）
 */
typedef struct {
    // 累积更新计数
    int pending_updates;           // 自上次刷盘以来的更新数
    time_t last_flush_time;        // 上次刷盘时间
    
    // 分片
    AutonomicShard shards[AUTONOMIC_SHARD_COUNT];
    
    // ========== 异步刷盘线程 ==========
    pthread_t flush_thread;                // 后台刷盘线程
    pthread_mutex_t flush_mutex;           // 刷盘请求互斥锁 + 条件变量
    pthread_cond_t flush_cond;             // 刷盘条件变量
    volatile int flush_requested;          // 刷盘请求标志
    volatile int flush_running;            // 刷盘线程运行中
    volatile int shutdown;                 // 刷盘线程关闭标志
    MasterTopology* flush_master;          // 刷盘时使用的 master 指针
    pthread_mutex_t flush_lock;            // 保留：兼容旧引用

    // 刷盘触发器
    int flush_threshold;           // 累积更新数阈值（默认50）
    int idle_flush_seconds;        // 空闲刷盘秒数（默认30）
    int max_pending_edges;         // 待更新边数上限
    
    // 线程本地缓冲区（每个worker独立记录）
    AutonomicActivationPair local_buffers[AUTONOMIC_BUFFER_SIZE];
    int local_buffer_count;
    
    // 是否已初始化
    int initialized;
} AutonomicState;

/**
 * 初始化自主学习的并发状态
 */
void autonomic_state_init(AutonomicState* state);

/**
 * 销毁自主学习并发状态
 */
void autonomic_state_destroy(AutonomicState* state);

/**
 * 请求刷盘（触发刷盘判断）
 * 检查是否满足刷盘条件，满足则保存拓扑状态
 * 如果异步刷盘线程已启动，则发信号给后台线程立即返回；
 * 否则同步执行刷盘。
 */
void autonomic_request_flush(AutonomicState* state, MasterTopology* master);

/**
 * 启动异步刷盘后台线程
 * 调用后 autonomic_request_flush 不再同步阻塞，改为信号通知后台线程
 * @param state  已初始化的 AutonomicState
 * @param master 主拓扑指针（线程需访问）
 * @return 1=成功, 0=失败
 */
int autonomic_start_async_flush(AutonomicState* state, MasterTopology* master);

/**
 * 停止异步刷盘后台线程（阻塞等待线程结束）
 */
void autonomic_stop_async_flush(AutonomicState* state);

// ==================== 核心 API ====================

/**
 * 从一次对话交互中自主学习
 * 
 * 核心逻辑：
 * 1. 将 user_input 和 ai_response 分别拆成单字
 * 2. 在词汇拓扑中找到对应的节点
 * 3. 对每对 (输入字, 回复字)：
 *    - 如果已有连接 → 涨置信度
 *    - 如果尚无连接 → 创建新边（初始置信度）
 * 4. 衰减本轮未激活的边（竞争机制）
 * 5. 记录到 AutonomicState，刷盘判断
 *
 * @param master  主拓扑（从中获取词汇拓扑）
 * @param user_input  用户输入文本
 * @param ai_response AI回复文本
 * @param state   状态累积器（可为NULL，跳过刷盘）
 */
void autonomic_learn_from_dialog(MasterTopology* master,
                                 const char* user_input,
                                 const char* ai_response,
                                 AutonomicState* state,
                                 void* causal_graph,
                                 MemorySystem* memory);

/**
 * 对拓扑中所有节点执行竞争衰减
 * 
 * @param master 主拓扑
 */
void autonomic_decay_all(MasterTopology* master);

/**
 * 获取拓扑中总边数和平均置信度
 * 用于验证学习效果
 */
int autonomic_get_edge_stats(MasterTopology* master,
                            int* out_total_edges,
                            float* out_avg_confidence);

/**
 * 从长文本中批量自主学习（替代 reader 工具）
 *
 * 将文本按句号/换行切分成句子，逐句推入学习管线：
 * 每对相邻句子作为 (输入, 回复) 调用 autonomic_learn_from_dialog。
 * 读一本书 = 把书逐句推入推理管线 → 自主层自动建边。
 *
 * @param master     主拓扑
 * @param text       完整文本（UTF-8）
 * @param text_len   文本长度（字节），传 -1 则自动计算 strlen
 * @param state      状态累积器（可为NULL）
 * @return           创建的句子对数（学习次数）
 */
int autonomic_learn_from_text(MasterTopology* master,
                              const char* text,
                              int text_len,
                              AutonomicState* state);

#endif // AUTONOMIC_LEARNER_H