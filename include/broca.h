/**
 * @file broca.h
 * @brief 布罗卡区 — 语言产出（句式构建+模板生成）
 *
 * 大脑类比：布罗卡区负责语言的产出——语法组织、句式构建。
 * 系统映射：template_builder + dialog_generate 的统一入口。
 *
 * v0.4: Broca 拥有自己的状态，自主管理模板构建调度，
 *       不再由脑干硬编码 build interval。
 */

#ifndef BROCA_H
#define BROCA_H

#include "multi_topology.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 布罗卡区状态 */
typedef struct Broca {
    MasterTopology* master;         /* 多拓扑网络 */
    int   build_interval_ticks;     /* 模板构建间隔 (tick数, 默认300) */
    int   max_build_depth;          /* 模板构建最大深度 */
    int   decay_threshold;          /* 衰减阈值 */
    float decay_rate;               /* 衰减率 */
    int   tick_count;               /* 累计 tick 计数 */
    int   total_builds;             /* 累计构建次数 */
    int   total_new_templates;      /* 累计新增模板数 */
} Broca;

/**
 * 创建布罗卡区
 */
Broca* broca_create(MasterTopology* master);

void broca_destroy(Broca* b);

/**
 * 每 tick 入口 — 自主管理模板构建与衰减调度
 * @return 本次新增模板数
 */
int broca_tick(Broca* b);

/** 模板构建（每N轮对话增量） */
int broca_build_templates(MasterTopology* topology, int count, int max_depth);

/** 模板衰减 */
void broca_decay_templates(MasterTopology* topology, int threshold, float decay);

/** 获取模板统计 */
int broca_template_count(MasterTopology* topology);

#ifdef __cplusplus
}
#endif

#endif
