#ifndef FEATURE_LEARN_H
#define FEATURE_LEARN_H

#include "huarong_topology.h"

/**
 * 图拉普拉斯平滑：用共现邻居的加权平均更新特征向量
 *
 * 每轮迭代将每个节点的 features 更新为其所有邻居 features 的加权平均
 * （权重 = 边权重 × 边置信度），使语义相近的节点获得相似的向量表示。
 *
 * @param net    拓扑网络（节点必须有 features）
 * @param iter   迭代轮数（建议 3-5 轮）
 * @return 0=成功，-1=失败
 */
int feature_learn_graph_smooth(HuarongTopologyNet* net, int iterations);

#endif // FEATURE_LEARN_H
