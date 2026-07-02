/**
 * @file cingulate.h
 * @brief 前扣带回 — 自我监控与错误检测
 *
 * 大脑类比：前扣带回(ACC)负责错误监测、冲突检测、行为评估。
 * 说话之前瞬间扫描自己的思维，发现矛盾立即触发修正。
 *
 * 系统映射：
 *   cingulate_evaluate() — 四维评分（语义+模板+情绪+长度）
 *   cingulate_approve()  — 决策：通过/回溯/重写
 */

#ifndef CINGULATE_H
#define CINGULATE_H

#include "multi_topology.h"
#include "diffusion.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_GENERATED_WORDS 64

/** 生成序列 + 评分 */
typedef struct {
    int    word_ids[MAX_GENERATED_WORDS];
    const char* words[MAX_GENERATED_WORDS];
    int    count;

    /* 四维评分 */
    float semantic_score;  /* 语义一致性 (0~1) */
    float template_score;  /* 模板匹配度 (0~1) */
    float emotion_score;   /* 情绪一致性 (0~1) */
    float length_score;    /* 长度合理性 (0~1) */
    float total_score;     /* 综合得分 */

    /* 回溯信息 */
    int   backtrack_step;  /* 从第几步开始重来 (-1=不需要) */
    const char* error_msg; /* 错误描述 */
} GeneratedSequence;

/** ACC 门控决策 */
typedef enum {
    CINGULATE_PASS      = 0,  /* 通过，直接输出 */
    CINGULATE_BACKTRACK = 1,  /* 局部错误，回溯到分叉点 */
    CINGULATE_REWRITE   = 2,  /* 完全失败，从头再来 */
} CingulateGate;

/**
 * 评估一个生成序列
 * @param seq    待评估的序列
 * @param topo   多拓扑
 * @param input  原始输入文本（用于语义一致性比对）
 * @param max_backtrack_depth 最大回溯深度
 */
void cingulate_evaluate(GeneratedSequence* seq,
                         MasterTopology* topo,
                         const char* input,
                         int max_backtrack_depth);

/**
 * 门控决策：通过/回溯/重写
 */
CingulateGate cingulate_gate(GeneratedSequence* seq, float threshold);

/**
 * 获取评分摘要（调试用）
 */
const char* cingulate_summary(GeneratedSequence* seq, char* buf, int size);

/**
 * 公共函数：扩散生成 + ACC 评估管线
 *
 * 对输入文本进行一次完整的 diffusion → cingulate_evaluate 流程，
 * 返回评估后的 GeneratedSequence。PFC/PFE/DMN 等脑区均可复用。
 *
 * @param topo       多拓扑网络
 * @param input      输入文本
 * @param temperature 扩散温度扰动 (0=关闭)
 * @param out_seq    输出：评估后的序列（调用者分配）
 * @return 生成词数，<2 表示生成失败
 */
int cingulate_diffusion_evaluate(MasterTopology* topo,
                                  const char* input,
                                  float temperature,
                                  struct EmergentPOS* emergent_pos,
                                  GeneratedSequence* out_seq);

#ifdef __cplusplus
}
#endif

#endif
