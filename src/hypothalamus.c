/**
 * @file hypothalamus.c
 * @brief 下丘脑实现 — 需求/动机调控
 *
 * 需求自然衰减 + 基线回归 + 外部刺激调制 + 昼夜耦合。
 */

#include "hypothalamus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* 需求衰减率（每 tick）：curiosity/social 衰减慢，hunger/comfort 衰减快 */
#define DRIVE_DECAY_CURIOSITY  0.002f
#define DRIVE_DECAY_HUNGER     0.005f
#define DRIVE_DECAY_SOCIAL     0.003f
#define DRIVE_DECAY_COMFORT    0.004f

/* 需求基线 */
#define DRIVE_BASELINE_CURIOSITY  0.50f
#define DRIVE_BASELINE_HUNGER     0.30f
#define DRIVE_BASELINE_SOCIAL     0.45f
#define DRIVE_BASELINE_COMFORT    0.55f

/* 刺激敏感度 */
#define DRIVE_SENSITIVITY_DEFAULT 0.15f

/* 昼夜调制幅度 */
#define CIRCADIAN_MODULATION 0.20f

/* 需求值边界 */
#define DRIVE_MIN 0.05f
#define DRIVE_MAX 0.95f

static float _clamp_drive(float v) {
    if (v < DRIVE_MIN) return DRIVE_MIN;
    if (v > DRIVE_MAX) return DRIVE_MAX;
    return v;
}

Hypothalamus* hypothalamus_create(CognitiveState* state) {
    if (!state) return NULL;

    Hypothalamus* h = (Hypothalamus*)calloc(1, sizeof(Hypothalamus));
    if (!h) return NULL;

    h->state = state;

    /* 初始化衰减率 */
    h->drive_decay[0] = DRIVE_DECAY_CURIOSITY;
    h->drive_decay[1] = DRIVE_DECAY_HUNGER;
    h->drive_decay[2] = DRIVE_DECAY_SOCIAL;
    h->drive_decay[3] = DRIVE_DECAY_COMFORT;

    /* 初始化基线 */
    h->drive_baseline[0] = DRIVE_BASELINE_CURIOSITY;
    h->drive_baseline[1] = DRIVE_BASELINE_HUNGER;
    h->drive_baseline[2] = DRIVE_BASELINE_SOCIAL;
    h->drive_baseline[3] = DRIVE_BASELINE_COMFORT;

    h->drive_sensitivity    = DRIVE_SENSITIVITY_DEFAULT;
    h->circadian_modulation = CIRCADIAN_MODULATION;

    printf("[下丘脑] 就绪 (驱动: 好奇=%.2f 获取=%.2f 社交=%.2f 舒适=%.2f)\n",
           state->drive_curiosity, state->drive_hunger,
           state->drive_social, state->drive_comfort);
    return h;
}

void hypothalamus_destroy(Hypothalamus* h) {
    free(h);
}

void hypothalamus_tick(Hypothalamus* h) {
    if (!h || !h->state) return;
    h->ticks++;

    CognitiveState* s = h->state;

    /* 需求自然衰减 + 向基线回归 */
    float* drives[4] = { &s->drive_curiosity, &s->drive_hunger,
                         &s->drive_social, &s->drive_comfort };
    for (int i = 0; i < 4; i++) {
        /* 向基线回归：decay 力度越大，回归越快 */
        float gap = h->drive_baseline[i] - *drives[i];
        *drives[i] += gap * h->drive_decay[i];
        *drives[i] = _clamp_drive(*drives[i]);
    }
}

void hypothalamus_on_dialog(Hypothalamus* h, float valence, float novelty) {
    if (!h || !h->state) return;
    h->dialog_events_processed++;

    CognitiveState* s = h->state;
    float sens = h->drive_sensitivity;

    /* 效价 → 社交/舒适驱动
     * 正效价（被夸奖）→ social↑, comfort↑
     * 负效价（被批评）→ comfort↓, hunger↑（寻求补偿） */
    s->drive_social  += valence * sens * 0.3f;
    s->drive_comfort += valence * sens * 0.2f;
    if (valence < 0) {
        s->drive_hunger += (-valence) * sens * 0.1f;  /* 负面情绪→寻求资源补偿 */
    }

    /* 新颖度 → 好奇驱动
     * 高新颖度 = 遇到没见过的东西 → 想多探索 */
    s->drive_curiosity += novelty * sens * 0.4f;

    /* 钳制 */
    s->drive_curiosity = _clamp_drive(s->drive_curiosity);
    s->drive_hunger    = _clamp_drive(s->drive_hunger);
    s->drive_social    = _clamp_drive(s->drive_social);
    s->drive_comfort   = _clamp_drive(s->drive_comfort);
}

void hypothalamus_set_circadian(Hypothalamus* h, float circadian) {
    if (!h || !h->state) return;

    /* 昼夜节律调制需求基线
     * 夜间（circadian < 0.3）：好奇↑ 社交↓（做梦/内省模式）
     * 白天（circadian > 0.6）：社交↑ 好奇↓（活跃互动模式） */
    float night_factor = 1.0f - circadian;  /* 夜间程度 */
    float day_factor   = circadian;         /* 白天程度 */

    h->drive_baseline[0] = DRIVE_BASELINE_CURIOSITY + night_factor * h->circadian_modulation;
    h->drive_baseline[2] = DRIVE_BASELINE_SOCIAL     + day_factor   * h->circadian_modulation;

    /* 钳制基线 */
    for (int i = 0; i < 4; i++) {
        if (h->drive_baseline[i] < DRIVE_MIN) h->drive_baseline[i] = DRIVE_MIN;
        if (h->drive_baseline[i] > DRIVE_MAX) h->drive_baseline[i] = DRIVE_MAX;
    }

    h->last_circadian = circadian;
}

float hypothalamus_get_drive(Hypothalamus* h, int drive_index) {
    if (!h || !h->state || drive_index < 0 || drive_index >= 4) return 0.0f;
    switch (drive_index) {
        case 0: return h->state->drive_curiosity;
        case 1: return h->state->drive_hunger;
        case 2: return h->state->drive_social;
        case 3: return h->state->drive_comfort;
        default: return 0.0f;
    }
}
