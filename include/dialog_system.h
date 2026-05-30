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
#include "cognitive_controller.h"
#include "bptt_learner.h"
#include <stdbool.h>
#include <time.h>

// ==================== 意图识别 ====================

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

typedef struct {
    DialogIntent intent;
    float confidence;
    char* original_phrase;
} IntentResult;

// ==================== 实体识别 ====================

typedef enum {
    ENTITY_UNKNOWN = 0,
    ENTITY_OBJECT,
    ENTITY_ACTION,
    ENTITY_ATTRIBUTE,
    ENTITY_CONCEPT,
    ENTITY_CAUSAL
} EntityType;

typedef struct {
    char* text;
    char* normalized;
    EntityType type;
    float confidence;
    int start_pos;
    int end_pos;
} DialogEntity;

// ==================== 对话激活结构 ====================

typedef struct {
    int topo_id;
    int node_id;
    float activation;
} DialogActivation;

// ==================== 语义理解 ====================

typedef struct {
    char* original_text;
    int text_length;
    char** tokens;
    int token_count;
    IntentResult intent;
    DialogEntity* entities;
    int entity_count;
    char** key_concepts;
    int* key_concept_ids;
    int key_concept_count;
    DialogActivation activations[100];
    int activation_count;
    int cause_node_id;
    int effect_node_id;
    bool causal_query;
} SemanticUnderstanding;

// ==================== 对话输入解析 ====================

typedef struct {
    char* original;
    int original_length;
    char** tokens;
    int token_count;
} DialogInput;

// ==================== 对话推理结构 ====================

#define MAX_ASSOCIATIONS PM_MAX_ASSOCIATIONS

typedef struct {
    char concept[PM_CONCEPT_NAME];
    float activation;
    int topo_type;
    int hop_count;
    int node_id;
    int from_node_id;
} DialogAssociation;

typedef struct {
    DialogAssociation associations[MAX_ASSOCIATIONS];
    int assoc_count;
    int path_nodes[MAX_ASSOCIATIONS];
    int path_topos[MAX_ASSOCIATIONS];
    float path_scores[MAX_ASSOCIATIONS];
    int path_depth;
    int total_activations;
    float avg_activation;
    float knowledge_quality;
    int is_verified;
    char reasoning_chain[10][PM_CONCEPT_NAME];
    int chain_length;
} DialogReasoning;

// ==================== 自我验证 ====================

typedef struct {
    float confidence;
    int is_consistent;
    char suggestion[256];
} SelfVerificationResult;

SelfVerificationResult self_verify_knowledge(DialogReasoning* reasoning, MemorySystem* memory);

// ==================== 主对话系统结构 ====================

typedef struct {
    MasterTopology* master;
    MemorySystem* memory;
    CausalGraph* causal_graph;
    ActiveLearner* learner;
    void* concept_hierarchy;
    void* str_pool;
    void* _placeholder;
    long session_id;
    int turn_count;
    int max_hop_count;
    float activation_threshold;
    float decay_rate;
    CognitiveState* cognitive_state;
    float last_knowledge_quality;
    CognitiveController* controller;
    BpttLearner* bptt;               // RNN BPTT 在线学习器

    // 预测误差反馈环
    int last_path_node_ids[PM_PATH_TRACK];
    int last_path_topo_types[PM_PATH_TRACK];
    int last_path_edge_ids[PM_PATH_TRACK];
    int last_path_count;
    char last_input[1024];
    char last_response[PM_RESPONSE_BUF];
    int has_last_turn;

    float prediction_lr;
    float curiosity;
    int consecutive_success;
    int consecutive_failures;
} DialogSystem;

// ==================== 预测误差反馈 ====================

float compute_prediction_error(DialogSystem* sys, const char* actual_input);
void apply_prediction_feedback(DialogSystem* sys, float error);

// ==================== API 函数 ====================

// 语义理解
SemanticUnderstanding* semantic_understanding_create(const char* text);
void semantic_understanding_destroy(SemanticUnderstanding* sem);
IntentResult recognize_intent(const char* text);
int recognize_entities(const char* text, DialogEntity* entities, int max_entities);
int extract_key_concepts(SemanticUnderstanding* sem, char** concepts, int max_concepts);
void resolve_causal_query(SemanticUnderstanding* sem, MasterTopology* master);

// 因果推理联动
char* causal_reason_from_semantic(SemanticUnderstanding* sem, CausalGraph* graph,
                                 MemorySystem* memory);
char* process_causal_query(SemanticUnderstanding* sem, CausalGraph* graph,
                          MemorySystem* memory);

// 对话输入解析
DialogInput* dialog_input_create(const char* text);
void dialog_input_destroy(DialogInput* input);

// 对话推理
DialogReasoning* dialog_reasoning_create(DialogInput* input, MasterTopology* master,
                               const float* intent_weights);
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

// ==================== 旧名兼容别名 ====================
#define semantic_understand    semantic_understanding_create
#define dialog_parse_input     dialog_input_create
#define dialog_reason          dialog_reasoning_create

#endif // DIALOG_SYSTEM_H
