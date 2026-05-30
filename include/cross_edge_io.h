#ifndef CROSS_EDGE_IO_H
#define CROSS_EDGE_IO_H

#include "multi_topology.h"

/**
 * @file cross_edge_io.h
 * @brief 跨拓扑连接持久化 — 原生二进制快照 + 原子写入
 *
 * 文件格式 (cross_edges.bin):
 *   [4B 魔数 0x43524F53 "CROS"]
 *   [4B 边数量 M]
 *   [M × 边记录:
 *       uint32_t from_topo_type
 *       uint32_t from_node_id
 *       uint32_t to_topo_type
 *       uint32_t to_node_id
 *       float    weight
 *       uint32_t use_count
 *   ]
 *
 * 魔数/节点数不匹配 → 静默失败, 调用者回退重建
 */

/** 跨连接文件魔数: "CROS" */
#define CROSS_EDGE_FILE_MAGIC 0x43524F53

/**
 * 保存所有跨拓扑连接
 * 原子写入: 先写 .tmp → rename 覆盖
 * @param master 主拓扑
 * @param filepath 输出文件路径
 * @return 写入边数, -1 失败
 */
int save_cross_edges(MasterTopology* master, const char* filepath);

/**
 * 加载跨拓扑连接
 * 节点数变化 → 返回 -1 (调用者回退重建)
 * @param master 主拓扑
 * @param filepath 输入文件路径
 * @return 加载边数, -1 失败
 */
int load_cross_edges(MasterTopology* master, const char* filepath);

/**
 * 重建跨拓扑连接 — 增强版
 *
 * 策略：
 * 1. 从词汇拓扑向语义/情绪/概念/语法/上下文/领域/语用/文化拓扑补充同名节点
 * 2. 在每对有意义的拓扑对之间：
 *    a. 精确名称匹配（strcmp）建双向跨连接
 *    b. 子串匹配（strstr）补充
 *    c. 特征向量余弦相似度匹配（节点有 features 时启用）
 * 3. 覆盖所有 9 个拓扑之间的合理语义关系
 *
 * @param master 主拓扑
 * @return 创建的连接数
 */
int rebuild_cross_connections(MasterTopology* master);

/**
 * 快捷函数：在指定拓扑之间为新激活的节点自动建立跨连接
 *
 * 由 autonomic_learn_from_dialog 在每次对话后调用，
 * 只处理当前活跃的概念，避免全量重建的性能开销。
 *
 * @param master 主拓扑
 * @param concepts 概念名数组（如用户输入中的汉字）
 * @param concept_count 概念数量
 * @return 创建的跨连接数
 */
int auto_link_activated_nodes(MasterTopology* master,
                              const char** concepts, int concept_count);

#endif // CROSS_EDGE_IO_H
