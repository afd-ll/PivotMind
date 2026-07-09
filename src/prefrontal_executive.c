/**
 * @file prefrontal_executive.c
 * @brief 前额叶执行控制器实现 (v0.3)
 *
 * 推理编排引擎：不自己走路、不自己评估，
 * 通过丘脑总线获取 CognitiveController 并复用其标准流水线。
 */

#include "prefrontal_executive.h"
#include "idea_arena.h"
#include "amygdala.h"
#include "cognitive_controller.h"
#include "huarong_topology.h"
#include "common.h"
#include "error.h"
#include "constants.h"
#include "cingulate.h"   /* 包含 cingulate_diffusion_evaluate */
#include "causal_reasoning.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── Phase 3: 策略权重持久化 ── */
#define PFE_STRATEGY_FILE "pfe_strategy.bin"

/* 权重学习参数 */
#define PFE_WEIGHT_EMA      0.08f  /* 权重 EMA 更新速率（低速率=稳定） */
#define PFE_EPSILON         0.15f  /* epsilon-greedy 探索率 */
#define PFE_SUCCESS_THRESH  0.5f   /* 平均满意度 > 此值视为"成功" */

/* ================================================================
 *  模式名称
 * ================================================================ */

static const char* MODE_NAMES[] = {
    "直接联想", "解释分解", "比较对比",
    "步骤指导", "溯因推断", "类比结构",
};

const char* pfe_mode_name(PFEReasonMode mode) {
    if (mode >= 0 && mode < PFE_MODE_COUNT) return MODE_NAMES[mode];
    return "未知";
}

/* ================================================================
 *  创建 / 销毁
 * ================================================================ */

PrefrontalExecutive* pfe_create(MasterTopology* master, Thalamus* thalamus) {
    if (!master) return NULL;

    PrefrontalExecutive* pfe = (PrefrontalExecutive*)calloc(1, sizeof(PrefrontalExecutive));
    if (!pfe) return NULL;

    pfe->master   = master;
    pfe->thalamus = thalamus;

    /* 默认配置 */
    pfe->max_decompose_depth      = PFE_MAX_DECOMPOSE_DEPTH;
    pfe->min_subgoal_satisfaction = PFE_MIN_SUBGOAL_SATISF;
    pfe->max_subgoal_retries      = PFE_MAX_SUBGOAL_RETRIES;
    pfe->conflict_threshold       = PFE_CONFLICT_THRESH;
    pfe->temperature_base         = 0.15f;   /* Phase 3: 扩散基础温度 */
    pfe->temperature_increment    = 0.12f;   /* Phase 3: 每次retry温度增量 */

    /* 策略权重 — 先设默认均匀，再尝试从磁盘加载 */
    for (int i = 0; i < PFE_MODE_COUNT; i++)
        pfe->strategy_weights[i] = 1.0f / PFE_MODE_COUNT;

    /* 按模式统计初始化 */
    memset(pfe->per_mode_stats, 0, sizeof(pfe->per_mode_stats));

    /* Phase 3: 尝试加载持久化的策略权重 */
    pfe_load_strategy_weights(pfe);

    pthread_mutex_init(&pfe->lock, NULL);

    LOG_INFO("[前额叶执行器] 就绪, decompose_depth=%d", pfe->max_decompose_depth);
    return pfe;
}

void pfe_destroy(PrefrontalExecutive* pfe) {
    if (!pfe) return;
    pthread_mutex_destroy(&pfe->lock);
    free(pfe);
}

/* ================================================================
 *  意图分析 — 复杂度评估
 * ================================================================ */

int pfe_assess_complexity(PrefrontalExecutive* pfe, const char* question) {
    (void)pfe;
    if (!question || !question[0]) return 0;

    int len   = (int)strlen(question);
    int score = 0;

    /* 长度因子：长问题倾向复杂 */
    if (len > 40)  score++;
    if (len > 100) score++;

    /* 关键词触发复杂度 */
    if (strstr(question, "为什么"))  score++;
    if (strstr(question, "原因"))    score++;
    if (strstr(question, "比较"))    score++;
    if (strstr(question, "区别"))    score++;
    if (strstr(question, "怎么"))    score++;
    if (strstr(question, "如何"))    score++;
    if (strstr(question, "如果"))    score++;
    if (strstr(question, "假设"))    score++;
    if (strstr(question, "类比"))    score++;
    if (strstr(question, "类似"))    score++;

    /* 多问句号/逗号暗示多子问题 */
    int qmarks = 0, commas = 0;
    for (int i = 0; question[i]; i++) {
        if (question[i] == '?') qmarks++;
        if (question[i] == ',') commas++;
    }
    if (qmarks >= 2) score++;
    if (commas >= 3) score++;

    /* 复杂度分级 */
    if (score <= 1)  return 0;   /* 简单 → 直接联想 */
    if (score <= 3)  return 1;   /* 中等 → 2步分解 */
    return 2;                     /* 复杂 → 多层分解 */
}

/* ================================================================
 *  模式确定
 * ================================================================ */

PFEReasonMode pfe_determine_mode(PrefrontalExecutive* pfe, const char* question) {
    if (!question) return PFE_MODE_DIRECT;

    /* ── 关键词明确命中 → 直接确定模式 ── */
    if (strstr(question, "为什么") || strstr(question, "原因"))
        return PFE_MODE_DECOMPOSE;

    if (strstr(question, "比较") || strstr(question, "区别") ||
        strstr(question, "不同") || strstr(question, "相同"))
        return PFE_MODE_COMPARE;

    if (strstr(question, "怎么") || strstr(question, "如何"))
        return PFE_MODE_HOWTO;

    if (strstr(question, "如果") || strstr(question, "假设"))
        return PFE_MODE_ABDUCE;

    if (strstr(question, "类比") || strstr(question, "类似"))
        return PFE_MODE_ANALOGY;

    /* ── Phase 3: 无关键词命中 → epsilon-greedy 权重选择 ── */
    if (!pfe) return PFE_MODE_DIRECT;

    /* epsilon 探索：随机选一个模式尝试 */
    if ((float)rand() / (float)RAND_MAX < PFE_EPSILON) {
        return (PFEReasonMode)(rand() % PFE_MODE_COUNT);
    }

    /* 贪心：选权重最高的模式 */
    int   best_mode  = PFE_MODE_DIRECT;
    float best_weight = pfe->strategy_weights[0];
    for (int i = 1; i < PFE_MODE_COUNT; i++) {
        if (pfe->strategy_weights[i] > best_weight) {
            best_weight = pfe->strategy_weights[i];
            best_mode   = i;
        }
    }
    return (PFEReasonMode)best_mode;
}

/* ================================================================
 *  任务分解
 * ================================================================ */

static void ws_add_subgoal(PFEReasonWorkspace* ws, const char* question,
                           int depends, int topo_mask) {
    if (ws->goal_count >= PFE_MAX_SUBGOALS) return;
    PFESubGoal* g = &ws->goals[ws->goal_count];
    snprintf(g->question, sizeof(g->question), "%s", question);
    g->depends_on   = depends;
    g->topo_mask    = topo_mask;
    g->status       = PFE_GOAL_PENDING;
    g->answer_len   = 0;
    g->answer_score = 0.0f;
    g->retry_count  = 0;
    ws->goal_count++;
}

