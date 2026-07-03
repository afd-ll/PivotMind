/**
 * @file json_config.h
 * @brief 运行时配置系统 — 替代硬编码常量的统一入口
 *
 * 启动时从 pivotmind_config.json 加载。
 * 文件缺失时全部使用 constants.h 默认值，零影响。
 */
#ifndef JSON_CONFIG_H
#define JSON_CONFIG_H

#include "common.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 顶级配置组数量上限 */
#define CONFIG_GROUP_MAX 8
#define CONFIG_KEY_MAX   32
#define CONFIG_PATH_MAX  512

/* ── 脑区开关标志 ── */
typedef struct {
    bool prefrontal;
    bool hippocampus;
    bool dmn;
    bool perception;
    bool broca;
    bool cerebellum;
    bool amygdala;
    bool hypothalamus;
    bool visual_cortex;     /* v0.5 视觉皮层脑区 */
} BrainRegionConfig;

/* ── 拓扑参数 ── */
typedef struct {
    int   feature_dim;
    int   max_nodes_per_topo;
    int   cross_hit_table_size;
} TopologyConfig;

/* ── 学习参数 ── */
typedef struct {
    float decay_rate;
    float learn_rate;
    int   autonomic_shard_count;
    int   active_learner_interval;
    int   max_connections;
    int   flush_threshold;
    int   idle_flush_seconds;
} LearningConfig;

/* ── 推理参数 ── */
typedef struct {
    int   max_response_len;
    int   default_hop_count;
    int   max_associations;
    int   max_hops_reasoning;
} InferenceConfig;

/* ── 后台时钟参数 ── */
typedef struct {
    int   tick_interval_ms;
    float decay_per_tick;
    float spontaneous_prob;
    float spontaneous_strength;
    int   consolidate_interval;
} ClockConfig;

/* ── 全局配置上下文 ── */
typedef struct {
    TopologyConfig   topology;
    LearningConfig   learning;
    InferenceConfig  inference;
    ClockConfig      clock;
    BrainRegionConfig brain_regions;
    int              loaded;  /* 是否成功加载了配置文件 */
} ConfigContext;

/**
 * 加载配置文件
 * @param path  配置路径（NULL 则尝试 "pivotmind_config.json"）
 * @return      配置上下文（调用方负责 free）
 */
ConfigContext* config_load(const char* path);

/**
 * 释放配置上下文
 */
void config_destroy(ConfigContext* ctx);

/**
 * 写入默认配置文件模板
 * @param path  输出路径
 * @return      0=成功
 */
int config_write_default(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* JSON_CONFIG_H */
