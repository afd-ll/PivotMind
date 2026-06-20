/**
 * @file idea_arena.h
 * @brief 想法竞争竞技场 (v0.3) — 多候选想法的多维度竞争选择引擎
 *
 * 生物学原型：
 *   层次1 — 皮层侧抑制（相似想法互相抑制）
 *   层次2 — 基底节行动选择（多候选抢执行权，多巴胺调制）
 *   层次3 — 全局工作空间（最强候选获胜，进入意识/输出）
 *
 * 竞争机制：
 *   每个候选想法在五维空间中占一个位置：
 *     维度0: 目标契合度 (Goal Fit)        — 这条路径回答了问题吗？
 *     维度1: 知识一致性 (Consistency)      — 和已知知识矛盾吗？
 *     维度2: 新颖/洞察 (Novelty)          — 是老生常谈还是有新东西？
 *     维度3: 情绪效价 (Valence)           — 正面还是负面？
 *     维度4: 可组合性 (Composability)     — 能和其他候选拼接吗？
 *
 *   竞争不是简单加权求和，而是：
 *     1. 硬门槛淘汰：任一维度不达标直接出局
 *     2. 侧抑制竞争：相似候选互相削弱，强者压制弱者
 *     3. 多巴胺调制：杏仁核探索/利用平衡动态调制新颖性权重
 *     4. 胜者回流：胜出路径反馈到学习系统
 *
 * 与杏仁核的关系：
 *   杏仁核维护的 explore_rate 控制竞技场的新颖性奖励强度。
 *   体验差 → explore_rate 高 → 偏好新颖候选
 *   体验好 → explore_rate 低 → 偏好连贯候选
 */

#ifndef IDEA_ARENA_H
#define IDEA_ARENA_H

#include "multi_topology.h"
#include "cognitive_params.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  竞争维度
 * ================================================================ */

#define ARENA_DIM_GOAL_FIT      0  /* 目标契合度 */
#define ARENA_DIM_CONSISTENCY   1  /* 知识一致性 */
#define ARENA_DIM_NOVELTY       2  /* 新颖性 */
#define ARENA_DIM_VALENCE       3  /* 情绪效价 */
#define ARENA_DIM_COMPOSABILITY 4  /* 可组合性 */
#define ARENA_DIM_COUNT         5

/* ================================================================
 *  候选想法
 * ================================================================ */

#define ARENA_MAX_EVIDENCE  16  /* 每条证据链最多证据节点数 */
#define ARENA_MAX_PATH      32  /* 候选路径最大节点数 */

/**
 * 证据链 — 解释"为什么这么评分"
 */
typedef struct {
    int   evidence_nodes[ARENA_MAX_EVIDENCE];
    int   evidence_count;
    float evidence_strength;      /* 证据链总体强度 */
    char  reason[128];            /* 自然语言理由 */
} ArenaEvidence;

/**
 * 一个候选想法
 */
typedef struct {
    int    node_ids[ARENA_MAX_PATH];  /* 路径节点ID */
    int    path_len;                  /* 路径长度 */
    int    topo_id;                   /* 来源拓扑 */
    int    source;                    /* 来源: 0=子目标, 1=beam, 2=策略X */

    /* 五维评分 */
    float  scores[ARENA_DIM_COUNT];
    float  confidence;               /* 自身置信度 */

    /* 证据链 — 每个维度一条 */
    ArenaEvidence justification[ARENA_DIM_COUNT];

    /* 竞争状态（运行时填充） */
    float  activation;               /* 当前竞争激活值 (0.0~1.0) */
    int    suppressed_by;            /* 被哪个候选抑制 (-1=无) */
    int    round;                    /* 进入竞技场的轮次 */

    /* 摘要文本 */
    char   summary[256];             /* 想法摘要（用于日志） */
} CandidateIdea;

/* ================================================================
 *  竞技场
 * ================================================================ */

#define ARENA_MAX_CANDIDATES  12      /* 单轮最多候选数 */
#define ARENA_MAX_ROUNDS       4      /* 竞争收敛最大轮数 */

/**
 * 想法竞争竞技场
 */
