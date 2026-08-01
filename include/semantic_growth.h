/**
 * @file semantic_growth.h
 * @brief 语义拓扑自动生长 — 从词汇拓扑特征向量聚类生成语义节点
 */
#ifndef SEMANTIC_GROWTH_H
#define SEMANTIC_GROWTH_H

#include "multi_topology.h"

/** 余弦相似度阈值 — 高于此值的词汇节点归入同一语义聚类 */
#define SG_COSINE_THRESHOLD 0.02f  /* 极低：512维随机向量几乎正交，需更多Hebbian训练 */
/** 每轮最多采样的词汇节点数 */
#define SG_MAX_SAMPLE 200  /* ARM 保守值 */
/** 最少成员数才创建语义聚类节点 */
#define SG_MIN_CLUSTER_SIZE 2
/** 每轮最多新建的语义节点数，防止爆炸 */
#define SG_MAX_NEW_NODES 15

/**
 * 从词汇拓扑的特征向量聚类生成语义节点。
 * 采样激活值最高的词汇节点，按余弦相似度贪心聚类，
 * 达到最小成员数后创建语义聚类节点 + 跨拓扑链接。
 *
 * @param master 主拓扑
 * @return 本轮新建的语义节点数量
 */
int semantic_grow_from_vocab(MasterTopology* master);
/* v0.5.7: 词级语义场——从概念拓扑（词节点）聚类生成语义概念 */
int semantic_grow_from_concepts(MasterTopology* master);

#endif /* SEMANTIC_GROWTH_H */
