#ifndef TOPO_SNAPSHOT_H
#define TOPO_SNAPSHOT_H

#include "huarong_topology.h"
#include "multi_topology.h"

/**
 * @file topo_snapshot.h
 * @brief 拓扑快照 — 用于后台学习线程安全地计算
 *
 * 设计原则：
 * 1. 学习线程从主拓扑抓取只读快照
 * 2. 快照释放后学习线程在本地副本上计算
 * 3. 计算完毕将变更（新边、权重更新）合并回主拓扑
 * 4. 主拓扑在快照抓取和合并期间持有短锁
 */

/**
 * 待提交的新边
 */
typedef struct {
    int from_node_id;
    int to_node_id;
    float weight;
    float confidence;
} SnapshotEdge;

/**
 * 拓扑快照结构（只读拷贝，轻量级）
 * 只拷贝需要的字段，不拷贝完整 ReasoningNode
 */
typedef struct {
    int node_count;
    int topo_type;

    // 节点概念名（用于相似度计算）
    char** concepts;

    // 邻接表（用于 betweenness centrality）
    int* degrees;
    int** adj_lists;
    float** edge_weights;
    float** edge_confidences;

    // 特征向量（用于嵌入相似度）
    float* features_flat;
    int feature_dim;

    // 新发现的连接（处理过程中累积，合并时写回）
    int new_edge_capacity;
    int new_edge_count;
    SnapshotEdge* new_edges;

    // 节点重要性评分（处理结果）
    float* importance_scores;
} TopoSnapshot;

/**
 * 从子拓扑创建快照
 * 调用时需持有子拓扑所在 net 的读锁或写锁
 * @param sub 子拓扑（只读，不修改）
 * @return 快照指针，失败返回 NULL
 */
TopoSnapshot* topo_snapshot_create(SubTopology* sub);

/**
 * 销毁快照
 */
void topo_snapshot_destroy(TopoSnapshot* snap);

/**
 * 记录一条新发现的可选连接
 * @param snap 快照
 * @param from_id 起始节点ID
 * @param to_id 目标节点ID
 * @param weight 连接权重
 * @param confidence 置信度
 * @return 0=成功, -1=失败
 */
int topo_snapshot_add_edge(TopoSnapshot* snap, int from_id, int to_id,
                           float weight, float confidence);

/**
 * 将快照中累积的变更合并回主拓扑
 * 调用时需持有子拓扑所在 net 的写锁
 * @param snap 快照
 * @param sub 目标子拓扑
 * @return 合并的连接数
 */
int topo_snapshot_merge(TopoSnapshot* snap, SubTopology* sub);

#endif // TOPO_SNAPSHOT_H
