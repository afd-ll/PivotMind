/**
 * @file batch_learn_lowmem.c
 * @brief 低内存版批量喂入工具 — 去掉周期性跨拓扑重建
 *
 * 与 batch_learn.c 相同，只是将跨拓扑重建延后到训练结束时一次性做。
 * 适用于 Zero 2W（416MB RAM）等内存受限设备。
 */
/* ⚠️ 本变体当前与 batch_learn 等价（见 batch_learn.c 中 CROSS_REBUILD_INTERVAL 处的
 *    [待决策] 说明）：LOW_MEM 只影响那个已无人引用的宏，故二者二进制逐字节相同。
 *    本文件曾写 DISABLE_PERIODIC_REBUILD（早已不存在的旧宏名），v0.5.31 已更正为 LOW_MEM，
 *    但更正的是“能编译对”，不等于“行为有别”——差异仍需另行接线。 */
#define LOW_MEM
#include "batch_learn.c"
