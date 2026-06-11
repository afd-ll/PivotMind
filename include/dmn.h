/**
 * @file dmn.h
 * @brief 默认模式网络 — 梦境引擎 + 自发联想
 *
 * 大脑类比：
 *   当大脑不专注于外部任务时，默认模式网络活跃——
 *   产生自发联想、记忆回放、创意涌现。
 *
 * 系统映射：
 *   dmn_cycle() 执行一轮梦境游走，参数受 throttle 调制
 */

#ifndef DMN_H
#define DMN_H

#include "dream_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 复用 DreamConfig，加 throttle 感知 */
typedef DreamConfig DmnConfig;

#define DMN_DEFAULT_CONFIG DREAM_DEFAULT_CONFIG

/**
 * 执行一轮梦境（受丘脑 throttle 调制）
 * @param throttle 丘脑 THAL_DMN 信号 (0.0=暂停, 1.0=全速)
 * @return 边修改数
 */
int dmn_cycle(MasterTopology* master, MemorySystem* memory,
              const DmnConfig* config, float throttle);

#ifdef __cplusplus
}
#endif

#endif