/* 依据关键词从问题中提取主体词汇 */
static void extract_subject(const char* question, char* out, int max_len) {
    if (!question || !out) return;
    out[0] = '\0';

    /* 简单启发式：取"为什么"之后、"是"之前的内容 */
    const char* start = NULL;
    const char* end   = NULL;

    /* 找 "为什么" */
    const char* why = strstr(question, "为什么");
    if (why) start = why + strlen("为什么");
    else {
        /* 找 "怎么" */
        const char* how = strstr(question, "怎么");
        if (how) start = how + strlen("怎么");
        else start = question;  /* 直接用全问题 */
    }

    /* 找结束标记 */
    const char* markers[] = {"是", "的", "不", "会", "能", "可以", "？", "?", "吗"};
    end = start + strlen(start);
    for (int i = 0; i < 9; i++) {
        const char* pos = strstr(start, markers[i]);
        if (pos && pos < end) end = pos;
    }

    int copy_len = (int)(end - start);
    if (copy_len <= 0) {
        /* Fallback: 复制前32个字符 */
        copy_len = (int)strlen(start) < 32 ? (int)strlen(start) : 32;
    }
    if (copy_len > max_len - 1) copy_len = max_len - 1;
    if (copy_len > 0) {
        memcpy(out, start, copy_len);
        out[copy_len] = '\0';

        /* 去除首尾空格 */
        char* p = out;
        while (*p == ' ' || *p == '\t') p++;
        if (p != out) memmove(out, p, strlen(p) + 1);
        for (int i = (int)strlen(out) - 1; i >= 0; i--) {
            if (out[i] == ' ' || out[i] == '\t') out[i] = '\0';
            else break;
        }
    }
}

int pfe_decompose_question(PrefrontalExecutive* pfe,
                           const char* question,
                           PFEReasonWorkspace* ws) {
    if (!question || !ws) return 0;

    memset(ws, 0, sizeof(PFEReasonWorkspace));
    ws->mode  = pfe_determine_mode(pfe, question);
    ws->active_goal = 0;

    /* 提取主体关键词 */
    char subject[128] = {0};
    extract_subject(question, subject, sizeof(subject));

    /* 默认拓扑位掩码 */
    const int ALL_TOPO = 0x7FF;  /* 词汇+语义+情绪+语法+上下文+领域+语用+文化+概念+模板 */

    switch (ws->mode) {

    case PFE_MODE_DECOMPOSE: {
        /* "为什么 X 会 Y？"
         * goal[0]: X 的定义/相关概念
         * goal[1]: Y 的定义/相关概念
         * goal[2]: X与Y的因果链（依赖0,1） */
        char q1[256], q2[256];
        snprintf(q1, sizeof(q1), "什么是%s？", subject[0] ? subject : question);
        ws_add_subgoal(ws, q1, -1, ALL_TOPO);

        snprintf(q2, sizeof(q2), "%s的关键特征和属性", subject[0] ? subject : question);
        ws_add_subgoal(ws, q2, -1, ALL_TOPO);

        /* 因果链依赖前两个 */
        if (ws->goal_count < PFE_MAX_SUBGOALS)
            ws_add_subgoal(ws, question, 0, ALL_TOPO);
        break;
    }

    case PFE_MODE_COMPARE: {
        /* "比较 A 和 B" → 拆成 A的属性 + B的属性 + 对比 */
        ws_add_subgoal(ws, "提取第一个比较对象的属性和特征", -1, ALL_TOPO);
        ws_add_subgoal(ws, "提取第二个比较对象的属性和特征", -1, ALL_TOPO);
        if (ws->goal_count < PFE_MAX_SUBGOALS)
            ws_add_subgoal(ws, "对比两个对象的异同点", 0, ALL_TOPO);
        break;
    }

    case PFE_MODE_HOWTO: {
        /* "怎么做 X" → 前置条件 + 步骤序列 */
        char q1[256], q2[256];
        snprintf(q1, sizeof(q1), "%s的前置条件和所需资源", subject[0] ? subject : question);
        ws_add_subgoal(ws, q1, -1, ALL_TOPO);
        snprintf(q2, sizeof(q2), "%s的操作步骤和方法", subject[0] ? subject : question);
        ws_add_subgoal(ws, q2, 0, ALL_TOPO);
        break;
    }

    case PFE_MODE_ABDUCE: {
        /* 溯因：假设后会发生什么 → 基线状态 + 连锁反应 */
        ws_add_subgoal(ws, "确定当前情况的基线状态", -1, ALL_TOPO);
        ws_add_subgoal(ws, question, 0, ALL_TOPO);
        break;
    }

    case PFE_MODE_ANALOGY: {
        /* 类比：A像B → A的结构特征 + B的结构特征 + 映射 */
        ws_add_subgoal(ws, "提取源对象的深层结构特征", -1, ALL_TOPO);
        if (ws->goal_count < PFE_MAX_SUBGOALS)
            ws_add_subgoal(ws, "提取目标对象的结构特征", -1, ALL_TOPO);
        if (ws->goal_count < PFE_MAX_SUBGOALS)
            ws_add_subgoal(ws, "建立结构映射和对等关系", 0, ALL_TOPO);
        break;
    }

    case PFE_MODE_DIRECT:
    default: {
        /* 直接路径：整个问题作为一个目标 */
        ws_add_subgoal(ws, question, -1, ALL_TOPO);
        break;
    }
    }

    return ws->goal_count;
}

/* ================================================================
 *  Phase 3: 递归分解
 * ================================================================ */

/**
 * 将子问题分解为子目标并追加到工作区，与父目标建立依赖链。
 *
 * 与 pfe_decompose_question() 的区别：
 *   本函数不清空 workspace，而是在已有 goals 数组尾部追加，
 *   新子目标的首个依赖 parent_index。
 *
 * @param pfe          前额叶执行器
 * @param ws           已有子目标的工作区
 * @param question     待分解的子问题文本
 * @param parent_index 父目标在 ws->goals 中的索引
 * @return 新增的子目标数量
 */
static int pfe_decompose_into(PrefrontalExecutive* pfe,
                              PFEReasonWorkspace* ws,
                              const char* question,
                              int parent_index) {
    if (!question || !ws) return 0;
    if (ws->goal_count >= PFE_MAX_SUBGOALS) return 0;

    int base_count = ws->goal_count;
    const int ALL_TOPO = 0x7FF;

    PFEReasonMode mode = pfe_determine_mode(pfe, question);
    char subject[128] = {0};
    extract_subject(question, subject, sizeof(subject));

    /* 最小子目标数：必须有足够的 slots */
    int slots = PFE_MAX_SUBGOALS - base_count;
    if (slots < 1) return 0;

    switch (mode) {

    case PFE_MODE_DECOMPOSE: {
        /* 拆成：定义 + 特征 + 因果链 */
        if (base_count < PFE_MAX_SUBGOALS) {
            char q[256];
            snprintf(q, sizeof(q), "什么是%.100s？", subject[0] ? subject : question);
            ws_add_subgoal(ws, q, parent_index, ALL_TOPO); /* 依赖父目标 */
        }
        if (base_count + 1 < PFE_MAX_SUBGOALS) {
            char q[256];
            snprintf(q, sizeof(q), "%.100s的关键特征", subject[0] ? subject : question);
            ws_add_subgoal(ws, q, parent_index, ALL_TOPO);
        }
        if (base_count + 2 < PFE_MAX_SUBGOALS && slots >= 3) {
            ws_add_subgoal(ws, question, base_count, ALL_TOPO); /* 依赖第一个子子目标 */
        }
        break;
    }

    case PFE_MODE_COMPARE: {
        if (base_count < PFE_MAX_SUBGOALS)
            ws_add_subgoal(ws, "提取第一个对象属性", parent_index, ALL_TOPO);
        if (base_count + 1 < PFE_MAX_SUBGOALS)
            ws_add_subgoal(ws, "提取第二个对象属性", parent_index, ALL_TOPO);
        if (base_count + 2 < PFE_MAX_SUBGOALS && slots >= 3)
            ws_add_subgoal(ws, "对比异同点", base_count, ALL_TOPO);
        break;
    }

    case PFE_MODE_HOWTO: {
        if (base_count < PFE_MAX_SUBGOALS) {
            char q[256];
            snprintf(q, sizeof(q), "%.100s的前置条件", subject[0] ? subject : question);
            ws_add_subgoal(ws, q, parent_index, ALL_TOPO);
        }
        if (base_count + 1 < PFE_MAX_SUBGOALS && slots >= 2) {
            char q[256];
            snprintf(q, sizeof(q), "%.100s的操作步骤", subject[0] ? subject : question);
            ws_add_subgoal(ws, q, base_count, ALL_TOPO);
        }
        break;
    }

    case PFE_MODE_ABDUCE: {
        if (base_count < PFE_MAX_SUBGOALS)
            ws_add_subgoal(ws, "确定基线状态", parent_index, ALL_TOPO);
        if (base_count + 1 < PFE_MAX_SUBGOALS && slots >= 2)
            ws_add_subgoal(ws, question, base_count, ALL_TOPO);
        break;
    }

    case PFE_MODE_ANALOGY: {
        if (base_count < PFE_MAX_SUBGOALS)
            ws_add_subgoal(ws, "提取源对象结构", parent_index, ALL_TOPO);
        if (base_count + 1 < PFE_MAX_SUBGOALS && slots >= 2)
            ws_add_subgoal(ws, "提取目标对象结构", parent_index, ALL_TOPO);
        if (base_count + 2 < PFE_MAX_SUBGOALS && slots >= 3)
            ws_add_subgoal(ws, "结构映射", base_count, ALL_TOPO);
        break;
    }

    default:
        /* DIRECT: 不分解 */
        return 0;
    }

    return ws->goal_count - base_count;
}

