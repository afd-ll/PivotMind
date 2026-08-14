/**
 * @file funcword.h
 * @brief 虚词分类器 + 职责分离（阶段 0：影子模式）
 *
 * v0.5 架构迭代（docs/funcword-classifier-design.md）
 * 本阶段只做：位置画像累积（喂料+对话）+ 构词轴离线扫描 + 分类器影子模式。
 * 零行为变化（不改变 is_function_word 决策）。
 */

#ifndef FUNCWORD_H
#define FUNCWORD_H

#include "huarong_topology.h"
#include "multi_topology.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 位置画像累积——在喂料/对话路径调用（is_stop_word 判断之前！）
 * 真·率 EMA：每个 token 出现都更新位置槽（命中位置→向1靠近，未命中位置→向0衰减），
 * 句中 token（is_first=0 && is_last=0）也累加样本数，p0/p2 都向 0 衰减。
 * 修正 v0.5.20 初版"只增不减"缺陷（p0/p2 漂移到 1.0 → 位置轴退化）。 */
void funcword_record_position(ReasoningNode* node, int is_first, int is_last);

/* 周期影子扫描（从 gateway 挂调用，扫描全程持 master 读锁） */
void funcword_master_scan(MasterTopology* master);

/* 跨拓扑聚合统计——轴②(无强边)/轴③(通用度) 的数据源。
 * 对同一 concept 在 VOCAB+DOMAIN+CONCEPT 三个拓扑各 find_concept 一次，
 * 累加 edge_count、取全局 max_w、mean_w 用合并后总边重算。
 * 不能用 node_id 聚合：node_id 是拓扑内局部 id，跨拓扑会撞车。 */
typedef struct {
    int   edge_count;   /* VOCAB+DOMAIN+CONCEPT 三拓扑边数累加 */
    float max_w;        /* 三拓扑全局最大边权 */
    float mean_w;       /* 三拓扑合并总边重算的均值 */
} FuncwordAgg;

/* 供测试/调试：单节点分类
 * agg：跨拓扑聚合统计（NULL 时退化用节点自身边，仅单拓扑场景）
 * compound_score：构词轴得分（按 VOCAB node_id 索引，可 NULL）
 * is_func_out：输出虚词判定 */
typedef enum {
    FC_VOID,         /* 普通实词 */
    FC_HIGH_FREQ,    /* 高频实词 */
    FC_FUNCTION      /* 虚词/功能词 */
} FuncClass;
FuncClass funcword_classify_node(ReasoningNode* nd,
                                 const FuncwordAgg* agg,
                                 const float* compound_score,
                                 int* is_func_out);

#ifdef __cplusplus
}
#endif

#endif /* FUNCWORD_H */
