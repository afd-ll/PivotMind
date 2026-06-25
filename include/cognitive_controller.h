/**
 * @file cognitive_controller.h
 * @brief 认知调度中心 — 位于记忆系统和子拓扑之间
 *
 * 核心职责：
 * 1. 根据记忆状态计算意图向量（intent_weights）
 * 2. 调度各子拓扑的搜索偏好
 * 3. 评估生成草案的内感受评分
 * 4. 负反馈修正：不满意就调整再试
 */

#ifndef COGNITIVE_CONTROLLER_H
#define COGNITIVE_CONTROLLER_H

#include "multi_topology.h"
#include "memory_system.h"
#include <stdbool.h>

/** 前向声明: 涌现词类系统（定义在 emergent_pos.h） */
typedef struct EmergentPOS EmergentPOS;

// ==================== 常量 ====================

/** 子拓扑数量上限（匹配 TopologyType 枚举 0-10，预留 1 个扩展位） */
#define MAX_SUBTOPOS 12

/** 束搜索候选路径池大小 */
#define PATH_POOL_SIZE 10

/** 路径最大长度 */
#define MAX_PATH_LENGTH 32

/** 语义意图类型（值域匹配 dialog_system.h 的 IntentType 枚举）
 *  UNKNOWN=0 QUERY=1 EXPLAIN=2 COMPARE=3 DEFINE=4
 *  HOWTO=5 CHAT=6 LEARN=7 TEST=8 FEEDBACK=9
 */

/** 最大修正次数 */
#define MAX_RETRY 3

/** 路径缓冲大小（环形） */
#define CC_PATH_BUF_SIZE 500

/** 单条路径最大步数 */
#define CC_PATH_MAX_LEN 32

/** 修正状态返回值 */
typedef enum {
    RETRY_OK          = 0,   // 通过，无需修正
    RETRY_FROM_POOL   = 1,   // 从候选池重排（不重搜）
    RETRY_WITH_SEARCH = 2,   // 缩域重搜（需重新 dialog_reasoning_create）
    RETRY_FAILED      = -1   // 已达上限或无解，强制输出
} RetryStatus;

// ==================== 路径结构 ====================

/**
 * 单条路径：节点序列 + 综合评分
 */
typedef struct {
    int node_ids[MAX_PATH_LENGTH];     // 节点ID序列
    int topo_id;                       // 所属子拓扑ID
    int length;                        // 实际长度
    float score;                       // 综合评分
    float act_sum;                     // 累计激活值
    float conf_sum;                    // 累计置信度
} PathResult;

// ==================== 词性标注系统 ====================

/** POS 标签枚举 */
typedef enum {
    POS_UNKNOWN = 0,  // 未知
    POS_NOUN    = 1,  // 名词
    POS_VERB    = 2,  // 动词
    POS_ADJ     = 3,  // 形容词
    POS_ADV     = 4,  // 副词
    POS_PRON    = 5,  // 代词
    POS_PREP    = 6,  // 介词
    POS_CONJ    = 7,  // 连词
    POS_NUM     = 8,  // 数词/量词
    POS_PARTICLE= 9,  // 助词
    POS_INTERJ  = 10, // 叹词
    POS_COUNT   = 11
} POSTag;

/** POS 模式观测缓冲区大小 */
#define POS_OBS_BUF_SIZE 2048

/** 最大 POS 模式数 */
#define MAX_POS_PATTERNS 128

/** POS 句式模式（自动发现） */
typedef struct {
    POSTag pos_seq[8];       // POS 序列
    int length;              // 序列长度
    int count;               // 观测次数
    int syntax_node_id;      // 在句式拓扑中的节点ID（-1=未创建）
    float avg_freq;          // 平均频率归一化值
} POSPattern;

/** 软分类最大候选数 */
#define SOFT_CLASS_MAX 4

/**
 * 涌现词类软分类结果 — 一个词的多个可能词类
 *
 * 用于多义词：如"计划"同时属于名词和动词。
 * 特征向量在高维空间中可以同时接近多个锚点中心。
 */
typedef struct {
    POSTag tags[SOFT_CLASS_MAX];           // 候选词类
    float  confs[SOFT_CLASS_MAX];          // 余弦相似度
    int    count;                          // 实际候选数
} SoftClassResult;

// ==================== 认知调度中心 ====================

/**
 * 认知调度中心结构体
 *
 * 运行在主循环中，位于记忆系统和子拓扑之间。
 * 每次对话回合，根据当前记忆状态计算意图向量，
 * 指导各子拓扑的搜索方向，并对产出草案进行内感受评估。
 */
