/**
 * @file dialog_system.h
 * @brief 对话系统头文件 - 基于多拓扑网络的智能对话引擎
 */

#ifndef DIALOG_SYSTEM_H
#define DIALOG_SYSTEM_H

#include "multi_topology.h"
#include "memory_system.h"
#include "causal_reasoning.h"
#include "active_learner.h"
#include "cognitive_params.h"
#include <stdbool.h>
#include <time.h>

// ==================== 意图识别 ====================

/**
 * 对话意图类型
 */
typedef enum {
    INTENT_UNKNOWN = 0,       // 未知
    INTENT_QUERY,            // 查询问题
    INTENT_EXPLAIN,          // 解释原因（为什么）
    INTENT_COMPARE,          // 比较
    INTENT_DEFINE,           // 定义
    INTENT_HOWTO,            // 如何做
    INTENT_CHAT,             // 闲聊
    INTENT_LEARN,            // 学习新知识
    INTENT_TEST,             // 测试/提问
    INTENT_FEEDBACK          // 反馈/评价
} DialogIntent;

/**
 * 意图识别结果
 */
typedef struct {
    DialogIntent intent;          // 识别的意图
    float confidence;             // 置信度
    char* original_phrase;        // 原始短语
} IntentResult;

// ==================== 实体识别 ====================

/**
 * 实体类型
 */
typedef enum {
    ENTITY_UNKNOWN = 0,
    ENTITY_OBJECT,        // 实体对象（CPU、内存...）
    ENTITY_ACTION,        // 动作（运行、发热...）
    ENTITY_ATTRIBUTE,     // 属性（温度、速度...）
    ENTITY_CONCEPT,       // 抽象概念（性能、效率...）
    ENTITY_CAUSAL         // 因果关系（导致、因为...）
} EntityType;

/**
 * 识别出的实体
 */
typedef struct {
    char* text;               // 实体文本
    char* normalized;         // 归一化文本
    EntityType type;          // 实体类型
    float confidence;         // 置信度
    int start_pos;           // 在原文本中的起始位置
    int end_pos;             // 结束位置
} DialogEntity;

// ==================== 对话激活结构 ====================

typedef struct {
    int topo_id;          // 拓扑ID
    int node_id;          // 节点ID
    float activation;     // 激活值
} DialogActivation;

// ==================== 语义理解 ====================

/**
 * 语义理解结果
 */
typedef struct {
    // 原始输入
    char* original_text;
    int text_length;

    // 分词
    char** tokens;
    int token_count;

    // 意图
    IntentResult intent;

    // 实体
    DialogEntity* entities;
    int entity_count;

    // 关键词（用于拓扑激活）
    char** key_concepts;
    int* key_concept_ids;      // 对应的拓扑节点ID
    int key_concept_count;

    // 激活的拓扑节点
    DialogActivation activations[100];
    int activation_count;

    // 关联的因果查询（如果有）
    int cause_node_id;         // 原因节点ID（拓扑）
    int effect_node_id;        // 效果节点ID（拓扑）
    bool causal_query;        // 是否是因果查询
} SemanticUnderstanding;

// ==================== 对话输入解析 ====================

typedef struct {
    char* original;            // 原始输入文本
    int original_length;       // 原始长度
    char** tokens;            // 分词结果
    int token_count;          // token数量
} DialogInput;

// ==================== 对话推理结构 ====================

#define MAX_ASSOCIATIONS 100

typedef struct {
    char concept[256];         // 概念名称
    float activation;          // 激活强度
    int topo_type;             // 来自哪个拓扑
    int hop_count;             // 推理跳数
    int node_id;               // 节点ID ★新增
    int from_node_id;          // 来源节点ID ★新增
} DialogAssociation;

typedef struct {
    DialogAssociation associations[MAX_ASSOCIATIONS];
    int assoc_count;
    
    // 推理路径（每个推理步骤）
    int path_nodes[MAX_ASSOCIATIONS];       // 节点ID序列
    int path_topos[MAX_ASSOCIATIONS];        // 对应拓扑ID
    float path_scores[MAX_ASSOCIATIONS];     // 每步置信度
    int path_depth;                          // 当前路径深度
    
    // 统计信息
    int total_activations;
    float avg_activation;
    
    // 自我验证结果
    float knowledge_quality;    // 知识质量评估 (0-1)
    int is_verified;          // 是否已验证
    
    // 推理链
    char reasoning_chain[10][256];  // 推理步骤
    int chain_length;               // 推理链长度
} DialogReasoning;