/**
 * 递归扫描子目标并展开复杂项。
 *
 * 扫描逻辑：
 *   - 对每个子目标评估复杂度
 *   - complexity > 0 且 depth < max_decompose_depth → 递归展开
 *   - 子子目标追加到 workspace 尾部，依赖链指向父目标
 *   - 父目标本身保留（作为"问题陈述"先求解，提供上下文）
 */
static void pfe_expand_recursive(PrefrontalExecutive* pfe,
                                  PFEReasonWorkspace* ws,
                                  int depth) {
    if (!pfe || !ws) return;
    if (depth >= pfe->max_decompose_depth) return;

    /* 先记住当前 goal_count，循环只到当前边界；
     * 新增的子目标在下一轮 depth+1 递归中展开 */
    int current_count = ws->goal_count;
    int any_expanded  = 0;

    for (int i = 0; i < current_count; i++) {
        if (ws->goal_count >= PFE_MAX_SUBGOALS - 2) break;

        PFESubGoal* g = &ws->goals[i];
        if (g->status != PFE_GOAL_PENDING) continue;

        /* 复杂度评估：中等及以上才递归 */
        int complexity = pfe_assess_complexity(pfe, g->question);
        if (complexity <= 0) continue;

        /* 尝试展开这个子目标 */
        int added = pfe_decompose_into(pfe, ws, g->question, i);
        if (added > 0) any_expanded = 1;
    }

    /* 如果有新子目标产生，继续递归展开下一层 */
    if (any_expanded && depth + 1 < pfe->max_decompose_depth)
        pfe_expand_recursive(pfe, ws, depth + 1);
}

/* ================================================================
 *  子目标求解 — Phase 2: 真实 diffusion + ACC 评估管线
 * ================================================================ */

/**
 * 将依赖目标答案的特征向量注入到工作记忆 `working_activation` 中，
 * 供后续子目标的扩散走边作为偏好偏置。
 */
static void pfe_store_dependency_features(PrefrontalExecutive* pfe, int dep_index) {
    PFEReasonWorkspace* ws = &pfe->workspace;
    PFESubGoal* dep = &ws->goals[dep_index];

    if (dep->status != PFE_GOAL_SOLVED || dep->answer_len == 0) return;

    /* 从依赖答案节点中提取特征向量并 EMA 写入 working_activation */
    for (int n = 0; n < dep->answer_len; n++) {
        int node_id = dep->answer_nodes[n];
        if (node_id < 0) continue;

        /* 尝试从语义拓扑获取该节点的特征向量 */
        SubTopology* sem = master_get_sub_topology_by_type(pfe->master, TOPO_SEMANTIC);
        if (!sem || !sem->net || node_id >= sem->net->node_count) continue;

        ReasoningNode* node = sem->net->nodes[node_id];
        if (!node) continue;

        /* 将该节点特征 EMA 叠加到工作记忆中 */
        int topo_idx = TOPO_SEMANTIC;  /* 主存储拓扑 */
        if (topo_idx >= 0 && topo_idx < 12) {
            int dim = node->feature_dim > 0 ? node->feature_dim : NODE_FEATURE_DIM;
            for (int d = 0; d < 64 && d < dim; d++) {
                float feat = (node->features && d < dim)
                    ? node->features[d] : 0.0f;
                ws->working_activation[topo_idx][d] =
                    ws->working_activation[topo_idx][d] * 0.7f + feat * 0.3f;
            }
        }
    }
}

/**
 * 将 working_activation 中的偏好偏置应用到语义拓扑节点激活上，
 * 为当前子目标的扩散走边提供引导。
 */
static void pfe_apply_working_bias(PrefrontalExecutive* pfe, int goal_index) {
    PFEReasonWorkspace* ws = &pfe->workspace;
    PFESubGoal* g = &ws->goals[goal_index];

    /* 仅当有前置依赖时才引导 */
    if (g->depends_on < 0) return;

    int topo_idx = TOPO_SEMANTIC;
    SubTopology* sem = master_get_sub_topology_by_type(pfe->master, TOPO_SEMANTIC);
    if (!sem || !sem->net) return;

    float* bias = ws->working_activation[topo_idx];

    /* 对语义拓扑节点应用余弦相似度偏置 */
    for (int i = 0; i < sem->net->node_count; i++) {
        ReasoningNode* node = sem->net->nodes[i];
        if (!node || !node->features) continue;

        int dim = node->feature_dim > 0 ? node->feature_dim : NODE_FEATURE_DIM;

        /* 计算特征余弦相似度 */
        float dot = 0.0f, norm_n = 0.0f, norm_b = 0.0f;
        for (int d = 0; d < 64 && d < dim; d++) {
            dot   += node->features[d] * bias[d];
            norm_n += node->features[d] * node->features[d];
            norm_b += bias[d] * bias[d];
        }
        float similarity = (norm_n > 1e-8f && norm_b > 1e-8f)
            ? dot / (sqrtf(norm_n) * sqrtf(norm_b)) : 0.0f;

        /* 相似度 > 0.3 → 小幅提升激活（引导而非强控） */
        if (similarity > 0.3f) {
            float boost = (similarity - 0.3f) * 0.25f;
            node->activation += boost;
            if (node->activation > 1.0f) node->activation = 1.0f;
        }
    }
}