typedef struct {
    // ========== 1. 意图向量 ==========
    float intent_weights[MAX_SUBTOPOS];  // 喂给每个子拓扑的偏好系数

    // ========== 2. 调度策略偏置 ==========
    float context_bias;      // 上下文记忆给出的偏向强度 (0.0-1.0)
    float novelty_bias;      // 短时记忆给出的求新强度 (0.0-1.0)
    float valence_bias;      // 效价(用户反馈)的整体调节强度 (0.0-1.0)
    float coherence_target;      // 语义连贯性目标 (0.0-1.0)
    float coherence_influence_scale;  // 连贯性因子缩放系数 (默认0.5，替代硬编码魔法数字)

    // ========== 3. 负反馈调节状态 ==========
    float satisfaction_threshold;  // 多高的内感受评分才算通过 (0.0-1.0)
    int   max_retry;               // 最多修正几次
    float correction_strength;     // 每次修正的力度 (0.0-1.0)
    int   retry_count;             // 当前回合己修正次数

    // ========== 4. 上次决策的快照 ==========
    float prev_intent_weights[MAX_SUBTOPOS];  // 上一轮意图向量
    float prev_satisfaction;                   // 上一轮满意度

    // ========== 5. 候选路径池（用于不重搜修正） ==========
    PathResult path_pool[MAX_SUBTOPOS][PATH_POOL_SIZE];  // 每个子拓扑的候选池
    int pool_counts[MAX_SUBTOPOS];                        // 各池当前大小

    // ========== 6. 外部分量（由主流程注入） ==========
    MasterTopology* master;
    MemorySystem*   memory;
    void*           causal_graph;      // CausalGraph*（用于因果一致性检验）
    void*           concept_hierarchy; // ConceptHierarchy*（用于自矛盾检测）
    const char*     current_input;     // 当前用户输入（仅引用，不拥有）
    const char*     last_response;     // 上一轮回复（仅引用，不拥有）
    int             intent_type;       // 当前语义意图类型 (INTENT_CHAT/QUERY/...)

    // ========== 7. 路径观察与概念涌现 ==========
    // 环形缓冲区
    int path_buf_nodes[CC_PATH_BUF_SIZE][CC_PATH_MAX_LEN];  // 节点ID序列
    int path_buf_lens[CC_PATH_BUF_SIZE];                     // 每条路径长度
    int path_buf_topo[CC_PATH_BUF_SIZE];                     // 拓扑类型
    int path_buf_count;                                      // 当前条目数
    int path_buf_cursor;                                     // 环形写入位置

    // 检测到的模式
    struct {
        int* node_ids;               // 序列节点ID
        int length;
        int count;                   // 出现次数
        float avg_edge_strength;     // 平均边强度
        int composite_id;            // 复合节点ID (-1=未创建)
    }* patterns;
    int pattern_count;
    int pattern_capacity;

    // 配置
    int scan_counter;                // 累计计数器（每50步扫描）
    int min_pattern_freq;            // 最低频率才创建复合节点
    float min_edge_strength;         // 最低边强度
    float composite_boost;           // 复合节点权重提升系数

    // ========== 9. POS 模式发现与句式自动扩展 ==========
    POSTag pos_obs_buf[POS_OBS_BUF_SIZE][16];  // 环形缓冲：POS 序列
    int    pos_obs_lens[POS_OBS_BUF_SIZE];      // 每条序列长度
    int    pos_obs_cursor;                       // 环形写入位置
    int    pos_obs_count;                        // 已写入总数（含回绕）

    POSPattern pos_patterns[MAX_POS_PATTERNS];   // 自动发现的句式模式
    int        pos_pattern_count;                 // 当前模式数

    // Scaffold 生成状态
    POSTag scaffold_seq[8];         // 当前选中的句式骨架 POS 序列
    int    scaffold_len;             // 骨架长度
    int    scaffold_active;          // 是否启用 scaffold 引导

    // ========== 8. 在线学习 ==========
    float learned_base[MAX_SUBTOPOS];    // 意图基准在线学习因子 (1.0=未调整)

    // ========== 10. 涌现式词类系统 ==========
    EmergentPOS* emergent_pos;           // 种子锚点 + 特征向量聚类词类系统

} CognitiveController;

// ==================== API函数 ====================

/**
 * 创建认知调度中心
 */
CognitiveController* cognitive_controller_create(MasterTopology* master,
                                                  MemorySystem* memory);

/**
 * 销毁认知调度中心
 */
void cognitive_controller_destroy(CognitiveController* cc);

/**
 * 计算上下文激活度 —— 估算各子拓扑与当前输入的关联度
 *
 * 在词汇/语义/情绪拓扑中匹配输入 token，
 * 返回各子拓扑的活跃节点比例作为上下文激活度。
 *
 * @param cc        认知调度中心（需已设置 current_input）
 * @param ctx_activations 输出数组 [MAX_SUBTOPOS]，调用者分配
 */
