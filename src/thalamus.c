/**
 * @file thalamus.c
 * @brief 丘脑调度器实现
 */

#include "thalamus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ================================================================
 *  子系统名称（调试用）
 * ================================================================ */

static const char* SYS_NAMES[THAL_SUBSYSTEM_COUNT] __attribute__((unused)) = {
    "前额叶(对话)",
    "海马体(学习)",
    "默认模式网络(梦境)",
    "感知(联网)",
    "布罗卡区(句式)",
    "小脑(微调)"
};

/* ================================================================
 *  创建 / 销毁
 * ================================================================ */

Thalamus* thalamus_create(void) {
    Thalamus* th = (Thalamus*)calloc(1, sizeof(Thalamus));
    if (!th) return NULL;

    /* 默认：所有子系统全速 */
    for (int i = 0; i < THAL_SUBSYSTEM_COUNT; i++) {
        th->throttle[i] = 1.0f;
    }

    /* 可配置默认值 */
    th->dialog_window_seconds  = 3600;
    th->idle_threshold_dialogs = 2;
    th->busy_threshold_dialogs = 10;
    th->learner_slow_ms        = 5000.0f;
    th->cpu_high_threshold     = 0.85f;

    th->circadian = 0.5f;
    th->circadian_phase = "active";

    pthread_mutex_init(&th->lock, NULL);
    printf("[丘脑] 调度器就绪 (子系统=%d)\n", THAL_SUBSYSTEM_COUNT);
    return th;
}

void thalamus_destroy(Thalamus* th) {
    if (!th) return;
    pthread_mutex_destroy(&th->lock);
    free(th);
}

/* ================================================================
 *  感知更新
 * ================================================================ */

void thalamus_update_sensors(Thalamus* th, int dialogs_1h, float learner_ms,
                              int hot_nodes, float cpu) {
    if (!th) return;
    pthread_mutex_lock(&th->lock);
    if (dialogs_1h >= 0) th->dialogs_per_hour = dialogs_1h;
    if (learner_ms  >= 0) th->learner_load_ms = learner_ms;
    if (hot_nodes   >= 0) th->hot_node_count = hot_nodes;
    if (cpu         >= 0) th->cpu_usage = cpu;
    pthread_mutex_unlock(&th->lock);
}

void thalamus_set_circadian(Thalamus* th, float circadian, const char* phase) {
    if (!th) return;
    pthread_mutex_lock(&th->lock);
    th->circadian = circadian;
    if (phase) th->circadian_phase = phase;
    pthread_mutex_unlock(&th->lock);
}

void thalamus_record_dialog(Thalamus* th) {
    if (!th) return;
    pthread_mutex_lock(&th->lock);
    th->dialogs_total++;
    th->dialogs_per_hour++;
    pthread_mutex_unlock(&th->lock);
}

/* ================================================================
 *  决策引擎
 * ================================================================ */

/**
 * 对话频率 → 繁忙度 (0.0=完全空闲, 1.0=极度繁忙)
 */
static float compute_busyness(Thalamus* th) {
    if (th->dialogs_per_hour <= th->idle_threshold_dialogs) return 0.0f;
    if (th->dialogs_per_hour >= th->busy_threshold_dialogs) return 1.0f;
    float range = (float)(th->busy_threshold_dialogs - th->idle_threshold_dialogs);
    return (float)(th->dialogs_per_hour - th->idle_threshold_dialogs) / range;
}

static const char* g_last_reason = "init";

