/**
 * @file prefrontal_executive.h
 * @brief 前额叶执行控制器 (v0.3) — 推理编排引擎
 *
 * 大脑类比：
 *   背外侧前额叶（DLPFC）负责维持目标、任务分解、策略切换、
 *   工作记忆暂存和冲突监控。它不是"存储知识"的地方，
 *   而是"管理思考流程"的地方。
 *
 * 系统映射：
 *   1. 意图分析 — 判断问题复杂度，决定走快速路径还是推理路径
 *   2. 任务分解 — 将复杂问题递归拆分为子目标序列
 *   3. 子目标调度 — 逐个求解子目标，注入依赖上下文
 *   4. 冲突检测 — 子目标结果互检，发现矛盾则回溯重试
 *   5. 综合输出 — 将子目标答案组装为最终回复
 *   6. IdeaArena 竞争 — 对多候选想法进行多维度竞争选择
 *
 * 与现有脑区的关系：
 *   通过 Thalamus 信号总线与其他脑区通信。
 *   PFE 不自己走路、不自己评估 — 它通过 thalamus_get_utility("cognitive_ctrl")
 *   复用 CognitiveController 的全部能力。
 *
 * 推理模式：
 *   PFE_MODE_DIRECT    — 简单问题，走现有单次联想路径
 *   PFE_MODE_DECOMPOSE — 复杂问题，查定义→查因果→综合
 *   PFE_MODE_COMPARE   — 比较类，提取属性→对比差异
 *   PFE_MODE_HOWTO     — 步骤类，提取前置条件→生成序列
 *   PFE_MODE_ABDUCE    — 溯因，反向因果推断
 */

#ifndef PREFRONTAL_EXECUTIVE_H
#define PREFRONTAL_EXECUTIVE_H

#include "multi_topology.h"
#include "thalamus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  推理模式
 * ================================================================ */

typedef enum {
    PFE_MODE_DIRECT    = 0,  /* 直接联想（不分解） */
    PFE_MODE_DECOMPOSE = 1,  /* 解释型：定义 + 因果 */
    PFE_MODE_COMPARE   = 2,  /* 比较型：属性提取 + 对比 */
    PFE_MODE_HOWTO     = 3,  /* 步骤型：条件 + 序列 */
    PFE_MODE_ABDUCE    = 4,  /* 溯因型：结果 + 反向因果 */
    PFE_MODE_ANALOGY   = 5,  /* 类比型：结构映射 */
    PFE_MODE_COUNT     = 6
} PFEReasonMode;

/* ================================================================
 *  子目标
 * ================================================================ */

#define PFE_MAX_SUBGOALS     8    /* 单次推理最多子目标 */
#define PFE_MAX_ANSWER_LEN   32   /* 子目标答案节点数上限 */
#define PFE_MAX_ANSWER_TEXT  512  /* 子目标答案文本上限 */

typedef enum {
    PFE_GOAL_PENDING = 0,  /* 等待调度 */
    PFE_GOAL_ACTIVE  = 1,  /* 正在求解 */
    PFE_GOAL_SOLVED  = 2,  /* 成功求解 */
    PFE_GOAL_FAILED  = 3,  /* 求解失败 */
    PFE_GOAL_SKIPPED = 4   /* 因依赖失败跳过 */
} PFESubGoalStatus;

typedef struct {
    char question[512];              /* 子问题文本 */
    int  depends_on;                 /* 依赖的子目标索引 (-1=无依赖) */
    int  topo_mask;                  /* 搜索拓扑位掩码(none=ff) */

    /* 答案 */
    int   answer_nodes[PFE_MAX_ANSWER_LEN];
    int   answer_len;
    float answer_score;              /* 满意度评分 */
    char  answer_text[PFE_MAX_ANSWER_TEXT];

    PFESubGoalStatus status;
    int   retry_count;               /* 本子目标retry次数 */
    int   source_idea_index;         /* 若来自IdeaArena胜出，记录索引 */
} PFESubGoal;

/* ================================================================
 *  推理工作区
 * ================================================================ */