typedef struct IdeaArena {
    CandidateIdea candidates[ARENA_MAX_CANDIDATES];
    int candidate_count;

    /* 竞争参数 */
    float lateral_inhibition;    /* 侧抑制强度 (默认 0.6) */
    float novelty_boost;         /* 新颖性奖励 (杏仁核调制, 默认 0.2) */
    float convergence_threshold; /* 收敛阈值: 最高/次高差距 (默认 0.3) */

    /* 硬门槛 */
    float hard_thresholds[ARENA_DIM_COUNT];  /* 各维度最低门槛 */

    /* 竞争结果 */
    int   winner_index;
    float winner_score;
    char  winner_reason[512];    /* 选它的理由 */

    /* 外部注入 */
    MasterTopology* master;      /* 评分需要访问拓扑 */
    CognitiveState*  cog_state;  /* 杏仁核explore_rate，情绪调制 */
    const char*      question;   /* 当前问题（用于goal_fit评分） */

    /* 统计 */
    int   total_rounds;
    int   total_ideas;
    int   total_wins;

    /* 线程安全 */
    pthread_mutex_t lock;
} IdeaArena;

/* ================================================================
 *  API
 * ================================================================ */

/* ── 生命周期 ── */

/**
 * 创建竞技场
 */
IdeaArena* arena_create(void);

void arena_destroy(IdeaArena* arena);

/* ── 上下文设置 ── */

/**
 * 设置外部上下文（在评分前调用）
 */
void arena_set_context(IdeaArena* arena, MasterTopology* master,
                       CognitiveState* cog_state, const char* question);

/* ── 候选管理 ── */

/**
 * 添加一个候选想法
 * @return 候选人索引, -1=满
 */
int arena_add_candidate(IdeaArena* arena,
                        const int* node_ids, int path_len,
                        int topo_id, int source,
                        const char* summary);

/**
 * 清空所有候选（新轮次开始前调用）
 */
void arena_clear(IdeaArena* arena);

/* ── 五维评分 ── */

/**
 * 对所有候选的五个维度逐一评分
 * 评分依赖 master/cog_state/question 上下文
 */
void arena_score_all(IdeaArena* arena);

/**
 * 对单个候选评分（五个维度全部计算）
 */
void arena_score_candidate(IdeaArena* arena, CandidateIdea* idea);

/* 各维度独立评分函数（供外部调用） */
float arena_score_goal_fit(const CandidateIdea* idea,
                           MasterTopology* master,
                           const char* question);

float arena_score_consistency(const CandidateIdea* idea,
                              MasterTopology* master);

float arena_score_novelty(const CandidateIdea* idea,
                          MasterTopology* master);

float arena_score_valence(const CandidateIdea* idea,
                          CognitiveState* cog_state);

float arena_score_composability(const CandidateIdea* idea,
                                IdeaArena* arena);

/* ── 竞争核心 ── */

/**
 * 执行完整竞争流程：硬门槛淘汰 → 侧抑制 → 收敛选优
 * 完成后 arena->winner_index / winner_score / winner_reason 被填充
 *
 * @return 胜出候选索引, -1=无合格候选
 */
int arena_compete(IdeaArena* arena);

/**
 * 执行单轮竞争（用于观察渐进收敛过程）
 * @return 1=已收敛, 0=需要更多轮, -1=无候选
 */
int arena_compete_round(IdeaArena* arena);

/**
 * 选当前最高激活的候选（不做侧抑制）
 */
int arena_pick_winner(IdeaArena* arena);

/* ── 侧抑制 ── */

/**
 * 计算两个候选的想法重叠度
 * @return 0.0(=完全不同) ~ 1.0(=完全相同)
 */
float idea_overlap(const CandidateIdea* a, const CandidateIdea* b);

/**
 * 计算候选的综合评分（五维加权求和）
 */
float idea_composite_score(const CandidateIdea* idea);

/* ── 杏仁核调制 ── */

/**
 * 根据认知状态的 explore_rate 计算 novel 奖励强度
 */
float arena_compute_novelty_boost(CognitiveState* state);

/**
 * 根据认知状态计算侧抑制强度
 * 利用模式 → 高抑制（需连贯） / 探索模式 → 低抑制（允许冒险）
 */
float arena_compute_inhibition(CognitiveState* state);

/* ── 胜者回流 ── */

/**
 * 将胜出结果反馈到节点/边：
 *   胜者路径节点 → confidence 上升
 *   被抑制候选 → 路径节点 selection_count 略降
 *   硬门槛淘汰 → 矛盾边 motivational_bias 降低
 */
int arena_feedback_to_master(IdeaArena* arena, MasterTopology* master);

/* ── 调试 ── */

void arena_print_state(IdeaArena* arena, FILE* out);
const char* arena_dimension_name(int dim);

#ifdef __cplusplus
}
#endif

#endif /* IDEA_ARENA_H */
