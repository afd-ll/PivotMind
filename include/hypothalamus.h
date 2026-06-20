/**
 * @file hypothalamus.h
 * @brief 下丘脑 — 需求/动机调控系统
 *
 * 大脑类比：
 *   下丘脑调节饥饿、渴、体温、昼夜节律、内分泌等内稳态需求。
 *   它是连接神经系统和内分泌系统的枢纽，决定"生物体想要什么"。
 *
 * 系统映射：
 *   1. 需求动态 — 管理 drive_curiosity / drive_hunger / drive_social / drive_comfort
 *   2. 自然衰减 — 各需求随时间向基线回归
 *   3. 外部刺激 — 对话事件（被夸奖→social+，被批评→comfort-）
 *   4. 昼夜耦合 — 夜间好奇↑（梦境探索），白天社交↑
 *   5. 丘脑信号 — 通过 THAL_SIG_DIALOG_EVENT 接收对话事件
 *
 * 与其他脑区的关系：
 *   - 通过丘脑接收 THAL_SIG_DIALOG_EVENT 调制需求
 *   - 与杏仁核 (Amygdala) 协作：杏仁核提供效价，下丘脑提供动机
 *   - 与脑干 (Brainstem) 协作：脑干提供昼夜节律，下丘脑据此调节需求基线
 */

#ifndef HYPOTHALAMUS_H
#define HYPOTHALAMUS_H

#include "cognitive_params.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 下丘脑句柄 */
typedef struct Hypothalamus {
    CognitiveState* state;          /* 指向认知状态（由 DialogSystem 管理） */

    /* 需求自然衰减率（每 tick） */
    float drive_decay[4];           /* [curiosity, hunger, social, comfort] */

    /* 需求基线（向此值回归） */
    float drive_baseline[4];

    /* 外部刺激敏感度 */
    float drive_sensitivity;        /* 对外部事件的响应幅度 (0-1) */

    /* 昼夜调制幅度 */
    float circadian_modulation;     /* 昼夜节律对需求的调制幅度 */

    /* 统计 */
    int   ticks;
    int   dialog_events_processed;
    float last_circadian;           /* 上一次的 circadian 值 */
} Hypothalamus;

/**
 * 创建下丘脑
 * @param state  认知状态指针（由 DialogSystem 管理生命周期）
 */
Hypothalamus* hypothalamus_create(CognitiveState* state);

void hypothalamus_destroy(Hypothalamus* h);

/**
 * 每 tick 更新：需求自然衰减 + 向基线回归
 */
void hypothalamus_tick(Hypothalamus* h);

/**
 * 对话事件后更新需求
 * @param h       下丘脑
 * @param valence 对话情绪效价 (-1~+1)
 * @param novelty 对话新颖度 (0~1)
 */
void hypothalamus_on_dialog(Hypothalamus* h, float valence, float novelty);

/**
 * 接收昼夜节律更新（由脑干每30tick推送）
 * @param h         下丘脑
 * @param circadian 当前昼夜活动值 (0~1)
 */
void hypothalamus_set_circadian(Hypothalamus* h, float circadian);

/**
 * 获取指定需求当前值
 */
float hypothalamus_get_drive(Hypothalamus* h, int drive_index);

#ifdef __cplusplus
}
#endif

#endif /* HYPOTHALAMUS_H */
