/**
 * @file intent_router.h
 * @brief 意图概率路由 —— 「目标统筹」的概率层
 *
 * 为什么要有这一层（2026-10-27）：
 *   现在的意图识别是 `recognize_intent()`（dialog_system.c），关键词第一个命中就返回
 *   **单一意图**。这有两个毛病：
 *     ① 没有概率 —— 无法表达「这句话既像解释又像查询」；
 *     ② 关键词表外的说法永远落到 default，冷门意图（FEEDBACK/TEST）永远触发不到。
 *
 * 设计照 DeepSeekMoE 的路由（不是 Transformer 的 QKV 注意力，玄枢是符号系统，用不上梯度）：
 *   · routed experts   = 6 种意图，各自一套拓扑配比
 *   · shared expert    = default_base（永远按比例保留一份，不被路由覆盖）
 *   · affinity s_i     = 朴素贝叶斯  P(intent|输入) ∝ P(intent)·Π P(字|intent)
 *                        （解析解，不需要梯度）
 *   · top-K + softmax  = 只在 top-K 内归一化，得到路由权重 g_i
 *   · bias 平衡        = auxiliary-loss-free：过载减、欠载加，不进任何梯度
 *
 * 训练不用人工标注：用现有 `recognize_intent()` 对语料问句做**自举打标**，
 * 规则只点火，概率才是产出。
 */
#ifndef INTENT_ROUTER_H
#define INTENT_ROUTER_H

#include "constants.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IR_INTENT_MAX   10     /* 与 DialogIntent 0..9 对应 */
#define IR_TOP_K         2     /* 只激活最可能的 2 个意图 */

/** 一次路由的概率结果 */
typedef struct {
    float prob[IR_INTENT_MAX];   /* 归一化后的概率分布（全 10 类） */
    int   top[IR_TOP_K];         /* 概率最高的若干意图（index = DialogIntent） */
    float g[IR_TOP_K];           /* 在 top-K 内重新归一化的路由权重，和 = 1 */
    int   n;                     /* 实际有效的 top 个数（模型不可用时为 0） */
    float entropy;               /* 分布熵（bit）；越低越确定 —— 温度调度的依据 */
} IntentDist;

/** 懒加载意图模型（优先读 data/intent_model.bin，缺失则不可用）。返回 1 就绪。 */
int ir_ensure(void);

/** 对输入做概率路由。返回 top 个数（0 = 模型不可用，调用方应回退硬分类）。 */
int ir_classify(const char* text, IntentDist* out);

/**
 * 把概率分布换算成【子拓扑权重】。
 * 公式：out[t] = alpha·shared[t] + (1-alpha)·Σ_i g_i·base(top_i)[t]
 * shared = get_intent_base_weights(INTENT_UNKNOWN)（即原 default_base）
 * 返回 1 成功 / 0 失败（调用方保持原权重）。
 */
int ir_mixed_topology_weights(const IntentDist* d, float* out, int n_topo);

/** 从「问|答」语料训练（自举打标 + 计数）。返回训练行数，<0 失败。 */
int ir_train(const char* pipe_path, int max_lines);

int ir_load(const char* path);
int ir_save(const char* path);

/** 读数：意图数 / 词表大小 / 平均熵 / 是否就绪。指针可传 NULL。 */
void ir_stats(int* n_intent, int* n_vocab, float* last_entropy, int* ready);

/** 当前各意图的负载均衡 bias（诊断用，可传 NULL）。 */
void ir_biases(float* out, int max);

#ifdef __cplusplus
}
#endif

#endif /* INTENT_ROUTER_H */
