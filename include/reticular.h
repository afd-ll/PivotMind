/**
 * @file reticular.h
 * @brief 网状结构 — 注意力过滤与信息筛选
 *
 * 大脑类比：脑干网状结构——调控觉醒水平，筛选哪些刺激进入皮层意识。
 * 系统映射：attention 模块包装，为丘脑提供注意力门控信号。
 */

#ifndef RETICULAR_H
#define RETICULAR_H

#include "multi_topology.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 一次注意力扫描：标记最相关的K个节点 */
int reticular_attend(MasterTopology* topology, int top_k);

/** 获取注意力加权后的节点激活值 */
float reticular_node_salience(MasterTopology* topology, int node_id);

#ifdef __cplusplus
}
#endif

#endif
