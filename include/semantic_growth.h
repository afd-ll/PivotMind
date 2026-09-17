/**
 * @file semantic_growth.h
 * @brief 语义拓扑自动生长 — 从词汇拓扑特征向量聚类生成语义节点
 */
#ifndef SEMANTIC_GROWTH_H
#define SEMANTIC_GROWTH_H

#include "multi_topology.h"

/** 余弦相似度阈值 — 高于此值的词汇节点归入同一语义聚类。
 *
 * [BG-24, 2026-09-17] 原值 0.02f，其注释口径是「256 维**随机向量**几乎正交」。
 * 🔴 但本函数读的特征**不是随机向量** —— 是运行中已被 `feature_learn_graph_smooth()`
 *    拉普拉斯平滑过的向量。实测（沙箱，vocab 2649 节点，平滑 3 步后 19900 对余弦）：
 *      p10 = 0.3178 · p50 = 0.5656 · p90 = 0.7711 · p97 = 0.8371 · **p99 = 0.8748** · max = 0.9883
 *    ⇒ 旧值 **0.02 落在该分布之外**（通过率 **99.92%**）⇒ **阈值不筛任何东西**，
 *      聚类的成员选择退化为「先到先得 + 偶然配对」（线上实测：814 个簇里 63.9% 是纯 2 元簇，
 *      最大 74 元簇是最早建的那批）。
 *
 * 取值依据：**p99 = 0.8748** ⇒ 通过率 ~1%（平均每节点 ~2 邻居）。
 *   阈值扫描（复刻聚类循环）：该值下建簇大小合理（最大 14 / 平均 4.1）；
 *   而 0.95 会退化成纯 2 元簇（失去聚类意义）⇒ 不宜再高。
 *
 * 回退开关：`PIVOTMIND_SG_THRESHOLD_LEGACY=1` ⇒ 用回旧值 0.02（对比实验 / 紧急回退）。 */
#define SG_COSINE_THRESHOLD 0.8748f
#define SG_COSINE_THRESHOLD_LEGACY 0.02f
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