int pfe_solve_subgoal(PrefrontalExecutive* pfe, int goal_index) {
    PFEReasonWorkspace* ws = &pfe->workspace;
    if (goal_index < 0 || goal_index >= ws->goal_count) return -1;

    PFESubGoal* g = &ws->goals[goal_index];
    g->status = PFE_GOAL_ACTIVE;

    /* 获取 CognitiveController（通过丘脑） */
    CognitiveController* cc = NULL;
    if (pfe->thalamus)
        cc = (CognitiveController*)thalamus_get_utility(pfe->thalamus, THAL_UTIL_COGNITIVE_CTRL);
    if (!cc) {
        LOG_WARNING("[PFE] 无 CognitiveController，无法求解子目标%d", goal_index);
        g->status = PFE_GOAL_FAILED;
        return -1;
    }

    /* 注入前置依赖上下文 */
    if (g->depends_on >= 0 && g->depends_on < goal_index) {
        pfe_inject_dependency_context(pfe, goal_index);
        /* Phase 2: 将依赖答案的特征向量存入工作记忆 */
        pfe_store_dependency_features(pfe, g->depends_on);
        /* Phase 2: 应用工作记忆偏置到拓扑层 */
        pfe_apply_working_bias(pfe, goal_index);
    }

    /* 设置子问题为当前上下文 */
    cognitive_controller_set_context(cc, g->question, NULL);
    float ctx_activations[MAX_SUBTOPOS] = {0};
    calc_context_activations(cc, ctx_activations);
    compute_intent(cc, ctx_activations);

    /* ── DECOMPOSE 模式：因果子目标优先走因果联想搜索 ── */
    {
        PFEReasonWorkspace* ws = &pfe->workspace;
        if (ws->mode == PFE_MODE_DECOMPOSE &&
            (strstr(g->question, "为什么") || strstr(g->question, "原因"))) {

            int causal_count = 0;
            CausalSearchResult* results = causal_associative_search(
                pfe->master, g->question, /*max_hops=*/5, /*max_results=*/3, &causal_count);

            if (results && causal_count > 0 && results[0].total_strength > 0.25f) {
                /* 重建因果图以解析节点名 */
                CausalGraph* cg = infer_causal_graph_from_master_topology(pfe->master, 0.2f);
                SubTopology* vocab = master_get_sub_topology_by_type(pfe->master, TOPO_VOCABULARY);

                int pos = 0;
                pos += snprintf(g->answer_text + pos,
                    sizeof(g->answer_text) - (size_t)pos,
                    "因果链分析：");

                for (int r = 0; r < causal_count && pos < (int)sizeof(g->answer_text) - 60; r++) {
                    CausalPath* path = results[r].path;
                    if (!path || path->length == 0) continue;

                    pos += snprintf(g->answer_text + pos,
                        sizeof(g->answer_text) - (size_t)pos,
                        "%s[路径%u 强度%.2f] ", r > 0 ? "；" : "", r + 1, path->total_strength);

                    for (int n = 0; n < path->length && pos < (int)sizeof(g->answer_text) - 20; n++) {
                        int cg_id = path->node_ids[n];
                        const char* name = "?";
                        if (cg && cg_id >= 0 && cg_id < cg->node_count) {
                            int topo_id = cg->node_mapping[cg_id];
                            if (vocab && vocab->net && topo_id >= 0 &&
                                topo_id < vocab->net->node_count &&
                                vocab->net->nodes[topo_id] &&
                                vocab->net->nodes[topo_id]->concept) {
                                name = vocab->net->nodes[topo_id]->concept;
                            }
                        }
                        pos += snprintf(g->answer_text + pos,
                            sizeof(g->answer_text) - (size_t)pos,
                            "%s%s", name, n < path->length - 1 ? "→" : "");
                    }
                }

                if (cg) causal_graph_destroy(cg);

                g->answer_score = 0.55f + results[0].total_strength * 0.35f;
                if (g->answer_score > 0.95f) g->answer_score = 0.95f;
                g->status = PFE_GOAL_SOLVED;
                g->answer_len = (causal_count > 0) ? 1 : 0;

                causal_search_results_free(results, causal_count);
                LOG_INFO("[PFE] 因果搜索求解子目标%d 成功 (%d条路径, 最高强度=%.2f)",
                         goal_index, causal_count, results[0].total_strength);
                return 0;
            }

            /* 因果搜索无结果 → 回退到扩散引擎 */
            if (results) causal_search_results_free(results, causal_count);
        }
    }

    /* ── Phase 2: 真实扩散走边 + ACC 评估 ── */
    const int MAX_RETRIES = pfe->max_subgoal_retries;
    float best_satisfaction    = 0.0f;
    int   best_node_ids[PFE_MAX_ANSWER_LEN];
    int   best_node_len        = 0;
    char  best_text[PFE_MAX_ANSWER_TEXT];
    best_text[0] = '\0';

    for (int retry = 0; retry <= MAX_RETRIES; retry++) {
        /* 逐次提升温度，探索不同候选 — Phase 3: 使用自适应温度 */
        float temperature = pfe->temperature_base + (float)retry * pfe->temperature_increment;

        GeneratedSequence seq = {0};
        int n = cingulate_diffusion_evaluate(pfe->master, g->question, temperature,
                                               NULL,  /* PFE 无 CognitiveController */
                                               &seq);

        if (n < 2) continue;

        float satisfaction = seq.total_score;
        if (satisfaction > best_satisfaction) {
            best_satisfaction = satisfaction;

            /* 拼合输出文本 */
            int pos = 0;
            for (int i = 0; i < seq.count && pos < (int)sizeof(best_text) - 10; i++)
                pos += snprintf(best_text + pos, sizeof(best_text) - (size_t)pos,
                                "%s", seq.words[i]);

            /* 从词汇拓扑查找节点 ID */
            SubTopology* vocab = master_get_sub_topology_by_type(pfe->master, TOPO_VOCABULARY);
            best_node_len = 0;
            for (int i = 0; i < seq.count && best_node_len < PFE_MAX_ANSWER_LEN; i++) {
                if (vocab && vocab->net) {
                    int nid = huarong_net_find_concept(vocab->net, seq.words[i]);
                    if (nid >= 0)
                        best_node_ids[best_node_len++] = nid;
                }
            }
        }

        if (best_satisfaction >= pfe->min_subgoal_satisfaction)
            break;

        g->retry_count++;
    }

    /* 存储结果 */
    g->answer_score = best_satisfaction;

    if (best_satisfaction >= pfe->min_subgoal_satisfaction && best_node_len > 0) {
        g->status = PFE_GOAL_SOLVED;
        snprintf(g->answer_text, sizeof(g->answer_text), "%s",
                 best_text[0] ? best_text : g->question);
        if (best_node_len <= PFE_MAX_ANSWER_LEN) {
            memcpy(g->answer_nodes, best_node_ids, (size_t)best_node_len * sizeof(int));
            g->answer_len = best_node_len;
        }
    } else if (best_satisfaction > 0.2f && best_text[0]) {
        /* 部分成功：有生成但评分偏低 */
        g->status    = PFE_GOAL_SOLVED;
        g->answer_score = best_satisfaction * 0.85f;  /* 弱答案折扣 */
        snprintf(g->answer_text, sizeof(g->answer_text), "%s", best_text);
        if (best_node_len <= PFE_MAX_ANSWER_LEN) {
            memcpy(g->answer_nodes, best_node_ids, (size_t)best_node_len * sizeof(int));
            g->answer_len = best_node_len;
        }
    } else {
        g->status = PFE_GOAL_FAILED;
        snprintf(g->answer_text, sizeof(g->answer_text),
                 "[无法充分求解] %s (最佳评分=%.2f)", g->question, best_satisfaction);
    }

    return g->status == PFE_GOAL_SOLVED ? 0 : -1;
}

void pfe_inject_dependency_context(PrefrontalExecutive* pfe, int target_goal) {
    PFEReasonWorkspace* ws = &pfe->workspace;
    PFESubGoal* tgt = &ws->goals[target_goal];

    if (tgt->depends_on < 0) return;

    PFESubGoal* dep = &ws->goals[tgt->depends_on];
    if (dep->status != PFE_GOAL_SOLVED) return;

    /* 将依赖目标的答案文本令牌化后注入语义拓扑 */
    CognitiveController* cc = NULL;
    if (pfe->thalamus)
        cc = (CognitiveController*)thalamus_get_utility(pfe->thalamus, THAL_UTIL_COGNITIVE_CTRL);
    if (!cc) return;

    /* 简单实现：用依赖目标的答案作为额外上下文 */
    char enhanced_question[512];
    snprintf(enhanced_question, sizeof(enhanced_question),
             "%s (已知: %.200s)", tgt->question, dep->answer_text);
    cognitive_controller_set_context(cc, enhanced_question, NULL);
}

/* ================================================================
 *  冲突检测
 * ================================================================ */

