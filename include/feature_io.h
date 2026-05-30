#ifndef FEATURE_IO_H
#define FEATURE_IO_H

#include "multi_topology.h"

/**
 * @file feature_io.h
 * @brief 节点特征向量持久化 — 原生二进制快照 + 原子写入
 *
 * 文件格式 (features.bin):
 *   [4B 魔数 0x46545652 "FTRF"]
 *   [4B 节点总数 N]
 *   [4B 特征维度 D (应=NODE_FEATURE_DIM)]
 *   [N × D 个 float]  // 按拓扑顺序→节点顺序排列
 *
 * 魔数/维度/节点数不匹配 → 静默失败, 调用者初始化
 */

/** 特征文件魔数: "FTRF" */
#define FEATURE_FILE_MAGIC 0x46545652

/**
 * 保存所有子拓扑的所有节点特征向量
 * 原子写入: 先写 .tmp → rename 覆盖
 * @param master 主拓扑
 * @param filepath 输出文件路径
 * @return 写入节点数, -1 失败
 */
int save_features(MasterTopology* master, const char* filepath);

/**
 * 加载特征向量到节点
 * 节点数/维度不匹配 → 返回 -1 (调用者回退初始化)
 * @param master 主拓扑
 * @param filepath 输入文件路径
 * @return 加载节点数, -1 失败
 */
int load_features(MasterTopology* master, const char* filepath);

/**
 * 为所有 feature_dim==0 或 features==NULL 的节点初始化随机向量
 * 固定种子确保每次初始化可复现
 * @param master 主拓扑
 * @return 初始化节点数
 */
int init_random_features(MasterTopology* master);

#endif // FEATURE_IO_H
