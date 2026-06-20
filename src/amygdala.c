/**
 * @file amygdala.c
 * @brief 杏仁核实现 — 从情绪拓扑采样效价，更新认知状态
 */

#include "amygdala.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ================================================================
 *  创建 / 销毁
 * ================================================================ */

Amygdala* amygdala_create(MasterTopology* topology,
                           Thalamus* thalamus,
                           CognitiveState* state) {
    if (!topology) return NULL;

    Amygdala* amy = (Amygdala*)calloc(1, sizeof(Amygdala));
    if (!amy) return NULL;

    amy->topology       = topology;
    amy->thalamus       = thalamus;
    amy->cognitive_state = state;

    printf("[杏仁核] 就绪 (情绪/文化拓扑归属)\n");
    return amy;
}

void amygdala_destroy(Amygdala* amy) {
    free(amy);
}

/* ================================================================
 *  效价采样
 * ================================================================ */

float amygdala_mean_valence(Amygdala* amy, int max_samples) {
    if (!amy || !amy->topology) return 0.0f;

    /* 找情绪拓扑 */
    SubTopology* emo = master_get_sub_topology_by_type(amy->topology, TOPO_EMOTION);
    if (!emo || !emo->net || emo->net->node_count == 0) return 0.0f;

    /* 采样前 N 个节点求平均效价 */
    int n = emo->net->node_count < max_samples
            ? emo->net->node_count : max_samples;
    float sum = 0.0f;
    int count = 0;
    for (int i = 0; i < n; i++) {
        ReasoningNode* node = emo->net->nodes[i];
        if (node) { sum += node->valence; count++; }
    }
    return count > 0 ? sum / count : 0.0f;
}

/* ================================================================
 *  tick 入口 — 被脑干每 30tick 左右调用
 * ================================================================ */

int amygdala_tick(Amygdala* amy) {
    if (!amy) return 0;
    amy->ticks++;

    /* 从情绪拓扑采样平均效价 */
    float topo_val = amygdala_mean_valence(amy, 20);

    /* 更新认知状态中的 valence */
    if (amy->cognitive_state) {
        CognitiveState* state = amy->cognitive_state;

        /* EMA 平滑融合：新采样占 10%，历史占 90% */
        state->valence = state->valence * 0.9f + topo_val * 0.1f;

        /* 自然衰减（向 0 回归） */
        state->valence *= 0.998f;
        if (fabsf(state->valence) < 0.001f) state->valence = 0.0f;

        /* 效价影响探索率：效价高→多利用；效价低（负）→多探索 */
        state->explore_rate = 0.5f + state->valence * 0.5f;
        if (state->explore_rate < 0.05f) state->explore_rate = 0.05f;
        if (state->explore_rate > 0.95f) state->explore_rate = 0.95f;

        amy->updates++;
    }

    /* 通过丘脑信号总线反馈上报 */
    if (amy->thalamus) {
        thalamus_send_feedback(amy->thalamus, THAL_AMYGDALA,
                                1,         /* 1次效价更新 */
                                -1, -1);   /* 无搜索/梦境 */
    }

    return 1;
}