int pfe_detect_conflicts(PrefrontalExecutive* pfe, PFEReasonWorkspace* ws) {
    if (!pfe || !ws) return 0;

    int conflicts = 0;

    /* 检查成对子目标结果是否存在矛盾 */
    for (int i = 0; i < ws->goal_count; i++) {
        if (ws->goals[i].status != PFE_GOAL_SOLVED) continue;

        for (int j = i + 1; j < ws->goal_count; j++) {
            if (ws->goals[j].status != PFE_GOAL_SOLVED) continue;

            PFESubGoal* a = &ws->goals[i];
            PFESubGoal* b = &ws->goals[j];

            /* 方法：比较两个答案文本的语义重叠度。
             * 差异过大 + 高分数 → 可能存在矛盾 */
            float score_gap = fabsf(a->answer_score - b->answer_score);

            /* 简单文本重叠度 */
            int overlap = 0;
            for (int ai = 0; a->answer_text[ai]; ai++) {
                for (int bj = 0; b->answer_text[bj]; bj++) {
                    if (a->answer_text[ai] == b->answer_text[bj]) {
                        overlap++;
                        break;
                    }
                }
            }

            int max_len = (int)strlen(a->answer_text) > (int)strlen(b->answer_text)
                ? (int)strlen(a->answer_text) : (int)strlen(b->answer_text);
            float text_overlap = max_len > 0 ? (float)overlap / (float)max_len : 0.0f;

            /* 重叠度极低 + 分数差距大 → 标记为冲突 */
            if (text_overlap < 0.15f && score_gap > pfe->conflict_threshold) {
                conflicts++;

                /* 标记低分者为待回溯 */
                if (b->answer_score < a->answer_score) {
                    b->answer_score -= 0.1f;
                } else {
                    a->answer_score -= 0.1f;
                }
            }
        }
    }

    return conflicts;
}

int pfe_resolve_conflicts(PrefrontalExecutive* pfe, PFEReasonWorkspace* ws) {
    int resolved = 0;

    for (int i = 0; i < ws->goal_count; i++) {
        if (ws->goals[i].status == PFE_GOAL_SOLVED &&
            ws->goals[i].answer_score < pfe->min_subgoal_satisfaction - 0.05f) {

            /* 标记低分目标为待重试 */
            ws->goals[i].status = PFE_GOAL_PENDING;
            resolved++;
        }
    }

    if (resolved == 0 && ws->goal_count > 1) {
        /* 没找到可重试的 → 降低全局满意度 */
        for (int i = 0; i < ws->goal_count; i++)
            ws->goals[i].answer_score *= 0.8f;
    }

    return resolved;
}

/* ================================================================
 *  Phase 3: 子目标批量求解（pfe_reason / pfe_resume_reason 共用）
 * ================================================================ */

/**
 * 求解工作区中所有待处理的子目标。
 * 已求解/已跳过的不重复处理。
 * @return 已求解数量
 */
static int pfe_solve_all_subgoals(PrefrontalExecutive* pfe,
                                   PFEReasonWorkspace* ws) {
    int n_goals = ws->goal_count;
    int solved  = 0;

    for (int i = 0; i < n_goals; i++) {
        PFESubGoal* g = &ws->goals[i];

        /* 已处理过的跳过（恢复场景） */
        if (g->status == PFE_GOAL_SOLVED) {
            solved++;
            continue;
        }
        if (g->status == PFE_GOAL_SKIPPED || g->status == PFE_GOAL_FAILED)
            continue;

        /* 依赖目标失败或跳过 → 传播失败 */
        if (g->depends_on >= 0) {
            PFESubGoal* dep = &ws->goals[g->depends_on];
            if (dep->status == PFE_GOAL_FAILED || dep->status == PFE_GOAL_SKIPPED) {
                g->status = PFE_GOAL_SKIPPED;
                continue;
            }
        }

        /* 求解当前子目标 */
        ws->active_goal = i;
        int result = pfe_solve_subgoal(pfe, i);
        if (result == 0) solved++;
    }

    return solved;
}

/* ================================================================
 *  推理后处理 — 消除 pfe_reason/pfe_resume_reason 的 ~80 行重复
 *  包含：Arena竞争 → 综合输出 → 统计更新 → 策略权重 → 结束信号
 * ================================================================ */
static void pfe_post_process(PrefrontalExecutive* pfe,
                              PFEReasonWorkspace* ws,
                              const char* question,
                              char* answer_out, int max_len) {
    int n_goals = ws->goal_count;

    /* 阶段4.5：IdeaArena 多候选竞争 */
    {
        int arena_solved_count = 0;
        for (int i = 0; i < n_goals; i++)
            if (ws->goals[i].status == PFE_GOAL_SOLVED) arena_solved_count++;

        if (arena_solved_count > 1) {
            IdeaArena* arena = NULL;
            if (pfe->thalamus)
                arena = (IdeaArena*)thalamus_get_utility(pfe->thalamus, THAL_UTIL_IDEA_ARENA);
            if (arena) {
                CognitiveState* cstate = NULL;
                Amygdala* amy = (Amygdala*)thalamus_get_region(pfe->thalamus, THAL_AMYGDALA);
                if (amy) cstate = amy->cognitive_state;

                arena_set_context(arena, pfe->master, cstate, question);
                arena_clear(arena);

                int arena_added = 0;
                for (int i = 0; i < n_goals && arena_added < ARENA_MAX_CANDIDATES; i++) {
                    if (ws->goals[i].status != PFE_GOAL_SOLVED) continue;
                    if (ws->goals[i].answer_len == 0) continue;

                    arena_add_candidate(arena,
                        ws->goals[i].answer_nodes,
                        ws->goals[i].answer_len,
                        TOPO_SEMANTIC,
                        0,
                        ws->goals[i].answer_text);
                    arena_added++;
                }

                if (arena_added > 0) {
                    arena_score_all(arena);
                    int winner = arena_compete(arena);
                    if (winner >= 0) {
                        pfe->avg_arena_winner_score =
                            pfe->avg_arena_winner_score * 0.9f
                            + arena->winner_score * 0.1f;
                        ws->goals[winner].source_idea_index = winner;

                        if (pfe->thalamus) {
                            BrainSignal sig;
                            memset(&sig, 0, sizeof(sig));
                            sig.type   = THAL_SIG_IDEA_SELECTED;
                            sig.source = THAL_PREF_EXEC;
                            sig.target = -1;
                            thalamus_send_signal(pfe->thalamus, -1, &sig);
                        }
                        arena_feedback_to_master(arena, pfe->master);
                    }
                }
            }
        }
    }

    /* 阶段5：综合输出 */
    pfe_synthesize_answer(pfe, ws, answer_out, max_len);

    /* 更新统计 */
    pfe->total_reasoning_cycles++;
    if (n_goals > 1) pfe->successful_decompositions++;

    float total_sat = 0.0f;
    int solved = 0;
    for (int i = 0; i < n_goals; i++) {
        if (ws->goals[i].status == PFE_GOAL_SOLVED) {
            total_sat += ws->goals[i].answer_score;
            solved++;
        }
    }
    if (solved > 0) {
        float avg = total_sat / solved;
        pfe->avg_satisfaction = pfe->avg_satisfaction * 0.9f + avg * 0.1f;
    }

    /* Phase 3: 策略权重更新 + 自适应调参 */
    {
        PFEReasonMode mode = ws->mode;
        float reward = (n_goals > 0) ? total_sat / (float)n_goals : 0.3f;
        if (pfe->avg_arena_winner_score > 0.0f)
            reward = reward * 0.6f + pfe->avg_arena_winner_score * 0.4f;
        pfe_update_strategy_weights(pfe, mode, reward);
        pfe_adapt_parameters(pfe, mode);
    }

    /* 发送推理结束信号 */
    if (pfe->thalamus) {
        BrainSignal sig;
        memset(&sig, 0, sizeof(sig));
        sig.type   = THAL_SIG_REASONING_END;
        sig.source = THAL_PREF_EXEC;
        sig.target = -1;
        thalamus_send_signal(pfe->thalamus, -1, &sig);
    }
}

/* ================================================================
 *  综合输出 — Phase 2: 可解释推理链
 * ================================================================ */

