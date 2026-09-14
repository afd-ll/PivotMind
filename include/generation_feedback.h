/**
 * @file generation_feedback.h
 * @brief 生成端在线反馈 + 内调节（运行期状态，不落盘）
 *
 * 两条量：
 *   反馈   —— 本次生成"走出去多远"的即时观测量（在线观测，不是离线统计）
 *   内调节 —— 以自身历史为基准的稳态带；无外部目标 ⇒ 目标只能自生
 *
 * 说明：
 *   · 与 CognitiveState（情绪/驱力，连续量）并列，不属于它。
 *   · 运行期注入，**不持久化** —— 与 master->cognitive_state_ptr 同一先例。
 *   · 反馈量是结构化计数的确定性映射，非统计量 ⇒ 口径唯一、可复现。
 *   · 慢变量必须可观测 / 可复位 / 可关闭。
 */
#ifndef GENERATION_FEEDBACK_H
#define GENERATION_FEEDBACK_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GenerationFeedback {
    /* ── 反馈：本次生成走出去了多远 ── */
    int   reach_edges;     /* 走过的边数（vocab 层两跳的候选池条目数） */
    int   reach_words;     /* 凑到的实词数（diffusion_generate 的返回值） */
    float reach;           /* 合成量，归一化到 0~1 */

    /* ── 内调节：目标 = 自身稳态带 ── */
    float reach_mu;        /* 带的中心：reach 的滑动均值（不由人拍） */
    float reach_sigma;     /* 带的半宽：reach 的滑动标准差 */
    float regulation;      /* 纠正力 [-1,+1]：正=往上推，负=往下压（负反馈） */
    unsigned long samples; /* 参与统计的样本数（可观测） */

    /* ── 开关：0 = 完全旁路（等价于本机制不存在）── */
    int   enabled;
} GenerationFeedback;

/** 创建（enabled=1）。失败返回 NULL。 */
GenerationFeedback* generation_feedback_create(void);

/** 销毁 */
void generation_feedback_destroy(GenerationFeedback* fb);

/** 复位到"零样本"初态（可复位，便于实验与 A/B） */
void generation_feedback_reset(GenerationFeedback* fb);

/**
 * 写入一次观测：更新【反馈】量，并据此更新【内调节】的稳态带与纠正力。
 * @param edges 走过的边数（<0 视为 0）
 * @param words 凑到的实词数（<0 视为 0）
 * ⚠️ 逐次调用即可（在线）；不要传离线统计值进来，那会破坏"在线"语义。
 */
void generation_feedback_observe(GenerationFeedback* fb, int edges, int words);

#ifdef __cplusplus
}
#endif

#endif /* GENERATION_FEEDBACK_H */
