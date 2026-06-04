/**
 * @file constants.h
 * @brief PivotMind 全局常量 — 集中管理所有硬编码数值
 *
 * 分类：
 *   PM_TOKEN_*   — 分词/概念数组上限
 *   PM_BUF_*     — 各类缓冲区大小
 *   PM_TRACK_*   — 路径/边跟踪数组上限
 *   PM_TOPOLOGY_*— 拓扑结构参数
 *   PM_LEARN_*   — 学习相关参数
 */

#ifndef PIVOTMIND_CONSTANTS_H
#define PIVOTMIND_CONSTANTS_H

/* ========== 分词/概念 ========== */
#define PM_TOKEN_MAX         64    // 单次输入最大 token 数
#define PM_CONCEPT_MAX       64    // 单轮最大概念数
#define PM_CONCEPT_NAME      256   // 概念名称最大长度
#define PM_CONCEPT_LARGE     4096  // 大概念缓冲区（序列化用）

/* ========== 缓冲区 ========== */
#define PM_SMALL_BUF         128   // 关联信息等小缓冲
#define PM_KEY_BUF           256   // 记忆系统 key 缓冲
#define PM_PATH_BUF          512   // snprintf / 路径缓冲
#define PM_RESPONSE_BUF      2048  // 回复文本缓冲

/* ========== 路径/边跟踪 ========== */
#define PM_EDGE_TRACK        128   // 每节点被激活边跟踪上限
#define PM_PATH_TRACK        128   // 路径节点/边跟踪数组大小
#define PM_PATH_MAX_STEPS    32    // 单次走边最大步数

/* ========== 拓扑结构 ========== */
#define PM_DEFAULT_CONN_CAP  10    // 节点默认连接容量
#define PM_CROSS_HIT_TABLE   2048  // 跨拓扑 hit 记录哈希表大小
#define PM_MAX_NODES_PER_TOPO 10000 // 单拓扑最大节点数（邻接表索引编码用）
#define PM_NODE_FEATURE_DIM  512   // 拓扑节点语义向量维度

/* ========== 学习参数 ========== */
#define PM_AUTONOMIC_MAX_CONN        8000 // 自主学习单节点最大连接数
#define PM_AUTONOMIC_SHARD           16   // 边更新分片数
#define PM_ACTIVATED_PAIRS           4096 // 每轮最大激活节点对
#define PM_PRUNE_BATCH_SIZE         500  // 剪枝每批处理节点数（减少持锁时间）
#define PM_AUTONOMIC_FLUSH_THRESHOLD 50   // 自主学习刷盘阈值（累积更新次数）
#define PM_AUTONOMIC_IDLE_FLUSH_SECS 30   // 自主学习空闲刷盘超时（秒）
#define PM_ACTIVE_LEARNER_INTERVAL   300  // 主动学习器循环间隔（秒）

/* ========== 每文本字符上限 ========== */
#define PM_CHARS_PER_TEXT    256   // 单条文本去重后字符数

/* ========== 推理 ========== */
#define PM_MAX_ASSOCIATIONS      100  // 对话推理最大联想数
#define PM_MAX_RESPONSE_LEN      2048 // 回复最大长度
#define PM_OUTPUT_CACHE_SIZE     5    // 输出缓存条数
#define PM_DEFAULT_HOP_COUNT     3    // 默认推理跳数
#define PM_WALK_MAX_OUTPUT       20   // 走边输出最大步数
#define PM_CONCEPT_JUMP_LIMIT    3    // 概念层次跳跃上限（超此扣分）
#define PM_WALK_PRUNE_FLOOR      0.03f // 走边动态剪枝底限
#define PM_WALK_PRUNE_CEIL       0.30f // 走边动态剪枝封顶
#define PM_EVALUATE_THRESHOLD    0.5f  // evaluate_draft 满意度阈值
#define PM_PATH_TRIPLET_TABLE    65536 // 路径三元组频率表桶数

/* ========== 后台时钟 (BackgroundClock) ========== */
#define PM_CLOCK_TICK_INTERVAL_MS     1000     // 时钟 tick 间隔（毫秒）
#define PM_CLOCK_DECAY_PER_TICK       0.97f    // 每 tick 激活衰减率（~3%/秒）
#define PM_CLOCK_SPONTANEOUS_PROB     0.0001f  // 自发激活概率（每节点每 tick）
#define PM_CLOCK_SPONTANEOUS_STRENGTH 0.15f    // 自发激活强度
#define PM_CLOCK_CONSOLIDATE_INTERVAL 10       // 每 N 个 tick 做一次记忆巩固
#define PM_CLOCK_STATE_DRIFT_RATE     0.995f   // 认知状态漂移保持率
#define PM_CLOCK_STATE_DRIFT_BASELINE 0.005f   // 认知状态漂移回归量
#define PM_CLOCK_ACTIVATION_FLOOR     0.01f    // 激活值低于此归零

#endif // PIVOTMIND_CONSTANTS_H
