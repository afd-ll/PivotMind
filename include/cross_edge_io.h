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
 * 重建跨拓扑连接
 * 根据词汇节点名匹配语义/情绪/概念拓扑的同名节点
 * @param master 主拓扑
 * @return 创建的连接数
 */
int rebuild_cross_connections(MasterTopology* master);

#endif // CROSS_EDGE_IO_H