void calc_context_activations(CognitiveController* cc, float* ctx_activations);

/**
 * 计算意图向量 —— 调度中心的核心
 *
 * 根据当前记忆状态和历史快照，为每个子拓扑计算偏好权重。
 * 权重 = 上下文相关性 × 新颖性 × 效价偏好 × 连贯性要求
 *
 * @param cc        认知调度中心
 * @param ctx_activations 各个子拓扑的当前上下文激活度（可为NULL，退化到无上下文模式）
 */
void compute_intent(CognitiveController* cc, const float* ctx_activations);

/**
 * 新颖性衰减 — 每轮结束后对子拓扑 recent_activation 做指数衰减。
 *
 * 从 calc_novelty_factors（纯读取）中分离，确保 compute_intent_local
 * 线程安全版本不会意外修改共享状态。
 * 仅在单线程路径 (compute_intent) 末尾调用。
 */
void cognitive_controller_decay_novelty(CognitiveController* cc);

/**
 * 计算意图向量 — 线程安全版本，输出到调用者提供的 buffer
 *
 * 与 compute_intent 算法完全相同，但结果写入 output_weights 而非 cc->intent_weights。
 * 适用于多线程推理场景，避免竞态写入共享状态。
 *
 * @param cc        认知调度中心（只读引用）
 * @param ctx_activations 上下文激活度（可为NULL）
 * @param output_weights  输出缓冲区 [MAX_SUBTOPOS]，调用者分配
 */
void compute_intent_local(CognitiveController* cc,
                          const float* ctx_activations,
                          float* output_weights);

/**
 * 内感受评估 —— 检查生成的草案是否满意
 *
 * 使用情绪子拓扑和语义子拓扑评估草案质量。
 *
 * @param cc        认知调度中心
 * @param draft     当前生成的草案路径
 * @param draft_len 草案长度
 * @return 满意度评分 (0.0-1.0)
 */
float evaluate_draft(CognitiveController* cc,
                     const PathResult* draft,
                     int draft_len);

/**
 * 因果路径评分 —— 用于在走边阶段筛选候选路径（中游约束）
 *
 * 与 evaluate_draft 分离：因果属于"探索约束"，内感受属于"效价检验"。
 */
float causal_path_score(CognitiveController* cc,
                        SubTopology* sub,
                        const int* node_ids,
                        int path_len);

/**
 * 计算修正向量 —— 不满意时生成修正方向
 *
 * @param cc        认知调度中心
 * @param draft     当前草案
 * @param satisfaction 满意度评分
 * @param correction   输出修正向量（长度 MAX_SUBTOPOS）
 */
void compute_correction_vector(CognitiveController* cc,
                               const PathResult* draft,
                               float satisfaction,
                               float* correction);

/**
 * 负反馈修正 —— 不满意时调整意图权重并重新调度
 *
 * @param cc        认知调度中心
 * @param draft     当前草案
 * @param satisfaction 满意度评分
 * @return RetryStatus: RETRY_OK/FROM_POOL/WITH_SEARCH/FAILED
 */
RetryStatus revise_and_retry(CognitiveController* cc,
                             const PathResult* draft,
                             float satisfaction);

/**
 * 保存路径到候选池（用于第1次修正不重搜）
 */
void pool_save_path(CognitiveController* cc, int topo_idx,
                    const PathResult* path);

/**
 * 从候选池中按新权重选出最佳路径
 */
int pool_select_best(CognitiveController* cc, int topo_idx,
                     PathResult* out);

/**
 * 重置修正状态（每轮对话开始前调用）
 */
void cognitive_controller_reset_round(CognitiveController* cc);

/**
 * 保存本轮决策快照（供下轮使用）
 */
void cognitive_controller_snapshot(CognitiveController* cc, float satisfaction);

/**
 * 设置当前用户输入和上一轮回复
 */
void cognitive_controller_set_context(CognitiveController* cc,
                                      const char* input,
                                      const char* last_response);

/**
 * 设置语义意图类型（由 dialog_system 的 semantic_understanding_create 注入）
 */
void cognitive_controller_set_intent(CognitiveController* cc, int intent_type);

/**
 * 意图基准在线学习：本轮推理中活跃的拓扑，其基准权重提高
 * @param cc 认知调度器
 * @param used_topos 本轮实际使用的拓扑ID数组
 * @param topo_count 拓扑数量
 * @param feedback 反馈强度（0.0-1.0，1.0=完全满意）
 */
void intent_base_learn(CognitiveController* cc, const int* used_topos,
                       int topo_count, float feedback);

/**
 * 获取子拓扑名称（用于调试日志）
 */
const char* cognitive_controller_topo_name(int topo_type);

// ==================== 路径观察与概念涌现 ====================

/**
 * 观察生成的走边路径 — 喂入缓冲供模式分析
 */
