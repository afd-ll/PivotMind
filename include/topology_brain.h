/**
 * @file topology_brain.h
 * @brief 9+1 脑区区索引模块 — 词性是涌现，不是标签
 *
 * 核心原则：
 *   不往 ReasoningNode 或 Connection 里加词性字段（避免维度灾难）。
 *   词性 = 节点在脑区中的位置，从连接模式中自动涌现。
 *
 * 脑区定义（9 功能 + 1 默认词汇区）：
 *   0. VOCAB  — 词汇区（新词默认入口）
 *   1. NOUN   — 名词区
 *   2. VERB   — 动词区
 *   3. ADJ    — 形容词区
 *   4. PRON   — 代词区（你/我/他/它/这/那）
 *   5. ADV    — 副词区（很/非常/已经/也/还）
 *   6. PREP   — 介词区（在/从/对/把/被）
 *   7. CONJ   — 连词区（和/与/但/而/或）
 *   8. PART   — 助词/语气词区（的/了/着/过/呢/吗）
 *   9. NUM    — 数词/量词区（一/二/三/个/只/条）
 *
 * 算法：
 *   - 新节点 → ema[VOCAB] = 1.0（默认词汇区）
 *   - 每次节点 A 连接到节点 B，A 的 ema 向 B 的脑区偏移
 *   - ema[region] += α，其他 *= (1-α)，归一化
 *   - 查询：按 node_id 二分查找 → 取 ema 最大值对应脑区
 *   - 若最大值 < threshold → 返回 VOCAB（尚未收敛）
 *
 * 零侵入：不修改 ReasoningNode / Connection 等核心结构。
 */

#ifndef TOPOLOGY_BRAIN_H
#define TOPOLOGY_BRAIN_H

#include "multi_topology.h"

// ==================== 脑区枚举 ====================

#define TOPOBRAIN_NUM_REGIONS  10

typedef enum {
    TOPOBRAIN_VOCAB = 0,  // 词汇区（默认，未分类）
    TOPOBRAIN_NOUN,       // 名词区
    TOPOBRAIN_VERB,       // 动词区
    TOPOBRAIN_ADJ,        // 形容词区
    TOPOBRAIN_PRON,       // 代词区
    TOPOBRAIN_ADV,        // 副词区
    TOPOBRAIN_PREP,       // 介词区
    TOPOBRAIN_CONJ,       // 连词区
    TOPOBRAIN_PART,       // 助词/语气词区
    TOPOBRAIN_NUM,        // 数词/量词区
} TopoBrainRegion;

// ==================== 配置 ====================

typedef struct {
    float ema_alpha;           // EMA 衰减率 (默认 0.05)
    float converge_threshold;  // 收敛阈值 (默认 0.35)
    int   scan_interval;       // 扫描间隔 (默认 600 tick ≈ 10min)
    int   verbose;
} TopoBrainConfig;

#define TOPOBRAIN_DEFAULT_CONFIG { 0.05f, 0.35f, 600, 0 }

// ==================== 主结构（对外不透明） ====================

typedef struct TopologyBrain TopologyBrain;

// ==================== API ====================

/**
 * 创建脑区索引模块
 * @param initial_nodes 预估节点数（影响初始分配大小）
 */
TopologyBrain* topobrain_create(int initial_nodes);

/** 销毁 */
void topobrain_destroy(TopologyBrain* tb);

/** 设置配置（生效于下次 scan） */
void topobrain_set_config(TopologyBrain* tb, TopoBrainConfig* cfg);

/**
 * 注册一个新节点，默认为词汇区
 * @return 0=成功, -1=已存在, -2=失败
 */
int topobrain_add_node(TopologyBrain* tb, int node_id);

/**
 * 更新节点隶属度：node_id 连接到了一个目标节点
 * 内部通过 topobrain_query(target) 获取目标脑区，更新 EMA
 */
void topobrain_update_by_node(TopologyBrain* tb, int node_id, int target_node_id);

/**
 * 更新节点隶属度：直接指定目标脑区（当已知目标脑区时更快）
 */
void topobrain_update_by_region(TopologyBrain* tb, int node_id, TopoBrainRegion region);

/**
 * 查询节点当前脑区
 * 二分查找 O(log n)，未找到返回 TOPOBRAIN_VOCAB
 */
TopoBrainRegion topobrain_query(TopologyBrain* tb, int node_id);

/**
 * 批量扫描整个词汇拓扑，更新所有节点的隶属度
 * 遍历所有节点 → 遍历所有连接 → topobrain_update_by_node
 * @param master 主拓扑（从中获取词汇拓扑和节点）
 * @return 本次发生脑区迁移的节点数
 */
int topobrain_scan(TopologyBrain* tb, MasterTopology* master);

/**
 * 获取脑区名称
 */
const char* topobrain_region_name(TopoBrainRegion r);

/**
 * 获取统计信息
 */
void topobrain_get_stats(TopologyBrain* tb,
                          int* out_entries,
                          int* out_updates,
                          int* out_migrations);

#endif // TOPOLOGY_BRAIN_H