typedef struct {
    PFESubGoal goals[PFE_MAX_SUBGOALS];
    int        goal_count;
    int        active_goal;          /* 当前处理的子目标索引 */

    /* 工作记忆 — 跨子目标共享的激活快照 */
    float working_activation[12][64];  /* [子拓扑ID][隐层] */

    /* 推理参数 */
    PFEReasonMode mode;
    int   max_depth;                 /* 最大分解层数 */

    /* 统计 */
    int conflict_count;
    int backtrack_count;
    int total_retries;
} PFEReasonWorkspace;

/* ================================================================
 *  策略模式统计 — Phase 3 策略权重自学习
 * ================================================================ */

typedef struct {
    int   use_count;            /* 该模式累计使用次数 */
    int   success_count;        /* 成功推理次数（平均满意度 > 阈值） */
    float total_satisfaction;   /* 累计满意度（用于计算 EMA 均值） */
    float avg_satisfaction;     /* EMA 平均满意度 */
    float avg_arena_score;      /* EMA 平均 Arena 胜出分数 */
    float success_rate;         /* EMA 成功率 */
} PFEModeStats;

/* ================================================================
 *  前额叶执行器
 * ================================================================ */

typedef struct PrefrontalExecutive {
    MasterTopology* master;           /* 多拓扑网络 */
    Thalamus*       thalamus;         /* 丘脑信号总线 */

    PFEReasonWorkspace workspace;

    /* 策略权重 — 不同推理模式的历史胜率，用于自适应模式选择 */
    float strategy_weights[PFE_MODE_COUNT];

    /* 按模式统计 — Phase 3 策略权重自学习 */
    PFEModeStats per_mode_stats[PFE_MODE_COUNT];

    /* 运行统计 */
    int   total_reasoning_cycles;
    int   successful_decompositions;
    float avg_satisfaction;
    float avg_arena_winner_score;     /* IdeaArena 平均胜出分数 */

    /* 配置 */
    int   max_decompose_depth;        /* 最大分解深度 (默认 2) */
    float min_subgoal_satisfaction;   /* 子目标最低满意度 (默认 0.5) */
    int   max_subgoal_retries;        /* 每个子目标最大retry次数 (默认 2) */
    float conflict_threshold;         /* 冲突检测阈值 (默认 0.25) */
    float temperature_base;           /* Phase 3: 扩散基础温度 (默认 0.15) */
    float temperature_increment;      /* Phase 3: 每次retry温度增量 (默认 0.12) */

    /* 线程安全 */
    pthread_mutex_t lock;
} PrefrontalExecutive;

/* ================================================================
 *  API
 * ================================================================ */

/* ── 生命周期 ── */

/**
 * 创建前额叶执行器
 * @param master   多拓扑网络
 * @param thalamus 丘脑信号总线
 */
PrefrontalExecutive* pfe_create(MasterTopology* master, Thalamus* thalamus);

void pfe_destroy(PrefrontalExecutive* pfe);

/* ── 核心推理 ── */

/**
 * 执行一次完整的推理会话
 *
 * 自动判断推理模式 → 分解子目标 → 逐个求解 → 冲突检测 → 综合输出
 *
 * @param pfe        前额叶执行器
 * @param question   用户问题
 * @param answer_out 输出缓冲区
 * @param max_len    缓冲区大小
 * @return 0=成功, -1=失败
 */
int pfe_reason(PrefrontalExecutive* pfe,
               const char* question,
               char* answer_out, int max_len);

/* ── 意图分析 ── */

/**
 * 评估问题复杂度
 * @return 0=简单(直接走联想路径), 1=中等(2步分解), 2=复杂(需要多层分解)
 */
int pfe_assess_complexity(PrefrontalExecutive* pfe, const char* question);

/**
 * 确定推理模式
 */
PFEReasonMode pfe_determine_mode(PrefrontalExecutive* pfe, const char* question);

/* ── 任务分解 ── */

/**
 * 将问题分解为子目标序列，填充 workspace.goals
 * @return 子目标数量
 */
int pfe_decompose_question(PrefrontalExecutive* pfe,
                           const char* question,
                           PFEReasonWorkspace* ws);

/* ── 子目标求解 ── */

/**
 * 求解单个子目标（内部调用 CognitiveController 的标准流水线）
 */
