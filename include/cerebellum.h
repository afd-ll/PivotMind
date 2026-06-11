/**
 * @file cerebellum.h
 * @brief 小脑 — 系统平衡与负载调节
 *
 * 大脑类比：小脑负责运动协调、平衡维持、肌张力调节。
 * 系统映射：
 *   - CPU 监控 → 心跳过快时踩刹车
 *   - 内存监控 → 涨太快要冻节点
 *   - 温度监控 → 过高时全局降速
 *   - 学习速率调节 → 太快时减速防过拟合
 *   - 睡眠保护 → 与脑干昼夜节律联动
 *
 * 与丘脑的关系：
 *   丘脑负责"往哪走"（感觉中继+注意力调度）
 *   小脑负责"走多快"（资源平衡+速度控制）
 */

#ifndef CEREBELLUM_H
#define CEREBELLUM_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Cerebellum {
    /* 监控窗口 */
    float cpu_history[16];        /* 环形CPU历史 */
    int   cpu_pos;
    float cpu_avg;

    float mem_usage_gb;           /* 当前内存使用 */
    float mem_high_water;         /* 内存高水位 */

    /* 输出信号 */
    float cpu_throttle;           /* CPU 保护系数 (1.0=正常) */
    float mem_throttle;           /* 内存保护系数 */
    float learn_brake;            /* 学习刹车 (1.0=正常) */

    int tick_count;
} Cerebellum;

Cerebellum* cerebellum_create(void);
void cerebellum_destroy(Cerebellum* cb);

/**
 * 每个 tick 更新监控数据，输出限速信号
 * @param cpu_pct  当前 CPU 使用率 (0~100)
 * @param mem_gb   当前内存使用 (GB)
 * @param circadian 昼夜节律系数
 * @return 综合 throttle (所有保护系数相乘)
 */
float cerebellum_tick(Cerebellum* cb, float cpu_pct, float mem_gb, float circadian);

/** 获取 CPU 保护系数 */
float cerebellum_cpu_protect(Cerebellum* cb);

/** 获取内存保护系数 */
float cerebellum_mem_protect(Cerebellum* cb);

/** 获取学习刹车 */
float cerebellum_learn_brake(Cerebellum* cb);

#ifdef __cplusplus
}
#endif

#endif