int pfe_synthesize_answer(PrefrontalExecutive* pfe,
                          PFEReasonWorkspace* ws,
                          char* answer_out, int max_len) {
    if (!answer_out || max_len <= 1) return -1;

    (void)pfe;

    int solved = 0;
    for (int i = 0; i < ws->goal_count; i++) {
        if (ws->goals[i].status == PFE_GOAL_SOLVED) solved++;
    }

    /* 全部失败 → 退化输出 */
    if (solved == 0) {
        snprintf(answer_out, max_len,
                 "这个问题涉及%d个子问题，但我目前的理解还不够深入。可以换个角度问吗？",
                 ws->goal_count);
        return 0;
    }

    int pos = 0;

    /* 单子目标（DIRECT 模式）：直接输出答案 */
    if (ws->goal_count == 1 && solved == 1) {
        PFESubGoal* g = &ws->goals[0];
        if (g->answer_score >= 0.4f) {
            /* 高置信度直接答案 */
            snprintf(answer_out, max_len, "%s", g->answer_text);
        } else {
            /* 低置信度带提示 */
            snprintf(answer_out, max_len, "%s (对该回答的把握较低)",
                     g->answer_text);
        }
        return 0;
    }

    /* 多子目标：输出可解释推理链 */
    /* 推理链头部 */
    pos += snprintf(answer_out + pos, (size_t)(max_len - pos),
                    "为了回答这个问题，我分步进行了思考：\n");

    /* 逐个输出已解决的子目标及其答案 */
    for (int i = 0; i < ws->goal_count && pos < max_len - 1; i++) {
        PFESubGoal* g = &ws->goals[i];

        if (g->status == PFE_GOAL_SOLVED) {
            /* 格式化子目标：编号 + 问题 + 答案 + 置信度 */
            pos += snprintf(answer_out + pos, (size_t)(max_len - pos),
                            "%d. %s → %s (置信度: %.0f%%)\n",
                            i + 1, g->question,
                            g->answer_text,
                            (double)(g->answer_score * 100.0f));
        } else if (g->status == PFE_GOAL_FAILED) {
            pos += snprintf(answer_out + pos, (size_t)(max_len - pos),
                            "%d. %s → [未能充分求解]\n",
                            i + 1, g->question);
        } else if (g->status == PFE_GOAL_SKIPPED) {
            pos += snprintf(answer_out + pos, (size_t)(max_len - pos),
                            "%d. %s → [跳过：依赖目标未解决]\n",
                            i + 1, g->question);
        }

        if (pos >= max_len - 1) break;
    }

    /* 综合结论 */
    if (pos < max_len - 50) {
        pos += snprintf(answer_out + pos, (size_t)(max_len - pos),
                        "\n综合以上分析：");
        /* 拼接最后一个已解子目标的答案作为结论 */
        for (int i = ws->goal_count - 1; i >= 0; i--) {
            if (ws->goals[i].status == PFE_GOAL_SOLVED) {
                pos += snprintf(answer_out + pos, (size_t)(max_len - pos),
                                "%s", ws->goals[i].answer_text);
                break;
            }
        }
    }

    /* 统计尾部 */
    if (pos < max_len - 30) {
        pos += snprintf(answer_out + pos, (size_t)(max_len - pos),
                        "\n\n（推理模式: %s，共%d子问题，已解决%d个）",
                        pfe_mode_name(ws->mode), ws->goal_count, solved);
    }

    return 0;
}

/* ================================================================
 *  主推理入口
 * ================================================================ */

int pfe_reason(PrefrontalExecutive* pfe,
               const char* question,
               char* answer_out, int max_len) {
    if (!pfe || !question || !answer_out) return -1;

    pthread_mutex_lock(&pfe->lock);

    /* 发送推理开始信号 */
    if (pfe->thalamus) {
        BrainSignal sig;
        memset(&sig, 0, sizeof(sig));
        sig.type   = THAL_SIG_REASONING_START;
        sig.source = THAL_PREF_EXEC;
        sig.target = -1;
        thalamus_send_signal(pfe->thalamus, -1, &sig);
    }

    /* 阶段1：复杂度评估 */
    int complexity = pfe_assess_complexity(pfe, question);
    PFEReasonWorkspace* ws = &pfe->workspace;

    /* 简单问题直接走快速路径 */
    if (complexity == 0) {
        CognitiveController* cc = NULL;
        if (pfe->thalamus)
            cc = (CognitiveController*)thalamus_get_utility(pfe->thalamus, THAL_UTIL_COGNITIVE_CTRL);

        if (cc) {
            /* 简单应答 — 无需走完整意图计算 */
            snprintf(answer_out, max_len, "关于\"%s\"，这是一个很好的问题。", question);
        } else {
            snprintf(answer_out, max_len, "认知调度尚未就绪。");
        }

        pfe->total_reasoning_cycles++;
        /* Phase 3: 快速路径视作 DIRECT 模式，中等满意度 */
        pfe_update_strategy_weights(pfe, PFE_MODE_DIRECT, 0.5f);
        pthread_mutex_unlock(&pfe->lock);
        return 0;
    }

    /* 阶段2：任务分解 */
    int n_goals = pfe_decompose_question(pfe, question, ws);

    /* ── Phase 3: 递归展开 — 对复杂子目标进一步拆解 ── */
    if (n_goals > 0 && pfe->max_decompose_depth > 1)
        pfe_expand_recursive(pfe, ws, 0);
    n_goals = ws->goal_count;  /* 递归展开后可能有新增 */

    /* 阶段3：逐子目标求解 */
    pfe_solve_all_subgoals(pfe, ws);

    /* 阶段4：冲突检测 */
    int conflicts = pfe_detect_conflicts(pfe, ws);
    if (conflicts > 0) {
        pfe_resolve_conflicts(pfe, ws);

        /* 重试被重新标记的子目标 */
        for (int i = 0; i < n_goals; i++) {
            if (ws->goals[i].status == PFE_GOAL_PENDING) {
                pfe_solve_subgoal(pfe, i);
            }
        }

        /* 再做一次冲突检测 */
        pfe_detect_conflicts(pfe, ws);
    }

    /* 阶段4.5~5：Arena竞争 + 综合输出 + 统计更新 + 策略权重 + 结束信号 */
    pfe_post_process(pfe, ws, question, answer_out, max_len);

    pthread_mutex_unlock(&pfe->lock);
    return 0;
}

/* ================================================================
 *  IdeaArena 集成
 * ================================================================ */

int pfe_select_best_idea(PrefrontalExecutive* pfe,
                         const char** candidates, int n,
                         int* winner_out) {
    if (!pfe || !candidates || n <= 0 || !winner_out) return -1;

    IdeaArena* arena = NULL;
    if (pfe->thalamus)
        arena = (IdeaArena*)thalamus_get_utility(pfe->thalamus, THAL_UTIL_IDEA_ARENA);

    if (!arena) {
        /* Fallback: 简单地取第一个 */
        *winner_out = 0;
        return 0;
    }

    /* 获取认知状态用于杏仁核调制 */
    CognitiveState* cstate = NULL;
    Amygdala* amy = (Amygdala*)thalamus_get_region(pfe->thalamus, THAL_AMYGDALA);
    if (amy) cstate = amy->cognitive_state;

    arena_set_context(arena, pfe->master, cstate, NULL);
    arena_clear(arena);

    /* 简易添加 — 完整实现在 Phase 2 */
    for (int i = 0; i < n && i < ARENA_MAX_CANDIDATES; i++) {
        int tmp_ids[4] = {0};
        arena_add_candidate(arena, tmp_ids, 1, TOPO_SEMANTIC, 0, candidates[i]);
    }

    arena_score_all(arena);
    int winner = arena_compete(arena);

    if (winner >= 0) {
        *winner_out = winner;
        pfe->avg_arena_winner_score = pfe->avg_arena_winner_score * 0.9f
            + arena->winner_score * 0.1f;
        return 0;
    }

    *winner_out = 0;
    return -1;
}

