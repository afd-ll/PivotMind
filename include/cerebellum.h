/**
 * @file cerebellum.h
 * @brief 小脑 — 精细学习与运动协调（BPTT微调）
 *
 * 大脑类比：小脑负责精细运动协调和程序性记忆（骑自行车、弹钢琴）。
 * 系统映射：BPTT在线增量学习——在海马体大批量学习之后做精细调参。
 */

#ifndef CEREBELLUM_H
#define CEREBELLUM_H

#include "multi_topology.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Cerebellum Cerebellum;

/** 创建小脑 */
Cerebellum* cerebellum_create(int input_dim, int hidden_dim, int output_dim);

void cerebellum_destroy(Cerebellum* cb);

/** 执行一步微调学习 */
int cerebellum_micro_step(Cerebellum* cb, float* input, float* target);

/** 获取统计 */
void cerebellum_stats(Cerebellum* cb, int* steps, float* avg_loss);

#ifdef __cplusplus
}
#endif

#endif
