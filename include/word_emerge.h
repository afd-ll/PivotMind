#ifndef WORD_EMERGE_H
#define WORD_EMERGE_H

/**
 * @file word_emerge.h
 * @brief 段二 · 词涌现（字对统计 → 词节点），判据全部可数、逐条可倒查
 *
 * 判据（默认值可用环境变量覆盖，便于调阈值而无需重编）：
 *   ① 次数 c(A,B) >= PIVOTMIND_EMERGE_MINCOUNT（默认 20）
 *   ② PMI        >= PIVOTMIND_EMERGE_MINPMI  （默认 2.5）
 *   ③ min(左熵,右熵) >= PIVOTMIND_EMERGE_MINENT（默认 2.0）
 *   单次最多新建 PIVOTMIND_EMERGE_MAX（默认 50）个词节点
 *
 * 为什么是这三条（CX-13 实测）：
 *   · 原判据「双向相对强度>=3.0」在真实数据上最大只有 2.53 ⇒ 永不达成；
 *     且真词是【有向】的（沸→点 有、点→沸 无），要求双向等于筛噪声。
 *   · PMI 先挡掉标点对与弱关联（了，/，我/，你 PMI 均 <3.0）。
 *   · 左右熵挡【跨词边界碎片】：叉编(0.00)/枢的(0.00)/有什(0.00)/的项(0.28)，
 *     而真词 min 熵普遍 >=3.7（编译3.71/问题3.70/玄枢4.11/一个6.99）。
 *
 * 落点：词节点进【概念拓扑】，并用 cross-link 回连其组成字（词汇拓扑），
 * 与既有 compound_promote 的架构一致（字拓扑保持纯单字）。
 */

struct MasterTopology;
typedef struct MasterTopology MasterTopology;

/** 跑一轮涌现。返回新建词节点数（0 = 没好候选或未达标）。 */
int word_emerge_tick(MasterTopology* master);

/** 判据快照（供日志/读数打印，只读）。 */
void word_emerge_thresholds(unsigned* min_count, double* min_pmi, double* min_ent, int* max_new);

#endif /* WORD_EMERGE_H */