/* ================================================================
 *  统计
 * ================================================================ */

int pfe_cycle_count(PrefrontalExecutive* pfe) {
    return pfe ? pfe->total_reasoning_cycles : 0;
}

float pfe_avg_satisfaction(PrefrontalExecutive* pfe) {
    return pfe ? pfe->avg_satisfaction : 0.0f;
}

/* ================================================================
 *  Phase 3: 策略权重自学习
 * ================================================================ */

void pfe_update_strategy_weights(PrefrontalExecutive* pfe,
                                 PFEReasonMode mode, float reward) {
    if (!pfe || mode < 0 || mode >= PFE_MODE_COUNT) return;

    /* 钳制奖励信号到 [0, 1] */
    if (reward < 0.0f) reward = 0.0f;
    if (reward > 1.0f) reward = 1.0f;

    PFEModeStats* s = &pfe->per_mode_stats[mode];

    /* 更新按模式统计 */
    s->use_count++;
    s->total_satisfaction += reward;

    /* EMA 平均满意度（首次直接设值） */
    if (s->use_count == 1) {
        s->avg_satisfaction = reward;
        s->avg_arena_score  = pfe->avg_arena_winner_score;
    } else {
        s->avg_satisfaction = s->avg_satisfaction * (1.0f - PFE_WEIGHT_EMA)
                              + reward * PFE_WEIGHT_EMA;
        s->avg_arena_score  = s->avg_arena_score * (1.0f - PFE_WEIGHT_EMA)
                              + pfe->avg_arena_winner_score * PFE_WEIGHT_EMA;
    }

    /* 成功率：奖励 > 阈值视为成功 */
    float is_success = (reward >= PFE_SUCCESS_THRESH) ? 1.0f : 0.0f;
    if (s->use_count == 1) {
        s->success_rate = is_success;
    } else {
        s->success_rate  = s->success_rate * (1.0f - PFE_WEIGHT_EMA)
                           + is_success * PFE_WEIGHT_EMA;
    }
    if (is_success > 0.5f) s->success_count++;

    /* ── 策略权重更新：EMA 向奖励方向移动 ── */
    float old_weight = pfe->strategy_weights[mode];

    /* 核心公式：权重 = 旧权重 * (1 - lr) + 奖励 * lr
     * 奖励高 → 权重上升；奖励低 → 权重下降 */
    pfe->strategy_weights[mode] =
        old_weight * (1.0f - PFE_WEIGHT_EMA) + reward * PFE_WEIGHT_EMA;

    /* 重新归一化所有策略权重，保证和为 1.0 */
    float total = 0.0f;
    for (int i = 0; i < PFE_MODE_COUNT; i++)
        total += pfe->strategy_weights[i];

    if (total > 1e-8f) {
        float inv = 1.0f / total;
        for (int i = 0; i < PFE_MODE_COUNT; i++)
            pfe->strategy_weights[i] *= inv;
    } else {
        /* 退化情况：重置为均匀 */
        for (int i = 0; i < PFE_MODE_COUNT; i++)
            pfe->strategy_weights[i] = 1.0f / PFE_MODE_COUNT;
    }

    /* 持久化 */
    pfe_save_strategy_weights(pfe);
}

int pfe_save_strategy_weights(PrefrontalExecutive* pfe) {
    if (!pfe) return -1;
    FILE* f = fopen(PFE_STRATEGY_FILE, "wb");
    if (!f) return -1;

    /* 写入权重数组 */
    if (fwrite(pfe->strategy_weights, sizeof(float), PFE_MODE_COUNT, f)
        != PFE_MODE_COUNT) {
        fclose(f);
        return -1;
    }

    /* 写入统计（用于诊断，非必须） */
    fwrite(pfe->per_mode_stats, sizeof(PFEModeStats), PFE_MODE_COUNT, f);

    fclose(f);
    return 0;
}

int pfe_load_strategy_weights(PrefrontalExecutive* pfe) {
    if (!pfe) return -1;
    FILE* f = fopen(PFE_STRATEGY_FILE, "rb");
    if (!f) return -1;  /* 文件不存在 → 保持默认均匀权重 */

    if (fread(pfe->strategy_weights, sizeof(float), PFE_MODE_COUNT, f)
        != PFE_MODE_COUNT) {
        fclose(f);
        return -1;
    }

    /* 归一化检查：防止文件损坏导致权重异常 */
    float total = 0.0f;
    for (int i = 0; i < PFE_MODE_COUNT; i++) {
        if (pfe->strategy_weights[i] < 0.0f || pfe->strategy_weights[i] > 1.0f) {
            /* 损坏值 → 重置为均匀 */
            for (int j = 0; j < PFE_MODE_COUNT; j++)
                pfe->strategy_weights[j] = 1.0f / PFE_MODE_COUNT;
            fclose(f);
            return -1;
        }
        total += pfe->strategy_weights[i];
    }

    if (total > 1e-8f) {
        float inv = 1.0f / total;
        for (int i = 0; i < PFE_MODE_COUNT; i++)
            pfe->strategy_weights[i] *= inv;
    }

    /* 尝试读取统计 */
    (void)!fread(pfe->per_mode_stats, sizeof(PFEModeStats), PFE_MODE_COUNT, f);

    fclose(f);
    return 0;
}

const PFEModeStats* pfe_get_mode_stats(PrefrontalExecutive* pfe,
                                        PFEReasonMode mode) {
    if (!pfe || mode < 0 || mode >= PFE_MODE_COUNT) return NULL;
    return &pfe->per_mode_stats[mode];
}

/* ================================================================
 *  Phase 3: 自适应参数调优
 * ================================================================ */

#define PFE_ADAPT_EMA       0.04f   /* 参数自适应 EMA 速率（很慢=平滑） */
#define PFE_ADAPT_MIN_SAMPLES 3     /* 最少样本数才触发调参 */

void pfe_adapt_parameters(PrefrontalExecutive* pfe, PFEReasonMode mode) {
    if (!pfe || mode < 0 || mode >= PFE_MODE_COUNT) return;

    PFEModeStats* s = &pfe->per_mode_stats[mode];
    if (s->use_count < PFE_ADAPT_MIN_SAMPLES) return;  /* 数据不足 */

    float sr = s->success_rate;  /* 该模式 EMA 成功率 ∈ [0, 1] */

    /* ── 1. min_subgoal_satisfaction ──
     * 成功率高 → 提高门槛（更严格，追求更高质量）
     * 成功率低 → 降低门槛（更宽容，允许更多答案通过）
     * 范围: [0.30, 0.70], 默认 0.50 */
    {
        float target = 0.50f + (sr - 0.50f) * 0.40f;
        if (target < 0.30f) target = 0.30f;
        if (target > 0.70f) target = 0.70f;
        pfe->min_subgoal_satisfaction =
            pfe->min_subgoal_satisfaction * (1.0f - PFE_ADAPT_EMA)
            + target * PFE_ADAPT_EMA;
    }

    /* ── 2. max_subgoal_retries ──
     * 成功率高 → 减少重试（高效）
     * 成功率低 → 增加重试（多探索）
     * 范围: [1, 5], 默认 2 */
    {
        float target_f = 2.5f - sr * 1.5f;
        int   target   = (int)(target_f + 0.5f);
        if (target < 1) target = 1;
        if (target > 5) target = 5;

        /* 整数参数用 EMA 平滑：缓慢趋近目标 */
        float current_f = (float)pfe->max_subgoal_retries;
        float smoothed  = current_f * (1.0f - PFE_ADAPT_EMA)
                          + (float)target * PFE_ADAPT_EMA;
        pfe->max_subgoal_retries = (int)(smoothed + 0.5f);
        if (pfe->max_subgoal_retries < 1) pfe->max_subgoal_retries = 1;
    }

    /* ── 3. temperature_base ──
     * 成功率高 → 降温（精准搜索）
     * 成功率低 → 升温（扩大探索）
     * 范围: [0.08, 0.25], 默认 0.15 */
    {
        float target = 0.25f - sr * 0.17f;
        if (target < 0.08f) target = 0.08f;
        if (target > 0.25f) target = 0.25f;
        pfe->temperature_base =
            pfe->temperature_base * (1.0f - PFE_ADAPT_EMA)
            + target * PFE_ADAPT_EMA;
    }

    /* ── 4. temperature_increment ──
     * 成功率高 → 小步升温（保守）
     * 成功率低 → 大步升温（激进探索）
     * 范围: [0.05, 0.18], 默认 0.12 */
    {
        float target = 0.18f - sr * 0.13f;
        if (target < 0.05f) target = 0.05f;
        if (target > 0.18f) target = 0.18f;
        pfe->temperature_increment =
            pfe->temperature_increment * (1.0f - PFE_ADAPT_EMA)
            + target * PFE_ADAPT_EMA;
    }

    /* ── 5. conflict_threshold ──
     * 成功率高 → 放宽冲突检测（信任答案）
     * 成功率低 → 收紧冲突检测（更敏感）
     * 范围: [0.15, 0.40], 默认 0.25 */
    {
        float target = 0.15f + sr * 0.25f;
        if (target < 0.15f) target = 0.15f;
        if (target > 0.40f) target = 0.40f;
        pfe->conflict_threshold =
            pfe->conflict_threshold * (1.0f - PFE_ADAPT_EMA)
            + target * PFE_ADAPT_EMA;
    }
}

