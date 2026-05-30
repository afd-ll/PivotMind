#ifndef FEATURE_PRETRAIN_H
#define FEATURE_PRETRAIN_H

#include "pretrain.h"
#include "huarong_topology.h"
#include "multi_topology.h"

/**
 * 将预训练 Word2Vec 嵌入迁移到拓扑节点特征向量
 *
 * 使用随机投影降维 (Johnson-Lindenstrauss 保证近似保持余弦相似度)
 * 对 pretrain vocab 中不存在的概念, 该节点保持原有特征不变
 *
 * @param master   主拓扑
 * @param pretrain 已训练的预训练状态 (含 embedding_layer)
 * @return 成功迁移的节点数, -1 失败
 */
int feature_transfer_pretrained(MasterTopology* master, PretrainState* pretrain);

#endif /* FEATURE_PRETRAIN_H */