void cognitive_controller_observe_path(CognitiveController* cc,
                                        int topo_type,
                                        const int* node_ids,
                                        int path_len);

/**
 * 扫描路径模式 — 检测高频共现序列，自动创建复合节点
 * 返回本次新创建的复合节点数
 */
int cognitive_controller_scan_patterns(CognitiveController* cc);

/**
 * 获取统计信息
 */
int cognitive_controller_pattern_count(CognitiveController* cc);

/**
 * 启发式中文字符/词词性标注
 * 基于硬编码的常用字词性字典 + 规则推断
 */
POSTag pos_tag_chinese(const char* word);
const char* pos_tag_name(POSTag tag);

/**
 * 初始化句式拓扑（TOPO_SYNTAX）
 * 创建常见汉语句式模式节点（SVO/SV/SOV/SVOC 等），
 * 并建立 POS 序列到句式节点的跨拓扑连接
 */
int cc_init_sentence_topology(CognitiveController* cc);

/**
 * POS 序列兼容性检查 — 判断候选词性是否能续接当前句式上下文
 */
float cc_pos_compatibility(CognitiveController* cc,
                            const POSTag* pos_sequence, int seq_len,
                            POSTag candidate_pos);

/**
 * 观测 POS 序列 — 训练时从回答文本中提取 POS 序列并记录
 */
void cc_observe_pos_sequence(CognitiveController* cc,
                              const POSTag* seq, int len);

/**
 * 扫描 POS 模式 — 从观测缓冲中发现高频 POS n-gram
 */
int cc_scan_pos_patterns(CognitiveController* cc);

/**
 * 选择句式模板 — 为当前输入选择最佳句式骨架
 */
int cc_select_sentence_pattern(CognitiveController* cc, const char* input);

/**
 * 句式 scaffold 评分 — 在生成步中检查候选 POS 是否匹配当前位置
 */
float cc_scaffold_bonus(CognitiveController* cc, int position,
                         POSTag candidate_pos);

/**
 * 路径模式匹配评分 — 检查当前 POS 序列与已知句式模式的匹配度
 *
 * 遍历内置句式 (CN_PATTERNS) 和自动发现的 POS 模式 (pos_patterns)，
 * 对当前已走过的 POS 序列做前缀匹配，返回最佳匹配分。
 * 用于 walk 阶段的候选评分，引导路径向自然句式靠拢。
 *
 * @param cc        认知调度中心
 * @param topo_id   当前子拓扑 ID（预留，可用于拓扑专属模式）
 * @param pos_trail 当前路径的 POS 序列（int 数组，值为 POSTag 枚举）
 * @param trail_len 序列长度
 * @return 匹配评分 (0.0-1.0)，越高越符合已知句式
 */
float cc_pattern_match_score(CognitiveController* cc, int topo_id,
                              const int* pos_trail, int trail_len);

/**
 * 获取选中的句式模板 POS 序列（用于调试）
 */
int cc_get_selected_pattern(CognitiveController* cc, POSTag* seq_out);

/**
 * 获取所有已知句式模式（用于外部统计）
 */
int cc_get_all_patterns(CognitiveController* cc,
                        POSPattern* patterns_out, int max_count);

/**
 * 概念文本输出消毒 — 检查概念是否适合输出到回复中
 * 允许中文、字母数字、合法标点；拒绝 @#$%^&*+=|~` 空格等纯噪音符号
 * @return 1=可输出, 0=应静默跳过
 */
int concept_is_printable(const char* concept);

// ==================== 涌现式词类系统 API ====================

/**
 * 初始化涌现词类系统
 *
 * 在词汇拓扑足够丰富后调用（通常 start_environ 中或词汇 >500 节点后）。
 * 扫描词汇拓扑找种子词节点，用其特征向量初始化锚点中心。
 *
 * @param cc    认知调度中心
 * @param lang  语言 "zh" / "en"
 * @return 成功初始化的锚点数
 */
int cc_init_emergent_pos(CognitiveController* cc, const char* lang);

/**
 * 涌现式词性标注（双层路由）
 *
 * 优先用锚点系统的特征向量匹配，失败则回退到硬编码字典取种子词。
 * 每次成功匹配自动微调锚点中心。
 *
 * @param cc    认知调度中心
 * @param word  词名
 * @return POSTag 标签
 */
POSTag pos_tag_emergent(CognitiveController* cc, const char* word);

/**
 * 涌现式软标注 — 返回多个候选词类（支持多义词）
 */
void pos_tag_emergent_soft(CognitiveController* cc, const char* word,
                           SoftClassResult* result);

/**
 * 检查涌现词类系统是否就绪
 */
int cc_emergent_pos_ready(CognitiveController* cc);

#endif // COGNITIVE_CONTROLLER_H