/* ================================================================
 *  Phase 3: 推理工作区持久化 — 支持中断后恢复
 * ================================================================ */

#define PFE_WORKSPACE_FILE "pfe_workspace.bin"
#define PFE_WORKSPACE_MAGIC "PFEWS01"   /* 文件头魔数 */

int pfe_save_workspace(PrefrontalExecutive* pfe, const char* question) {
    if (!pfe) return -1;
    PFEReasonWorkspace* ws = &pfe->workspace;

    FILE* f = fopen(PFE_WORKSPACE_FILE, "wb");
    if (!f) return -1;

    /* 魔数 */
    if (fwrite(PFE_WORKSPACE_MAGIC, 1, 8, f) != 8) { fclose(f); return -1; }

    /* workspace 元数据 */
    if (fwrite(&ws->goal_count, sizeof(int), 1, f) != 1) { fclose(f); return -1; }
    if (fwrite(&ws->active_goal, sizeof(int), 1, f) != 1) { fclose(f); return -1; }
    {
        int mode_int = (int)ws->mode;
        if (fwrite(&mode_int, sizeof(int), 1, f) != 1) { fclose(f); return -1; }
    }
    if (fwrite(&ws->max_depth, sizeof(int), 1, f) != 1) { fclose(f); return -1; }
    if (fwrite(&ws->conflict_count, sizeof(int), 1, f) != 1) { fclose(f); return -1; }
    if (fwrite(&ws->backtrack_count, sizeof(int), 1, f) != 1) { fclose(f); return -1; }
    if (fwrite(&ws->total_retries, sizeof(int), 1, f) != 1) { fclose(f); return -1; }

    /* 工作记忆 */
    if (fwrite(ws->working_activation, sizeof(float), 12 * 64, f)
        != (size_t)(12 * 64)) { fclose(f); return -1; }

    /* 子目标数组 */
    if (fwrite(ws->goals, sizeof(PFESubGoal), PFE_MAX_SUBGOALS, f)
        != PFE_MAX_SUBGOALS) { fclose(f); return -1; }

    /* 原始问题（用于恢复时上下文） */
    {
        char qbuf[512] = {0};
        if (question) {
            snprintf(qbuf, sizeof(qbuf), "%.511s", question);
        }
        if (fwrite(qbuf, 1, sizeof(qbuf), f) != sizeof(qbuf)) { fclose(f); return -1; }
    }

    fclose(f);
    return 0;
}

int pfe_load_workspace(PrefrontalExecutive* pfe, char* question_out, int qmax) {
    if (!pfe) return -1;

    FILE* f = fopen(PFE_WORKSPACE_FILE, "rb");
    if (!f) return -1;

    char magic[9] = {0};
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, PFE_WORKSPACE_MAGIC, 8) != 0) {
        fclose(f); return -1;
    }

    PFEReasonWorkspace* ws = &pfe->workspace;

    /* 元数据 */
    if (fread(&ws->goal_count, sizeof(int), 1, f) != 1) { fclose(f); return -1; }
    if (fread(&ws->active_goal, sizeof(int), 1, f) != 1) { fclose(f); return -1; }
    {
        int mode_int = 0;
        if (fread(&mode_int, sizeof(int), 1, f) != 1) { fclose(f); return -1; }
        ws->mode = (PFEReasonMode)mode_int;
    }
    if (fread(&ws->max_depth, sizeof(int), 1, f) != 1) { fclose(f); return -1; }
    if (fread(&ws->conflict_count, sizeof(int), 1, f) != 1) { fclose(f); return -1; }
    if (fread(&ws->backtrack_count, sizeof(int), 1, f) != 1) { fclose(f); return -1; }
    if (fread(&ws->total_retries, sizeof(int), 1, f) != 1) { fclose(f); return -1; }

    /* 工作记忆 */
    if (fread(ws->working_activation, sizeof(float), 12 * 64, f)
        != (size_t)(12 * 64)) { fclose(f); return -1; }

    /* 子目标数组 */
    if (fread(ws->goals, sizeof(PFESubGoal), PFE_MAX_SUBGOALS, f)
        != PFE_MAX_SUBGOALS) { fclose(f); return -1; }

    /* 原始问题 */
    {
        char qbuf[512] = {0};
        if (fread(qbuf, 1, sizeof(qbuf), f) != sizeof(qbuf)) { fclose(f); return -1; }
        if (question_out && qmax > 0) {
            snprintf(question_out, (size_t)qmax, "%.511s", qbuf);
        }
    }

    fclose(f);

    /* 校验：goal_count 合理性 */
    if (ws->goal_count < 0 || ws->goal_count > PFE_MAX_SUBGOALS) {
        memset(ws, 0, sizeof(PFEReasonWorkspace));
        return -1;
    }

    return 0;
}

int pfe_resume_reason(PrefrontalExecutive* pfe,
                      const char* question,
                      char* answer_out, int max_len) {
    if (!pfe || !answer_out || max_len <= 1) return -1;

    pthread_mutex_lock(&pfe->lock);

    PFEReasonWorkspace* ws = &pfe->workspace;
    if (ws->goal_count <= 0) {
        /* 工作区为空 → 从头开始推理 */
        pthread_mutex_unlock(&pfe->lock);
        return pfe_reason(pfe, question, answer_out, max_len);
    }

    /* 发送恢复信号 */
    if (pfe->thalamus) {
        BrainSignal sig;
        memset(&sig, 0, sizeof(sig));
        sig.type   = THAL_SIG_REASONING_START;
        sig.source = THAL_PREF_EXEC;
        sig.target = -1;
        thalamus_send_signal(pfe->thalamus, -1, &sig);
    }

    int n_goals = ws->goal_count;

    /* 阶段3续：继续求解剩余子目标 */
    pfe_solve_all_subgoals(pfe, ws);

    /* 阶段4：冲突检测 */
    int conflicts = pfe_detect_conflicts(pfe, ws);
    if (conflicts > 0) {
        pfe_resolve_conflicts(pfe, ws);

        for (int i = 0; i < n_goals; i++) {
            if (ws->goals[i].status == PFE_GOAL_PENDING)
                pfe_solve_subgoal(pfe, i);
        }

        pfe_detect_conflicts(pfe, ws);
    }

    /* 阶段4.5~5：Arena竞争 + 综合输出 + 统计更新 + 策略权重 + 结束信号 */
    pfe_post_process(pfe, ws, question, answer_out, max_len);

    pthread_mutex_unlock(&pfe->lock);
    return 0;
}
