/**
 * @file broca.h
 * @brief 布罗卡区 — 语言产出（句式构建+模板生成）
 *
 * 大脑类比：布罗卡区负责语言的产出——语法组织、句式构建。
 * 系统映射：template_builder + dialog_generate 的统一入口。
 */

#ifndef BROCA_H
#define BROCA_H

#include "multi_topology.h"

#ifdef __cplusplus
extern "C" {
#endif

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