int pfe_solve_subgoal(PrefrontalExecutive* pfe, int goal_index);

/**
 * 将已解决的前置子目标上下文注入当前子目标的搜索空间
 */
void pfe_inject_dependency_context(PrefrontalExecutive* pfe, int target_goal);

/* ── 冲突检测 ── */

/**
 * 检查子目标结果之间是否存在矛盾
 * @return 检测到的冲突数
 */
int pfe_detect_conflicts(PrefrontalExecutive* pfe, PFEReasonWorkspace* ws);

/**
 * 尝试解决冲突：标记低置信度子目标，降低其权重后重新求解
 */
int pfe_resolve_conflicts(PrefrontalExecutive* pfe, PFEReasonWorkspace* ws);

/* ── 综合输出 ── */

/**
 * 将所有已解决的子目标答案综合为最终回复
 */
int pfe_synthesize_answer(PrefrontalExecutive* pfe,
                          PFEReasonWorkspace* ws,
                          char* answer_out, int max_len);

/* ── IdeaArena 集成 ── */

/**
 * 对多个候选答案进行多维度竞争，选出最佳
 * @param candidates 候选答案文本数组
 * @param n          候选数量
 * @param winner_out 胜出者索引
 * @return 0=成功, -1=失败/无候选
 */
int pfe_select_best_idea(PrefrontalExecutive* pfe,
                         const char** candidates, int n,
                         int* winner_out);

/* ── 策略权重自学习 (Phase 3) ── */

/**
 * 根据当前推理结果更新策略权重
 * 在 pfe_reason() 内部自动调用，也可外部触发
 * @param pfe  前额叶执行器
 * @param mode 当次使用的推理模式
 * @param reward 奖励信号（0.0 - 1.0，来自子目标平均满意度 / Arena 胜出分数）
 */
void pfe_update_strategy_weights(PrefrontalExecutive* pfe,
                                 PFEReasonMode mode, float reward);

/**
 * 持久化策略权重到文件
 * @return 0=成功, -1=失败
 */
int pfe_save_strategy_weights(PrefrontalExecutive* pfe);

/**
 * 从文件加载策略权重
 * @return 0=成功, -1=文件不存在（使用默认均匀权重）
 */
int pfe_load_strategy_weights(PrefrontalExecutive* pfe);

/**
 * 获取指定模式的统计信息
 */
const PFEModeStats* pfe_get_mode_stats(PrefrontalExecutive* pfe, PFEReasonMode mode);

/* ── 自适应参数调优 (Phase 3) ── */

/**
 * 根据当前模式的统计信息，自适应调整推理参数
 * （min_subgoal_satisfaction / max_subgoal_retries / temperature / conflict_threshold）
 * 在每次推理完成后自动调用。
 */
void pfe_adapt_parameters(PrefrontalExecutive* pfe, PFEReasonMode mode);

/* ── 推理持久化 (Phase 3) ── */

/**
 * 保存当前推理工作区到文件（支持中断后恢复）
 * @param pfe      前额叶执行器
 * @param question 原始问题文本（恢复时需要）
 * @return 0=成功, -1=失败
 */
int pfe_save_workspace(PrefrontalExecutive* pfe, const char* question);

/**
 * 从文件加载推理工作区
 * @return 0=成功, -1=文件不存在/损坏
 */
int pfe_load_workspace(PrefrontalExecutive* pfe, char* question_out, int qmax);

/**
 * 从已保存的工作区恢复推理：继续求解未完成的子目标 → 综合输出
 * @param pfe        前额叶执行器（需已加载工作区或调用 pfe_load_workspace）
 * @param question   原始问题
 * @param answer_out 输出缓冲区
 * @param max_len    缓冲区大小
 * @return 0=成功, -1=失败
 */
int pfe_resume_reason(PrefrontalExecutive* pfe,
                      const char* question,
                      char* answer_out, int max_len);

/* ── 统计 ── */

int pfe_cycle_count(PrefrontalExecutive* pfe);
float pfe_avg_satisfaction(PrefrontalExecutive* pfe);
const char* pfe_mode_name(PFEReasonMode mode);

#ifdef __cplusplus
}
#endif

#endif /* PREFRONTAL_EXECUTIVE_H */
