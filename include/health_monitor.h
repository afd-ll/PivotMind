/**
 * @file health_monitor.h
 * @brief 内感受自检模块 — AI 的"身体觉察"
 *
 * 类比：人觉得头疼/发烧/疲劳 → 自动减负。
 * AI 检测自己的 RSS、连接膨胀、冻结速率 → 自动调度器干预。
 *
 * 三级状态：
 *   GREEN  — 正常运转
 *   YELLOW — 预警：增 throttle、减感知频率、加速衰减
 *   RED    — 紧急：强制存状态、猛砍弱边、暂停外部学习
 *
 * 接入调度器 (CognitiveController) 做统一决策。
 */

#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

#include "multi_topology.h"
#include "cognitive_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HM_GREEN  = 0,
    HM_YELLOW = 1,
    HM_RED    = 2
} HealthLevel;

typedef struct {
    /* 当前快照 */
    float rss_mb;            /* 当前 RSS */
    float rss_growth_mb_min; /* RSS 增速 */
    int   total_conns;        /* 连接总数 */
    int   conn_growth;        /* 上次快照后的连接增长 */
    int   frozen_nodes;       /* 冻结节点数 */
    int   total_nodes;        /* 总节点 */

    /* 阈值 (可配置) */
    float rss_yellow_mb;        /* RSS 黄线 (默认 520) */
    float rss_red_mb;           /* RSS 红线 (默认 560) */
    float rss_growth_yellow;    /* 增速黄线 (默认 0.5MB/min) */
    float rss_growth_red;       /* 增速红线 (默认 1.0MB/min) */
    int   conn_growth_yellow;   /* 连接涨速黄线 (默认 500/tick群) */
    int   conn_growth_red;      /* 连接涨速红线 (默认 2000) */
    int   frozen_yellow;        /* 冻结黄线 (默认 500) */
    int   frozen_red;           /* 冻结红线 (默认 2000) */

    /* 历史 (用于计算增速) */
    float last_rss_mb;
    int   last_total_conns;
    int   last_tick;
    int   ticks_since_check;

    /* 当前状态 */
    HealthLevel level;
    const char* reason;       /* 触因描述 */

    /* 干预历史 */
    int interventions;
    int emergency_saves;
} HealthMonitor;

/** 创建 */
HealthMonitor* health_monitor_create(void);

/** 每 N tick 调用一次，传入当前拓扑和调度器 */
void health_monitor_tick(HealthMonitor* hm,
                         MasterTopology* master,
                         CognitiveController* controller);

/** 获取当前健康等级 */
HealthLevel health_get_level(HealthMonitor* hm);

/** 销毁 */
void health_monitor_destroy(HealthMonitor* hm);

#ifdef __cplusplus
}
#endif

#endif
