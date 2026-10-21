#ifndef TRANSITION_MODEL_H
#define TRANSITION_MODEL_H

/**
 * @file transition_model.h
 * @brief [PROB-ASSIST v0.6.11] 词转移概率模型 —— 走边生成的统计辅助层
 *
 * 动机（拍板 2026-10-06）：纯图走边（diffusion_generate → broca 包装）产出
 * 的是「词碎片的乱拼装」，调度器的竞争机制（revise_and_retry /
 * RETRY_FROM_POOL）名存实亡（该分支原本空转 continue）。本模块提供
 * 基于喂料语料统计的 bigram 条件概率 P(next|prev)，用于：
 *   1. 在 RETRY_FROM_POOL 时生成候选词序列（走边的竞争者，真竞争）；
 *   2. 给候选打分（几何平均条件概率），与 evaluate_draft 的内感受分融合。
 *
 * 设计要点：
 *   - 键 = vocab 节点 node_id 对（(uint64)prev<<32 | (uint32)next），零词表对齐成本；
 *   - 计数挂在既有喂料路径 _learn_tokens 的连边点上（用户输入与 AI 回复连续
 *     喂料 ⇒ 表内天然含「输入尾词 → 回复首词」的对话转移，正是沟通所需）；
 *   - add-λ 平滑，采样/打分均无需归一化常数；
 *   - 独立持久化 transitions.bin（v11 状态格式不动），可随时整体回退。
 *
 * 开关：环境变量 PIVOTMIND_PROB_ASSIST=0 关闭（tm_global() 返回 NULL，
 * 调用点判空即可，零侵入）；默认开启。
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TransitionModel TransitionModel;

/* ==================== 生命周期 ==================== */

TransitionModel* tm_create(void);
void tm_destroy(TransitionModel* tm);

/* ==================== 观测（线程安全，learn worker 与生成并发安全） ==================== */

/** 单对计数 +1。prev==next 或任一 <0 时跳过（与 huarong_net_add_connection 的
 *  连边判据一致：自环不连边也不计数）。 */
int tm_observe_pair(TransitionModel* tm, int32_t prev_id, int32_t next_id);

/** 序列批量计数：对 ids[0..n-2]→ids[1..n-1] 逐对调用 tm_observe_pair。 */
int tm_observe_run(TransitionModel* tm, const int32_t* ids, int n);

/* ==================== 查询 ==================== */

/** add-λ 平滑的 P(next|prev) = (c+λ)/(out(prev)+λ·K)。
 *  K 为常数 64（假设每词的候选延续池约 64 个）——概率不必严格归一，
 *  采样在表内重新归一，打分只做相对比较。无该 prev 记录返回 0。 */
float tm_prob(const TransitionModel* tm, int32_t prev_id, int32_t next_id);

/** prev 的出边计数（Σ next c(prev,next)）。 */
uint32_t tm_out_count(const TransitionModel* tm, int32_t prev_id);

/** 按平滑分布采样下一个词；prev 无记录返回 -1。
 *  rng_state 可为 NULL（内部回退到 rand()）；非 NULL 时由调用方持有状态。 */
int32_t tm_sample_next(const TransitionModel* tm, int32_t prev_id, uint32_t* rng_state);

/** 贪心 top-k（按 c+λ 降序），返回实际个数（≤k）。无记录返回 0。 */
int tm_top_next(const TransitionModel* tm, int32_t prev_id, int32_t* out_ids, int k);

/** 候选链打分：Σ log P(ids[i]|ids[i-1])，i=1..n-1。n<2 返回 0。 */
float tm_chain_logprob(const TransitionModel* tm, const int32_t* ids, int n);

/* ==================== 持久化（magic+version+扁平键值对） ==================== */

int tm_save(const TransitionModel* tm, const char* path);
int tm_load(TransitionModel* tm, const char* path);

/* ==================== 统计 ==================== */

int tm_pair_count(const TransitionModel* tm);

/* ==================== 全局单例（g_perception 模式） ==================== */

/** 懒创建并加载（pm_file(PM_FILE_TRANSITIONS)）。
 *  PIVOTMIND_PROB_ASSIST=0 时返回 NULL（整体关闭，调用点判空）。 */
TransitionModel* tm_global(void);

/** 显式判定开关（默认开）。 */
int tm_global_enabled(void);

/** 网关关闭时存盘。 */
void tm_global_shutdown_save(void);

#ifdef __cplusplus
}
#endif

#endif /* TRANSITION_MODEL_H */