// ==================== 主对话系统结构 ====================

typedef struct {
    MasterTopology* master;    // 多拓扑网络
    MemorySystem* memory;      // 记忆系统
    CausalGraph* causal_graph; // 因果图
    ActiveLearner* learner;    // 主动学习器（可选）
    void* concept_hierarchy;   // 概念层次结构（ConceptHierarchy*）
    void* str_pool;            // 字符串池（StringPool*）
    void* seq2seq;             // Seq2Seq神经模型（Seq2SeqModel*）
    void* gen_vocab;           // 生成词汇表（GenVocabulary*）
    
    long session_id;           // 会话ID
    int turn_count;            // 对话轮数
    
    // 配置
    int max_hop_count;         // 最大推理跳数
    float activation_threshold;// 激活阈值
    float decay_rate;          // 衰减率
    CognitiveState* cognitive_state;  // 认知状态（情感/动机系统）
    float last_knowledge_quality;    // 上一轮知识质量（供CognitiveState使用）
} DialogSystem;

// ==================== API函数 ====================

// ==================== 语义理解 API ====================

/**
 * 语义理解主函数
 * @param text 用户输入文本
 * @return 语义理解结果（需调用 semantic_understanding_destroy 释放）
 */
SemanticUnderstanding* semantic_understand(const char* text);

/**
 * 释放语义理解结果
 * @param sem 语义理解结果
 */
void semantic_understanding_destroy(SemanticUnderstanding* sem);

/**
 * 意图识别
 * @param text 输入文本
 * @return 意图识别结果
 */
IntentResult recognize_intent(const char* text);

/**
 * 实体识别
 * @param text 输入文本
 * @param entities 输出实体数组（需预先分配）
 * @param max_entities 最大实体数
 * @return 识别出的实体数量
 */
int recognize_entities(const char* text, DialogEntity* entities, int max_entities);

/**
 * 关键词提取（用于拓扑激活）
 * @param sem 语义理解结果
 * @param concepts 输出概念数组（需预先分配）
 * @param max_concepts 最大概念数
 * @return 提取的概念数量
 */
int extract_key_concepts(SemanticUnderstanding* sem, char** concepts, int max_concepts);

// ==================== 因果推理联动 ====================

/**
 * 基于语义理解执行因果推理
 * @param sem 语义理解结果
 * @param graph 因果图
 * @param memory 记忆系统
 * @return 因果推理结果描述（需调用者释放）
 */
char* causal_reason_from_semantic(SemanticUnderstanding* sem, CausalGraph* graph,
                                 MemorySystem* memory);

/**
 * 处理因果查询（为什么/原因）
 * @param sem 语义理解结果
 * @param graph 因果图
 * @param memory 记忆系统
 * @return 完整回复（需调用者释放）
 */
char* process_causal_query(SemanticUnderstanding* sem, CausalGraph* graph,
                          MemorySystem* memory);

// ==================== 兼容旧接口 ====================

// 对话输入解析
DialogInput* dialog_parse_input(const char* text);
void dialog_input_destroy(DialogInput* input);

// 对话推理
DialogReasoning* dialog_reason(DialogInput* input, MasterTopology* master);
void dialog_add_association(DialogReasoning* reasoning, const char* concept,
                           float activation, int topo_type, int hop_count,
                           int node_id, int from_node_id);
void dialog_reasoning_destroy(DialogReasoning* reasoning);

// 回复生成
char* dialog_generate(DialogReasoning* reasoning, const char* input,
                     MemorySystem* memory, int max_len, void* sys);

// 主对话流程
DialogSystem* dialog_system_create(MasterTopology* master, MemorySystem* memory,
                                CausalGraph* causal_graph, ActiveLearner* learner);
void dialog_system_destroy(DialogSystem* sys);
char* dialog_process(DialogSystem* sys, const char* user_input, DialogReasoning** out_reasoning);

// 自动学习
void auto_learn_concepts(MasterTopology* master, const char* text, void* str_pool);

// 测试
void dialog_test(MasterTopology* master, MemorySystem* memory, CausalGraph* causal_graph, ActiveLearner* learner);

#endif // DIALOG_SYSTEM_H