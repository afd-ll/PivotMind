/**
 * @file amygdala.h
 * @brief 杏仁核 — 情绪效价调控 + 文化价值感知
 *
 * 大脑类比：
 *   杏仁核参与情绪处理、恐惧学习、效价评估。
 *   系统映射为认知状态中的 valence、emotion 各维度的更新源。
 *
 * 子拓扑归属：
 *   杏仁核拥有 [情绪拓扑, 文化拓扑]
 *   情绪拓扑中的节点 valence 值 → 认知状态 valence
 *   文化拓扑中的节点 → 价值观/规范偏好的偏置
 *
 * 与丘脑的关系：
 *   杏仁核通过丘脑信号总线上报情感采样结果，
 *   丘脑在 tick 中基于杏仁核反馈做 throttle 微调。
 */

#ifndef AMYGDALA_H
#define AMYGDALA_H

#include "multi_topology.h"
#include "thalamus.h"
#include "cognitive_params.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 杏仁核句柄 */
typedef struct Amygdala {
    MasterTopology* topology;       /* 多拓扑（访问情绪/文化拓扑） */
    Thalamus*       thalamus;       /* 丘脑信号总线 */
    CognitiveState* cognitive_state; /* 认知状态（更新 valence/emotion/explore_rate） */

    /* 统计 */
    long updates;                    /* 效价更新次数 */
    long ticks;                      /* tick 计数 */
} Amygdala;

/**
 * 创建杏仁核
 * @param topology  多拓扑网络
 * @param thalamus  丘脑信号总线（可选，用于反馈上报）
 * @param state     认知状态（可选，用于更新效价）
 */
Amygdala* amygdala_create(MasterTopology* topology,
                           Thalamus* thalamus,
                           CognitiveState* state);

void amygdala_destroy(Amygdala* amy);

/**
 * 每个脑干 tick 调用
 * 采样情绪拓扑 → 更新认知状态 valence / explore_rate
 * @return 采样节点数（用于反馈上报）
 */
int amygdala_tick(Amygdala* amy);

/**
 * 获取情绪拓扑平均效价（供外部查询）
 * @param max_samples 最多采样节点数
 */
float amygdala_mean_valence(Amygdala* amy, int max_samples);

#ifdef __cplusplus
}
#endif

#endif /* AMYGDALA_H */