void thalamus_tick(Thalamus* th) {
    if (!th) return;

    pthread_mutex_lock(&th->lock);
    th->tick_count++;

    float busy = compute_busyness(th);

    /* ── 决策逻辑 ── */

    if (busy > 0.6f) {
        /* 繁忙：对话优先，其他全压 */
        th->throttle[THAL_PREFRONTAL]  = 1.0f;
        th->throttle[THAL_HIPPOCAMPUS] = 0.3f - 0.2f * busy;  /* 0.1~0.3 */
        th->throttle[THAL_DMN]         = 0.0f;                 /* 暂停梦境 */
        th->throttle[THAL_PERCEPTION]  = 0.0f;                 /* 暂停联网 */
        th->throttle[THAL_BROCA]       = 1.0f;                 /* 句式要跟对话联动 */
        th->throttle[THAL_CEREBELLUM]  = 0.2f;                 /* 微调压低 */
        th->active_subsystem = THAL_PREFRONTAL;
        g_last_reason = "对话繁忙，抑制非必要子系统";

    } else if (busy > 0.2f) {
        /* 正常：全跑但非优先的降速 */
        th->throttle[THAL_PREFRONTAL]  = 1.0f;
        th->throttle[THAL_HIPPOCAMPUS] = 0.7f;
        th->throttle[THAL_DMN]         = 0.5f;
        th->throttle[THAL_PERCEPTION]  = 0.3f;
        th->throttle[THAL_BROCA]       = 0.8f;
        th->throttle[THAL_CEREBELLUM]  = 0.5f;
        th->active_subsystem = THAL_PREFRONTAL;
        g_last_reason = "正常负载，各系统降速运行";

    } else {
        /* 空闲：优先巩固+联网，然后梦境探索 */
        /* 检查是否有积压的冷却节点需要审查 */
        if (th->cooled_node_count > 100) {
            /* 冷却节点多 → 优先巩固整理 */
            th->throttle[THAL_HIPPOCAMPUS] = 0.9f;
            th->throttle[THAL_PERCEPTION]  = 0.6f;  /* 巩固时可联网查证 */
            th->throttle[THAL_DMN]         = 0.2f;
            th->throttle[THAL_PREFRONTAL]  = 0.1f;
            th->active_subsystem = THAL_HIPPOCAMPUS;
            g_last_reason = "空闲+冷却节点积压，优先巩固+联网查证";

        } else if (th->learner_load_ms > th->learner_slow_ms) {
            /* 学习器积压 → 专注训练 */
            th->throttle[THAL_HIPPOCAMPUS] = 1.0f;
            th->throttle[THAL_DMN]         = 0.1f;
            th->throttle[THAL_PERCEPTION]  = 0.3f;
            th->throttle[THAL_PREFRONTAL]  = 0.1f;
            th->active_subsystem = THAL_HIPPOCAMPUS;
            g_last_reason = "空闲+训练积压，专注学习";

        } else {
            /* 完全空闲 → 做梦 + 好奇探索 */
            th->throttle[THAL_DMN]         = 0.8f;
            th->throttle[THAL_PERCEPTION]  = 0.7f;
            th->throttle[THAL_HIPPOCAMPUS] = 0.4f;
            th->throttle[THAL_PREFRONTAL]  = 0.1f;
            th->throttle[THAL_BROCA]       = 0.3f;
            th->throttle[THAL_CEREBELLUM]  = 0.3f;
            th->active_subsystem = THAL_DMN;
            g_last_reason = "完全空闲，梦境+好奇探索";
        }
    }

    /* ── 昼夜调制：沉睡期全局压降 ── */
    if (th->circadian < 0.3f) {
        float sleep_factor = th->circadian * 3.0f;  /* 0.3→1.0 */
        for (int i = 0; i < THAL_SUBSYSTEM_COUNT; i++) {
            if (i == THAL_PREFRONTAL) continue;  /* 对话不受睡眠影响 */
            th->throttle[i] *= sleep_factor;
        }
    }

    /* ── CPU 保护：过载时全局降速 ── */
    if (th->cpu_usage > th->cpu_high_threshold) {
        float cpu_protect = 1.0f - (th->cpu_usage - th->cpu_high_threshold) * 2.0f;
        if (cpu_protect < 0.1f) cpu_protect = 0.1f;
        for (int i = 0; i < THAL_SUBSYSTEM_COUNT; i++) {
            if (i == THAL_PREFRONTAL) continue;
            th->throttle[i] *= cpu_protect;
        }
        g_last_reason = "CPU过载保护，全局降速";
    }

    /* ── 记录历史 ── */
    if (th->throttle_history_pos < 32) {
        for (int i = 0; i < THAL_SUBSYSTEM_COUNT; i++) {
            th->throttle_history[i][th->throttle_history_pos] = th->throttle[i];
        }
        th->throttle_history_pos++;
    } else {
        /* 环形覆盖 */
        for (int i = 0; i < THAL_SUBSYSTEM_COUNT; i++) {
            th->throttle_history[i][th->tick_count % 32] = th->throttle[i];
        }
    }

    pthread_mutex_unlock(&th->lock);
}

/* ================================================================
 *  查询
 * ================================================================ */

float thalamus_get_throttle(Thalamus* th, ThalamusSubsystem subsystem) {
    if (!th || subsystem < 0 || subsystem >= THAL_SUBSYSTEM_COUNT) return 1.0f;
    float val;
    pthread_mutex_lock(&th->lock);
    val = th->throttle[subsystem];
    pthread_mutex_unlock(&th->lock);
    return val;
}

const char* thalamus_phase_description(Thalamus* th) {
    if (!th) return "unknown";
    float busy;
    pthread_mutex_lock(&th->lock);
    busy = compute_busyness(th);
    pthread_mutex_unlock(&th->lock);

    if (busy > 0.6f) return "对话繁忙";
    if (busy > 0.2f) return "正常运行";
    return "空闲探索";
}

const char* thalamus_decision_reason(Thalamus* th) {
    (void)th;
    return g_last_reason;
}

void thalamus_report(Thalamus* th, int consolidated, int searched, int dreamed) {
    if (!th) return;
    pthread_mutex_lock(&th->lock);
    if (consolidated >= 0) th->fb_hippo_consolidated = consolidated;
    if (searched     >= 0) th->fb_percept_searched   = searched;
    if (dreamed      >= 0) th->fb_dmn_dreamed        = dreamed;
    pthread_mutex_unlock(&th->lock);
}
