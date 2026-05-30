/**
 * @file batch_learn_lowmem.c
 * @brief 低内存版批量喂入工具 — 去掉周期性跨拓扑重建
 *
 * 与 batch_learn.c 相同，只是将跨拓扑重建延后到训练结束时一次性做。
 * 适用于 Zero 2W（416MB RAM）等内存受限设备。
 */
#define DISABLE_PERIODIC_REBUILD
#include "batch_learn.c"
