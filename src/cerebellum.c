/**
 * @file cerebellum.c
 * @brief 小脑实现 — 资源平衡 + 负载调速
 */

#include "cerebellum.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Cerebellum* cerebellum_create(void) {
    Cerebellum* cb = (Cerebellum*)calloc(1, sizeof(Cerebellum));
    if (!cb) return NULL;
    cb->cpu_throttle = 1.0f;
    cb->mem_throttle = 1.0f;
    cb->learn_brake  = 1.0f;
    cb->mem_high_water = 3.0f;  /* 3GB 警告线 */
    printf("[小脑] 就绪 (CPU保护=%s, 内存保护=%s)\n",
           "ON", "ON");
    return cb;
}

void cerebellum_destroy(Cerebellum* cb) { free(cb); }

float cerebellum_tick(Cerebellum* cb, float cpu_pct, float mem_gb, float circadian) {
    if (!cb) return 1.0f;

    cb->tick_count++;
    cb->mem_usage_gb = mem_gb;

    /* CPU 滑动平均 */
    cb->cpu_history[cb->cpu_pos] = cpu_pct;
    cb->cpu_pos = (cb->cpu_pos + 1) % 16;
    float sum = 0;
    int n = 0;
    for (int i = 0; i < 16; i++) {
        if (cb->cpu_history[i] > 0) { sum += cb->cpu_history[i]; n++; }
    }
    cb->cpu_avg = n > 0 ? sum / n : 0;

    /* CPU 保护：>70% 开始踩刹车 */
    if (cb->cpu_avg > 90.0f)      cb->cpu_throttle = 0.1f;
    else if (cb->cpu_avg > 80.0f) cb->cpu_throttle = 0.3f;
    else if (cb->cpu_avg > 70.0f) cb->cpu_throttle = 0.6f;
    else if (cb->cpu_avg > 50.0f) cb->cpu_throttle = 0.85f;
    else                          cb->cpu_throttle = 1.0f;

    /* 内存保护：超过高水位就减速 */
    if (mem_gb >= cb->mem_high_water)
        cb->mem_throttle = 0.3f;
    else if (mem_gb >= cb->mem_high_water * 0.85f)
        cb->mem_throttle = 0.6f;
    else
        cb->mem_throttle = 1.0f;

    /* 学习刹车：CPU和内存双重保护下取最保守值 */
    cb->learn_brake = cb->cpu_throttle < cb->mem_throttle
                      ? cb->cpu_throttle : cb->mem_throttle;

    /* 沉睡期额外降速 */
    if (circadian < 0.3f) {
        cb->cpu_throttle *= 0.5f;
        cb->learn_brake  *= 0.3f;
    }

    /* 综合 throttle：取所有保护的最小值 */
    float combined = cb->cpu_throttle;
    if (cb->mem_throttle < combined) combined = cb->mem_throttle;

    if (cb->tick_count % 600 == 0 && (cb->cpu_throttle < 0.8f || cb->mem_throttle < 0.8f)) {
        printf("[小脑] 保护模式: CPU=%.0f%%→throttle=%.1f, MEM=%.1fG→throttle=%.1f, 学习刹车=%.1f\n",
               cb->cpu_avg, cb->cpu_throttle, mem_gb, cb->mem_throttle, cb->learn_brake);
    }

    return combined;
}

float cerebellum_cpu_protect(Cerebellum* cb)  { return cb ? cb->cpu_throttle : 1.0f; }
float cerebellum_mem_protect(Cerebellum* cb)  { return cb ? cb->mem_throttle : 1.0f; }
float cerebellum_learn_brake(Cerebellum* cb)   { return cb ? cb->learn_brake  : 1.0f; }
