/**
 * @file diffusion.h
 * @brief 多层扩散引擎 — 具身认知的语料生成核心
 *
 * 大脑类比：语义理解是跨皮层区域同步扩散激活。
 * 一个词触发的不只是它的邻居词，还有它的语义概念、情绪色调、句法模板。
 *
 * 算法：
 *   输入 → 词汇节点激活
 *        → cross_edge 跨到语义/情绪/模板层
 *        → 各层内部扩散1-2步
 *        → 所有路径回归词汇层加权投票
 *        → 侧抑制去重
 *        → 模板约束排序
 *        → 输出序列
 */

#ifndef DIFFUSION_H
#define DIFFUSION_H

#include "multi_topology.h"
#include "huarong_topology.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DIFF_MAX_CANDIDATES  256
#define DIFF_MAX_PATH_DEPTH  5
#define DIFF_MAX_SEQUENCE     32

/* 前向声明 — 避免引入 emergent_pos.h 循环依赖 */
typedef struct EmergentPOS EmergentPOS;

/** 单个候选词的多维评分 */
typedef struct {
    int    node_id;
    int    topo_id;          /* 来源拓扑 */
    float  vocab_score;       /* 词汇层得分 */
    float  semantic_score;    /* 语义层得分 */
    float  template_score;    /* 模板层得分 */
    float  emotion_score;     /* 情绪层得分 */
    float  total_score;       /* 加权综合 */
    int    used;              /* 已选标记（侧抑制） */
    const char* word;
} DiffusionCandidate;

/** 扩散上下文 */
typedef struct {
    MasterTopology* master;
    int             depth;            /* 扩散深度(默认3) */
    int             top_k;            /* 每步保留候选数(默认8) */
    int             output_len;       /* 序列长度上限(默认20) */
    float           decay;            /* 每步衰减(默认0.7) */
    SubTopology*    vocab;
    SubTopology*    semantic;
    SubTopology*    template;
    SubTopology*    emotion;
    SubTopology*    concept;          /* 概念拓扑（词层）— 词锚定 */

    /* 预分配评分数组 (避免每次calloc/free造成的堆碎片) */
    float*  _vocab_scores;
    float*  _sem_scores;
    float*  _tpl_scores;
    float*  _emo_scores;
    int     _vocab_cap;       /* 词汇层容量 */
    int     _sem_cap;         /* 语义层容量 */
    int     _tpl_cap;         /* 模板层容量 */
    int     _emo_cap;         /* 情绪层容量 */

    /* 温度扰动参数 (0=关闭, >0 添加随机扰动, 默认0.15) */
    float   temperature;

    /* EmergentPOS* — 涌现式词类系统引用，用于输出词性标注和句式重排（NULL=不启用） */
    struct EmergentPOS* emergent_pos;
} DiffusionCtx;

/** 从输入文本生成序列 */
int diffusion_generate(DiffusionCtx* ctx,
                        const char* input,
                        const char** output_words,
                        int max_output);

/** 初始化扩散上下文（自动定位各子拓扑） */
int diffusion_init(DiffusionCtx* ctx, MasterTopology* master);

/** 释放扩散上下文堆资源（评分数组等），调用后方可安全销毁 ctx */
void diffusion_cleanup(DiffusionCtx* ctx);

/** 单层扩散一步：从一组节点扩展到它们的邻居 */
int diffusion_spread(SubTopology* layer,
                      int* active_ids, int active_count,
                      float* scores,     /* [node_count] 累积分数 */
                      float decay,
                      float temperature);  /* 0=关闭, >0 添加随机扰动 */

/** 跨层激活：通过cross_edge从源层扩散到目标层 */
int diffusion_cross_spread(MasterTopology* master,
                           int src_topo_id, int* src_ids, int src_count,
                           int dst_topo_id,
                           float* scores, float weight);

/** 侧抑制：已选词抑制候选中的同类词 */
void diffusion_side_inhibit(DiffusionCandidate* cands, int count,
                             const char** selected, int sel_count);

/** 模板评分：给定候选序列，评估模板匹配度 */
float diffusion_template_score(DiffusionCtx* ctx,
                                const char** words, int count);

/** 虚词/停用词检查 — 复用 diffusion 统一虚词表，供 dialog_generate 等模块调用 */
int diffusion_is_stop_word(const char* word);

#ifdef __cplusplus
}
#endif

#endif
