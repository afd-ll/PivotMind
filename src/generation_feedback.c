/**
 * @file generation_feedback.c
 * @brief 生成端在线反馈 + 内调节（运行期状态，不落盘）实现
 */
#include "generation_feedback.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* 滑动统计的学习率（越小越慢 ⇒ 稳态带变化越慢）。
 * 要求：稳态带必须远慢于单次生成（时间尺度不能过近）。 */
#define GF_EMA_ALPHA      0.05f

/* 稳态带半宽 = K × 滑动标准差 */
#define GF_BAND_K         2.0f

/* σ 下限：防"零波动"把偏离度判据做死（1/0 型退化） */
#define GF_MIN_SIGMA      0.02f

/* 样本不足时不产生调节力（避免开局乱动） */
#define GF_MIN_SAMPLES    20

/* 反退化判据：样本够多 + 波动极小 + 均值偏低 ⇒ 视为"贴下界" */
#define GF_DEGEN_SAMPLES  200
#define GF_DEGEN_MU       0.35f
#define GF_DEGEN_PUSH     0.5f

GenerationFeedback* generation_feedback_create(void) {
    GenerationFeedback* fb = (GenerationFeedback*)calloc(1, sizeof(*fb));
    if (!fb) return NULL;
    fb->enabled = 1;
    return fb;
}

void generation_feedback_destroy(GenerationFeedback* fb) {
    if (fb) free(fb);
}

void generation_feedback_reset(GenerationFeedback* fb) {
    if (!fb) return;
    memset(fb, 0, sizeof(*fb));
    fb->enabled = 1;
}

void generation_feedback_observe(GenerationFeedback* fb, int edges, int words) {
    if (!fb || !fb->enabled) return;

    if (edges < 0) edges = 0;
    if (words < 0) words = 0;
    fb->reach_edges = edges;
    fb->reach_words = words;

    /* ── 反馈：合成「走得多远 + 走得多准」──
     * 旧口径 0.7·e/(e+64) + 0.3·min(w/8,1) 只衡量「走了多少边」、完全不含
     * 「走对没」⇒ A31 探针实测：坏例(1119 边 / 4 词)=0.8121 **反而高于**
     * 好例(20 边 / 10 词)=0.4667 —— **激励方向是反的**；且 500~1119 这段
     * 真实工作区间两者只差 0.004（无鉴别力）。
     * 新口径：以**出口效率**（凑到的词 ÷ 走过的边）为主项、实词量做饱和辅项。
     *   实测单调：坏例 0.339 / 中等 0.459 / 好例 0.799。
     *   副产品：坏例落回 GF_DEGEN_MU(0.35) 之下 ⇒ 反退化防护终于够得着
     *   （那正是它设计出来要救的「恒定低质」场景）。
     * ⚠️ 纯确定性映射：同一 (edges,words) 恒得同一个 reach ⇒ 口径唯一。 */
    float eff  = (float)words / ((float)words + (float)edges / 64.0f + 1.0f); /* 出口效率 ∈ [0,1] */
    float size = (float)words / ((float)words + 4.0f);                        /* 实词量，饱和型 */
    fb->reach = 0.5f * eff + 0.5f * size;

    /* ── 内调节：稳态带只能来自自身历史 ── */
    if (fb->samples == 0) {
        fb->reach_mu    = fb->reach;   /* 首个样本即带的起点 */
        fb->reach_sigma = 0.0f;
        fb->samples     = 1;
        fb->regulation  = 0.0f;
        return;
    }

    const float a   = GF_EMA_ALPHA;
    const float dev = fb->reach - fb->reach_mu;   /* 用更新前的均值算偏差 */

    fb->reach_mu += a * dev;

    /* 滑动方差（EMA）。用 (1-a)*v + a*dev^2 —— dev 取更新前均值，避免自相关偏置。 */
    {
        float v = fb->reach_sigma * fb->reach_sigma;
        v = (1.0f - a) * v + a * dev * dev;
        fb->reach_sigma = sqrtf(v > 0.0f ? v : 0.0f);
    }
    fb->samples++;

    if (fb->samples < GF_MIN_SAMPLES) {   /* 开局不动作 */
        fb->regulation = 0.0f;
        return;
    }

    float sigma = fb->reach_sigma;
    if (sigma < GF_MIN_SIGMA) sigma = GF_MIN_SIGMA;

    float d = (fb->reach - fb->reach_mu) / (GF_BAND_K * sigma);
    if (d >  1.0f) d =  1.0f;
    if (d < -1.0f) d = -1.0f;
    fb->regulation = -d;                  /* 负反馈：高出带 ⇒ 往下压 */

    /* ── 内调节会退化 ──
     * 系统会发现"全兜底最稳"（reach 恒低、波动趋零）⇒ 那时 d 恒 ≈ 0、调节力≈0，
     * 等于把"不动"奖励成了稳态。故：波动极小 + 均值偏低 ⇒ 主动给一个正推力。 */
    if (fb->samples > GF_DEGEN_SAMPLES &&
        fb->reach_sigma < GF_MIN_SIGMA &&
        fb->reach_mu    < GF_DEGEN_MU) {
        fb->regulation = GF_DEGEN_PUSH;
    }
}
