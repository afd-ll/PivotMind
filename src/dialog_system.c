/**
 * @file dialog_system.c
 * @brief 对话系统 - 基于多拓扑网络的智能对话引擎
 * 
 * 对话流程:
 * 1. 输入解析 → 分词 + 激活拓扑节点
 * 2. 联想推理 → 多拓扑激活传播
 * 3. 回复生成 → 基于激活强度动态组合
 * 4. 记忆学习 → 重要对话存入记忆系统
 */

#include "dialog_system.h"
#include "multi_topology.h"
#include "memory_system.h"
#include "huarong_topology.h"
#include "causal_reasoning.h"
#include "utf8_tokenizer.h"
#include "string_pool.h"
#include "node_hash.h"
#include "ui.h"
#include "cognitive_params.h"
#ifdef _WIN32
#include "network_tool.h"
#endif
#include "concept_processor.h"
#include "concept_abstraction.h"
#include "common.h"
#include "autonomic_learner.h"
#include "active_learner.h"
#include "cognitive_controller.h"
#include "web_search.h"
#include "error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdbool.h>
#include <pthread.h>

#include "thread_pool.h"
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#endif

// 前向声明
static const char* get_confidence_level_name(CausalConfidenceLevel level);
static WebResult* dialog_search_concept(const char* concept);

// ==================== 推理常量 ====================

#define MAX_TOKENS   PM_TOKEN_MAX
#define MAX_RESPONSE_LENGTH PM_MAX_RESPONSE_LEN
#undef MAX_PATH_LENGTH
#define MAX_PATH_LENGTH 10
#define DEFAULT_HOP_COUNT PM_DEFAULT_HOP_COUNT
#define OUTPUT_CACHE_SIZE PM_OUTPUT_CACHE_SIZE

// ==================== 并行拓扑传播任务（每跳内部） ====================

/** 单跳内子拓扑传播任务 */
typedef struct {
    MasterTopology* master;
    DialogReasoning* reasoning;
    int topo_id;
    int hop;
    pthread_mutex_t* assoc_mutex;
    volatile int* hop_propagated;
    const float* intent_weights;  // 认知调度给出的子拓扑偏好
} DialogTopoTask;

/** Worker：在指定子拓扑内执行一跳传播 */
static void dialog_topo_worker(void* arg) {
    DialogTopoTask* task = (DialogTopoTask*)arg;
    MasterTopology* master = task->master;
    DialogReasoning* reasoning = task->reasoning;
    SubTopology* sub = master_get_sub_topology(master, task->topo_id);
    if (!sub || !sub->net) return;

    // 先收集激活节点（避免遍历全部 ~23712 节点）
    int* active_ids = (int*)malloc(sub->net->node_count * sizeof(int));
    int active_count = 0;
    if (active_ids) {
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (node && node->activation >= 0.15f &&
                !(task->hop > 1 && node->is_visited)) {
                active_ids[active_count++] = n;
            }
        }
    }

    for (int ai = 0; ai < active_count; ai++) {
        int n = active_ids[ai];
        ReasoningNode* node = sub->net->nodes[n];
        if (!node) continue;

        node->is_visited = 1;  // 安全：每个 worker 处理唯一的子拓扑（topo_id 不重叠）

        for (int c = 0; c < node->edge_count; c++) {
            ReasoningNode* connected = node->edges[c].target;
            if (!connected) continue;

            float edge_confidence = node->edges[c].confidence;
            float avg_confidence = (node->confidence + connected->confidence + edge_confidence) / 3.0f;

            float activation_multiplier = 1.0f;
            if (avg_confidence < 0.3f) activation_multiplier = 1.3f;
            else if (avg_confidence > 0.7f) activation_multiplier = 0.7f;

            float confidence_factor = avg_confidence;

            float embed_factor = 1.0f;
            if (node->features && connected->features &&
                node->feature_dim > 0 && node->feature_dim == connected->feature_dim) {
                float sim = cosine_similarity(node->features, connected->features, node->feature_dim);
                embed_factor = 0.5f + 0.5f * (sim + 1.0f) / 2.0f;
            }

            // 自适应衰减：前2跳宽松（0.85/0.80），第3跳收紧（0.65）
            // 确保深度传播的同时防止无限扩散
            float adaptive_decay;
            if (task->hop <= 1)       adaptive_decay = 0.85f;
            else if (task->hop == 2)  adaptive_decay = 0.80f;
            else                      adaptive_decay = 0.65f;

            float new_activation = node->edges[c].weight *
                                  node->activation *
                                  confidence_factor *
                                  activation_multiplier *
                                  embed_factor *
                                  adaptive_decay;

            // 认知调度：乘以当前子拓扑的意图权重
            // 仅对认知拓扑(0-9)加权，模板拓扑(10)不参与意图调制
            if (task->intent_weights && sub->type >= 0 && sub->type <= TOPO_MASTER) {
                new_activation *= task->intent_weights[sub->type];
            }

            if (new_activation > ACTIVATION_THRESHOLD) {
                /* 节点级锁：保护 activation 字段防止与后台时钟竞态 */
                int conn_lock = connected->node_id & (PM_NODE_LOCK_COUNT - 1);
                pthread_mutex_lock(&sub->net->node_locks[conn_lock]);
                if (new_activation > connected->activation)
                    connected->activation = new_activation;
                pthread_mutex_unlock(&sub->net->node_locks[conn_lock]);

                pthread_mutex_lock(task->assoc_mutex);
                dialog_add_association(reasoning,
                    connected->concept, new_activation,
                    sub->type, task->hop,
                    connected->node_id, node->node_id);

                if (reasoning->chain_length < 10 && task->hop <= 3) {
                    snprintf(reasoning->reasoning_chain[reasoning->chain_length],
                             256, "%s -> %s",
                             node->concept ? node->concept : "?",
                             connected->concept ? connected->concept : "?");
                    reasoning->chain_length++;
                }
                pthread_mutex_unlock(task->assoc_mutex);

                (*task->hop_propagated)++;
            }
        }
        master_propagate_activation(master, sub->topo_id, n);
    }
    free(active_ids);
}

// ==================== 辅助函数 ====================

// ==================== 意图识别 ====================

// 意图关键词
static const char* INTENT_QUERY_WORDS[] = {"是什么", "什么是", "哪个", "多少", "谁", "什么时候"};
static const int INTENT_QUERY_COUNT = 6;

static const char* INTENT_EXPLAIN_WORDS[] = {"为什么", "原因", "怎么回事", "为什么呢", "怎么会", "导致", "造成"};
static const int INTENT_EXPLAIN_COUNT = 7;

static const char* INTENT_HOWTO_WORDS[] = {"怎么", "如何", "怎样", "方法", "步骤", "操作"};
static const int INTENT_HOWTO_COUNT = 6;

static const char* INTENT_COMPARE_WORDS[] = {"比较", "区别", "不同", "vs", "对比", "差异"};
static const int INTENT_COMPARE_COUNT = 6;

static const char* INTENT_LEARN_WORDS[] = {"学习", "记住", "了解", "知道", "认识"};
static const int INTENT_LEARN_COUNT = 5;

static const char* INTENT_CHAT_WORDS[] = {"你好", "嗨", "在吗", "嘿", "喂"};
static const int INTENT_CHAT_COUNT = 5;

/**
 * 检测文本是否包含指定关键词
 */
static int contains_keyword(const char* text, const char** keywords, int count) {
    if (!text || !keywords) return 0;
    for (int i = 0; i < count; i++) {
        if (strstr(text, keywords[i]) != NULL) {
            return 1;
        }
    }
    return 0;
}

/**
 * 识别对话意图
 */
IntentResult recognize_intent(const char* text) {
    IntentResult result = {INTENT_UNKNOWN, 0.0f, NULL};

    if (!text) return result;

    // 检测"为什么"类问题（优先匹配，因果推理核心）
    if (contains_keyword(text, INTENT_EXPLAIN_WORDS, INTENT_EXPLAIN_COUNT)) {
        result.intent = INTENT_EXPLAIN;
        result.confidence = 0.9f;
    }
    // 检测"怎么/如何"类问题
    else if (contains_keyword(text, INTENT_HOWTO_WORDS, INTENT_HOWTO_COUNT)) {
        result.intent = INTENT_HOWTO;
        result.confidence = 0.8f;
    }
    // 检测"是什么"类问题
    else if (contains_keyword(text, INTENT_QUERY_WORDS, INTENT_QUERY_COUNT)) {
        result.intent = INTENT_QUERY;
        result.confidence = 0.8f;
    }
    // 检测比较类
    else if (contains_keyword(text, INTENT_COMPARE_WORDS, INTENT_COMPARE_COUNT)) {
        result.intent = INTENT_COMPARE;
        result.confidence = 0.7f;
    }
    // 检测学习类
    else if (contains_keyword(text, INTENT_LEARN_WORDS, INTENT_LEARN_COUNT)) {
        result.intent = INTENT_LEARN;
        result.confidence = 0.8f;
    }
    // 检测闲聊
    else if (contains_keyword(text, INTENT_CHAT_WORDS, INTENT_CHAT_COUNT)) {
        result.intent = INTENT_CHAT;
        result.confidence = 0.6f;
    }
    // 默认作为查询
    else {
        result.intent = INTENT_QUERY;
        result.confidence = 0.5f;
    }

    result.original_phrase = strdup(text);
    return result;
}

// ==================== 实体识别 ====================

/**
 * 判断实体类型
 */
static EntityType classify_entity_type(const char* word) {
    if (!word) return ENTITY_UNKNOWN;

    // 因果关键词
    const char* causal_words[] = {"导致", "造成", "引起", "使得", "因为", "所以", "因此"};
    for (int i = 0; i < 7; i++) {
        if (strstr(word, causal_words[i])) return ENTITY_CAUSAL;
    }

    // 动作词
    const char* action_words[] = {"运行", "工作", "发热", "消耗", "使用", "启动", "关闭", "加载"};
    for (int i = 0; i < 8; i++) {
        if (strstr(word, action_words[i])) return ENTITY_ACTION;
    }

    // 属性词
    const char* attr_words[] = {"温度", "速度", "频率", "功率", "效率", "性能", "容量", "大小"};
    for (int i = 0; i < 8; i++) {
        if (strstr(word, attr_words[i])) return ENTITY_ATTRIBUTE;
    }

    // 概念词
    const char* concept_words[] = {"系统", "进程", "程序", "数据", "网络", "内存", "缓存"};
    for (int i = 0; i < 7; i++) {
        if (strstr(word, concept_words[i])) return ENTITY_CONCEPT;
    }

    // 默认为对象
    return ENTITY_OBJECT;
}

/**
 * 识别文本中的实体
 */
int recognize_entities(const char* text, DialogEntity* entities, int max_entities) {
    if (!text || !entities || max_entities <= 0) return 0;

    // 简单实现：基于关键词提取
    // 未来可以扩展为基于 CRF 或神经网络的序列标注

    const char* entity_keywords[] = {
        "CPU", "处理器", "内存", "硬盘", "显卡", "GPU", "主板",
        "温度", "频率", "功耗", "性能", "速度", "负载",
        "程序", "进程", "系统", "软件", "应用",
        "导致", "造成", "引起", "因为", "所以",
        "发热", "运行", "工作", "使用", "加载"
    };
    int keyword_count = sizeof(entity_keywords) / sizeof(entity_keywords[0]);

    int entity_count = 0;

    for (int i = 0; i < keyword_count && entity_count < max_entities; i++) {
        const char* keyword = entity_keywords[i];
        const char* found = strstr(text, keyword);

        if (found) {
            entities[entity_count].text = strdup(keyword);
            entities[entity_count].normalized = strdup(keyword);
            entities[entity_count].type = classify_entity_type(keyword);
            entities[entity_count].confidence = 0.8f;
            entities[entity_count].start_pos = found - text;
            entities[entity_count].end_pos = entities[entity_count].start_pos + strlen(keyword);
            entity_count++;
        }
    }

    return entity_count;
}

// ==================== 关键词提取 ====================

/**
 * 提取关键词（用于拓扑激活）
 */
int extract_key_concepts(SemanticUnderstanding* sem, char** concepts, int max_concepts) {
    if (!sem || !concepts || max_concepts <= 0) return 0;

    int count = 0;

    // 从实体中提取
    for (int i = 0; i < sem->entity_count && count < max_concepts; i++) {
        if (sem->entities[i].type == ENTITY_OBJECT ||
            sem->entities[i].type == ENTITY_ATTRIBUTE ||
            sem->entities[i].type == ENTITY_ACTION) {
            concepts[count++] = strdup(sem->entities[i].normalized);
        }
    }

    // 如果实体不够，从分词中提取
    for (int i = 0; i < sem->token_count && count < max_concepts; i++) {
        // 跳过停用词
        const char* stop_words[] = {"的", "了", "在", "是", "我", "你", "他", "它", "这", "那"};
        int is_stop = 0;
        for (int j = 0; j < 10; j++) {
            if (strcmp(sem->tokens[i], stop_words[j]) == 0) {
                is_stop = 1;
                break;
            }
        }
        if (!is_stop && strlen(sem->tokens[i]) >= 2) {
            // 检查是否已存在
            int exists = 0;
            for (int k = 0; k < count; k++) {
                if (strcmp(concepts[k], sem->tokens[i]) == 0) {
                    exists = 1;
                    break;
                }
            }
            if (!exists) {
                concepts[count++] = strdup(sem->tokens[i]);
            }
        }
    }

    return count;
}

/**
 * 在多拓扑网络中查找概念对应的节点ID
 * @param master 多拓扑网络
 * @param concept 概念名称
 * @return 节点ID，-1 表示未找到
 */
static int find_node_id_by_concept(MasterTopology* master, const char* concept) {
    if (!master || !concept) return -1;

    // 在所有子拓扑中查找
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->node_hash) continue;

        // 使用 node_hash 查找
        ReasoningNode* node = node_hash_find(sub->node_hash, concept);
        if (node) {
            return node->node_id;
        }

        // 线性搜索作为备选
        for (int i = 0; i < sub->net->node_count; i++) {
            ReasoningNode* n = sub->net->nodes[i];
            if (n && strcmp(n->concept, concept) == 0) {
                return n->node_id;
            }
        }
    }

    return -1;
}

/**
 * 从语义理解结果和拓扑网络建立因果查询
 * @param sem 语义理解结果
 * @param master 多拓扑网络
 */
void resolve_causal_query(SemanticUnderstanding* sem, MasterTopology* master) {
    if (!sem || !sem->causal_query || !master) return;

    // 为所有关键概念查找节点ID
    for (int i = 0; i < sem->key_concept_count && i < 50; i++) {
        sem->key_concept_ids[i] = find_node_id_by_concept(master, sem->key_concepts[i]);
        if (sem->key_concept_ids[i] >= 0) {
            LOG_DEBUG("  概念 [%s] → 节点ID %d", sem->key_concepts[i], sem->key_concept_ids[i]);
        }
    }

    // 如果没有找到，使用第一个和最后一个有效概念
    if (sem->cause_node_id < 0) {
        for (int i = 0; i < sem->key_concept_count; i++) {
            if (sem->key_concept_ids[i] >= 0) {
                sem->cause_node_id = sem->key_concept_ids[i];
                break;
            }
        }
    }

    if (sem->effect_node_id < 0) {
        for (int i = sem->key_concept_count - 1; i >= 0; i--) {
            if (sem->key_concept_ids[i] >= 0) {
                sem->effect_node_id = sem->key_concept_ids[i];
                break;
            }
        }
    }

    LOG_DEBUG("  因果查询: 节点%d → 节点%d", sem->cause_node_id, sem->effect_node_id);
}

// ==================== 语义理解主函数 ====================

/**
 * 语义理解主函数
 */
SemanticUnderstanding* semantic_understand(const char* text) {
    if (!text) return NULL;

    SemanticUnderstanding* sem = (SemanticUnderstanding*)calloc(1, sizeof(SemanticUnderstanding));
    if (!sem) return NULL;

    sem->original_text = strdup(text);
    sem->text_length = strlen(text);

    char* tokens_buf[PM_TOKEN_MAX];
    sem->token_count = utf8_tokenize(text, tokens_buf, PM_TOKEN_MAX);
    sem->tokens = (char**)malloc(sem->token_count * sizeof(char*));
    for (int i = 0; i < sem->token_count; i++) {
        sem->tokens[i] = strdup(tokens_buf[i]);
    }
    // 释放 utf8_tokenize 分配的临时缓冲区
    for (int i = 0; i < sem->token_count; i++) {
        free(tokens_buf[i]);
    }

    sem->intent = recognize_intent(text);

    sem->entities = (DialogEntity*)malloc(50 * sizeof(DialogEntity));
    sem->entity_count = recognize_entities(text, sem->entities, 50);

    sem->key_concepts = (char**)malloc(50 * sizeof(char*));
    sem->key_concept_ids = (int*)malloc(50 * sizeof(int));
    for (int i = 0; i < 50; i++) sem->key_concept_ids[i] = -1;
    sem->key_concept_count = extract_key_concepts(sem, sem->key_concepts, 50);

    sem->causal_query = (sem->intent.intent == INTENT_EXPLAIN);
    sem->cause_node_id = -1;
    sem->effect_node_id = -1;

    // 如果是因果查询，尝试找出原因和结果概念
    // 策略：在分词中找到"为什么"后面的词作为原因，"导致"前面的词作为原因
    if (sem->causal_query) {
        // 查找"为什么"后面的概念（原因）
        const char* why_pos = strstr(text, "为什么");
        const char* cause_pos = NULL;
        const char* effect_pos = NULL;

        if (why_pos) {
            // 跳过"为什么"三个字
            cause_pos = why_pos + 6;  // "为什么"是3个UTF8字符，跳过

            // 查找"导致"或"会引起"
            const char* cause_keyword = strstr(cause_pos, "导致");
            if (cause_keyword) {
                effect_pos = cause_keyword + 6;  // "导致"是2个字符
            } else {
                cause_keyword = strstr(cause_pos, "会引起");
                if (cause_keyword) {
                    effect_pos = cause_keyword + 12;
                }
            }
        }

        // 如果没找到，使用关键词的第一个和第二个
        if (!cause_pos && !effect_pos && sem->key_concept_count >= 2) {
            // 假设第一个是关键原因，最后一个是关键结果
            sem->cause_node_id = sem->key_concept_ids[0];
            sem->effect_node_id = sem->key_concept_ids[sem->key_concept_count - 1];
        }

        LOG_DEBUG("  原因位置: %s", cause_pos ? cause_pos : "(使用关键词)");
        LOG_DEBUG("  结果位置: %s", effect_pos ? effect_pos : "(使用关键词)");
    }

    return sem;
}

/**
 * 释放语义理解结果
 */
void semantic_understanding_destroy(SemanticUnderstanding* sem) {
    if (!sem) return;

    free(sem->original_text);
    free(sem->intent.original_phrase);

    if (sem->tokens) {
        for (int i = 0; i < sem->token_count; i++) {
            free(sem->tokens[i]);
        }
        free(sem->tokens);
    }

    if (sem->entities) {
        for (int i = 0; i < sem->entity_count; i++) {
            free(sem->entities[i].text);
            free(sem->entities[i].normalized);
        }
        free(sem->entities);
    }

    if (sem->key_concepts) {
        for (int i = 0; i < sem->key_concept_count; i++) {
            free(sem->key_concepts[i]);
        }
        free(sem->key_concepts);
    }

    if (sem->key_concept_ids) {
        free(sem->key_concept_ids);
    }

    free(sem);
}

// ==================== 对话输入解析 ====================

DialogInput* dialog_parse_input(const char* text) {
    if (!text) return NULL;
    
#ifdef _WIN32
    static int console_initialized = 0;
    if (!console_initialized) {
        SetConsoleOutputCP(65001);
        SetConsoleCP(65001);
        console_initialized = 1;
    }
#endif
    
    DialogInput* input = (DialogInput*)calloc(1, sizeof(DialogInput));
    if (!input) return NULL;
    
    // 保存原始输入
    input->original = strdup(text);
    input->original_length = strlen(text);
    
    // UTF-8分词
    char* tokens_buf[PM_TOKEN_MAX];  // 临时缓冲区
    input->token_count = utf8_tokenize(text, tokens_buf, PM_TOKEN_MAX);
    
    if (input->token_count == 0) {
        input->token_count = 0;
        return input;
    }
    
    // 复制tokens到动态内存
    input->tokens = (char**)malloc(input->token_count * sizeof(char*));
    if (!input->tokens) { input->token_count = 0; return input; }
    for (int i = 0; i < input->token_count; i++) {
        input->tokens[i] = strdup(tokens_buf[i]);
    }
    
    return input;
}

void dialog_input_destroy(DialogInput* input) {
    if (!input) return;
    
    free(input->original);
    
    if (input->tokens) {
        for (int i = 0; i < input->token_count; i++) {
            free(input->tokens[i]);
        }
        free(input->tokens);
    }
    
    free(input);
}

// ==================== 对话推理 ====================

DialogReasoning* dialog_reason(DialogInput* input, MasterTopology* master,
                               const float* intent_weights) {
    if (!input || !master || input->token_count == 0) return NULL;
    
    master_decay_activations(master, 0.5f);
    
    DialogReasoning* reasoning = (DialogReasoning*)calloc(1, sizeof(DialogReasoning));
    if (!reasoning) return NULL;
    
    // 先收集所有找到/创建的节点
    ReasoningNode* found_nodes[PM_CONCEPT_MAX];
    int found_topo_ids[PM_CONCEPT_MAX];      // 记录每个节点所属拓扑
    int found_count = 0;
    
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->node_hash) continue;
        
        for (int i = 0; i < input->token_count; i++) {
            ReasoningNode* node = node_hash_find(sub->node_hash, input->tokens[i]);
            
            if (node) {
                float init_activation = 0.9f;
                node->activation = init_activation;
                
                dialog_add_association(reasoning, 
                    node->concept, init_activation, sub->type, 0,
                    node->node_id, -1);
                
                master_activate_node(master, sub->topo_id, node->node_id, init_activation);
                
                if (found_count < PM_CONCEPT_MAX) {
                    found_nodes[found_count] = node;
                    found_topo_ids[found_count] = t;   // 记录拓扑索引
                    found_count++;
                }
            } else {
                // 找不到节点？创建新节点 + 联网学习
                int new_id = huarong_net_dynamic_add_node(sub->net, input->tokens[i], NULL, 0);
                if (new_id >= 0 && sub->node_hash) {
                    ReasoningNode* new_node = sub->net->nodes[sub->net->node_count - 1];
                    new_node->confidence = 0.45f;
                    new_node->activation = 0.65f;
                    node_hash_add(sub->node_hash, new_node);
                    
                    dialog_add_association(reasoning, 
                        new_node->concept, 0.5f, sub->type, 0,
                        new_node->node_id, -1);
                    
                    if (found_count < PM_CONCEPT_MAX) {
                        found_nodes[found_count] = new_node;
                        found_topo_ids[found_count] = t;
                        found_count++;
                    }

                    /* 联网搜索学习：为不懂的概念查询百度百科 */
                    {
                        WebResult* wr = dialog_search_concept(new_node->concept);
                        if (wr && wr->keyword_count > 0) {
                            int learned = 0;
                            for (int k = 0; k < wr->keyword_count && learned < 4; k++) {
                                if (!wr->keywords[k] || strlen(wr->keywords[k]) < 2) continue;
                                int has_text = 0;
                                for (const char* cp = wr->keywords[k]; *cp; cp++)
                                    if ((unsigned char)*cp > 127 || isalpha((unsigned char)*cp))
                                        { has_text = 1; break; }
                                if (!has_text) continue;

                                ReasoningNode* exist = node_hash_find(sub->node_hash, wr->keywords[k]);
                                if (!exist) {
                                    ReasoningNode* kn = huarong_net_add_node(
                                        sub->net, wr->keywords[k], NULL, 0);
                                    if (kn) {
                                        kn->confidence = 0.35f;
                                        node_hash_add(sub->node_hash, kn);
                                        huarong_net_add_connection(sub->net,
                                            new_node->node_id, kn->node_id, 0.3f);
                                        learned++;
                                    }
                                } else if (exist != new_node) {
                                    int already = 0;
                                    for (int c = 0; c < new_node->edge_count; c++)
                                        if (new_node->edges[c].target == exist) { already=1; break; }
                                    if (!already)
                                        huarong_net_add_connection(sub->net,
                                            new_node->node_id, exist->node_id, 0.35f);
                                }
                            }
                            if (learned > 0) {
                                LOG_INFO("[对话学习] '%s' → 联网学习 %d 个关联概念",
                                        new_node->concept, learned);
                            }
                        }
                        web_result_free(wr);
                    }
                }
            }
        }
    }
    
    // 同一句话中出现的概念应该互相连接（像人脑的Hebbian学习）
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        
        // 收集这个子拓扑中的节点（用预存的拓扑索引避免O(n)扫描）
        ReasoningNode* sub_nodes[PM_CONCEPT_MAX];
        int sub_count = 0;
        
        for (int i = 0; i < found_count && sub_count < PM_CONCEPT_MAX; i++) {
            if (found_topo_ids[i] == t && found_nodes[i]) {
                sub_nodes[sub_count++] = found_nodes[i];
            }
        }
        
        // 在同一子拓扑的节点之间建立连接
        for (int i = 0; i < sub_count; i++) {
            for (int j = i + 1; j < sub_count; j++) {
                ReasoningNode* a = sub_nodes[i];
                ReasoningNode* b = sub_nodes[j];
                if (a && b && a != b) {
                    // 检查是否已有连接
                    int already_connected = 0;
                    for (int c = 0; c < a->edge_count; c++) {
                        if (a->edges[c].target == b) {
                            already_connected = 1;
                            break;
                        }
                    }
                    if (!already_connected) {
                        huarong_net_add_connection(sub->net, a->node_id, b->node_id, 0.7f);
                        huarong_net_add_connection(sub->net, b->node_id, a->node_id, 0.7f);
                    }
                }
            }
        }
    }
    
    // ===== 并行传播：每跳内子拓扑并行 =====
    for (int hop = 1; hop <= DEFAULT_HOP_COUNT; hop++) {
        // 1. 统计活跃子拓扑（动态分配，无上限）
        int* topo_ids = (int*)malloc((size_t)master->sub_topo_count * sizeof(int));
        if (!topo_ids) break;
        int active_topos = 0;
        for (int t = 0; t < master->sub_topo_count; t++) {
            SubTopology* sub = master->sub_topologies[t];
            if (!sub || !sub->net) continue;
            for (int n = 0; n < sub->net->node_count; n++) {
                ReasoningNode* node = sub->net->nodes[n];
                if (node && node->activation >= 0.15f && (hop <= 1 || !node->is_visited)) {
                    topo_ids[active_topos++] = t;
                    break;
                }
            }
        }
        if (active_topos == 0) break;

        // 2. 构建并行任务（动态分配，匹配 active_topos）
        pthread_mutex_t assoc_mutex = PTHREAD_MUTEX_INITIALIZER;
        int hop_propagated = 0;

        DialogTopoTask* tasks = (DialogTopoTask*)calloc((size_t)active_topos, sizeof(DialogTopoTask));
        ThreadTask* th_tasks = (ThreadTask*)calloc((size_t)active_topos, sizeof(ThreadTask));
        if (!tasks || !th_tasks) { free(tasks); free(th_tasks); free(topo_ids); break; }
        for (int i = 0; i < active_topos; i++) {
            tasks[i].master = master;
            tasks[i].reasoning = reasoning;
            tasks[i].topo_id = topo_ids[i];
            tasks[i].hop = hop;
            tasks[i].assoc_mutex = &assoc_mutex;
            tasks[i].hop_propagated = &hop_propagated;
            tasks[i].intent_weights = intent_weights;
            th_tasks[i].func = dialog_topo_worker;
            th_tasks[i].arg = &tasks[i];
        }

        // 3. 提交到线程池（拓扑间并行传播）
        ThreadPool* pool = master_get_thread_pool(master);
        if (pool && active_topos > 1) {
            thread_pool_batch(pool, th_tasks, active_topos);
        } else {
            // 单拓扑或线程池不可用：串行回退
            for (int i = 0; i < active_topos; i++)
                dialog_topo_worker(&tasks[i]);
        }

        free(tasks);
        free(th_tasks);

        if (hop_propagated == 0) { free(topo_ids); break; }
        free(topo_ids);
    }
    
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        for (int n = 0; n < sub->net->node_count; n++) {
            if (sub->net->nodes[n]) {
                sub->net->nodes[n]->is_visited = 0;
            }
        }
    }
    
    int topo_counts[9] = {0};
    for (int i = 0; i < reasoning->assoc_count; i++) {
        if (reasoning->associations[i].topo_type >= 0 && 
            reasoning->associations[i].topo_type <= 8) {
            topo_counts[reasoning->associations[i].topo_type]++;
        }
    }
    
    master_consolidate_confidence(master, 0.1f);
    
    batch_self_verify(master);
    
    return reasoning;
}

void dialog_add_association(DialogReasoning* reasoning, const char* concept,
                           float activation, int topo_type, int hop_count,
                           int node_id, int from_node_id) {
    if (!reasoning || !concept) return;
    if (reasoning->assoc_count >= MAX_ASSOCIATIONS) return;
    
    // 检查是否已存在，取最大激活值
    for (int i = 0; i < reasoning->assoc_count; i++) {
        if (strcmp(reasoning->associations[i].concept, concept) == 0) {
            if (activation > reasoning->associations[i].activation) {
                reasoning->associations[i].activation = activation;
                reasoning->associations[i].hop_count = hop_count;
                reasoning->associations[i].node_id = node_id;
                reasoning->associations[i].from_node_id = from_node_id;
            }
            return;
        }
    }
    
    // 新增联想
    strncpy(reasoning->associations[reasoning->assoc_count].concept, 
            concept, 255);
    reasoning->associations[reasoning->assoc_count].concept[255] = '\0';
    reasoning->associations[reasoning->assoc_count].activation = activation;
    reasoning->associations[reasoning->assoc_count].topo_type = topo_type;
    reasoning->associations[reasoning->assoc_count].hop_count = hop_count;
    reasoning->associations[reasoning->assoc_count].node_id = node_id;
    reasoning->associations[reasoning->assoc_count].from_node_id = from_node_id;
    reasoning->assoc_count++;
}

void dialog_reasoning_destroy(DialogReasoning* reasoning) {
    if (!reasoning) return;
    free(reasoning);
}

// ==================== 回复生成（实现在 dialog_generate.c） ====================
// dialog_generate() 已统一定义在 src/dialog_generate.c 中，
// 包含走边路径生成、因果筛选、内感受评估等完整管线。

// ==================== 自我验证机制 ====================

// SelfVerificationResult defined in dialog_system.h

// 检查两个概念是否矛盾

// 自我验证：检查知识一致性
SelfVerificationResult self_verify_knowledge(DialogReasoning* reasoning, MemorySystem* memory) {
    SelfVerificationResult result = {0};
    result.confidence = 0.5f; // 默认置信度
    
    if (!reasoning || reasoning->assoc_count == 0) {
        result.confidence = 0.1f;
        snprintf(result.suggestion, sizeof(result.suggestion), "知识不足，需要学习");
        return result;
    }
    
    // 检查关联知识的激活强度
    float total_activation = 0.0f;
    
    // 检查关联知识的激活强度
    int high_conf_count = 0;
    
    for (int i = 0; i < reasoning->assoc_count; i++) {
        total_activation += reasoning->associations[i].activation;
        if (reasoning->associations[i].activation >= 0.5f) {  // 改为 >= 
            high_conf_count++;
        }
    }
    
    // 检查是否有足够的强关联
    if (high_conf_count >= 3) {
        result.confidence = 0.8f;
        result.is_consistent = 1;
        snprintf(result.suggestion, sizeof(result.suggestion), "知识充足且一致");
    } else if (high_conf_count >= 1) {
        result.confidence = 0.5f;
        result.is_consistent = 1;
        snprintf(result.suggestion, sizeof(result.suggestion), "知识基本一致，但可以更丰富");
    } else {
        result.confidence = 0.2f;
        result.is_consistent = 0;
        snprintf(result.suggestion, sizeof(result.suggestion), "知识薄弱，需要更多学习");
    }
    
    // 检查记忆系统中的冲突
    if (memory) {
        // 简单检查：看是否有多个不同的答案
        // (实际实现会更复杂)
    }
    
    return result;
}

// ==================== 预测误差反馈环 ====================

/**
 * 计算预测误差：对比预期延续字符与实际输入的字符重合度
 * 越重合→误差越低，越不相关→误差越高
 */
float compute_prediction_error(DialogSystem* sys, const char* actual_input) {
    if (!sys->has_last_turn || !actual_input || !sys->last_response[0])
        return 0.5f;  // 中立值，无惩罚无奖励

    // 从最后回复的激活节点走边，生成预期延续
    char predicted[MAX_RESPONSE_LENGTH] = {0};
    int pred_pos = 0;

    for (int i = 0; i < sys->last_path_count && pred_pos < 200; i++) {
        int topo_type = sys->last_path_topo_types[i];
        int node_id = sys->last_path_node_ids[i];
        SubTopology* sub = master_get_sub_topology_by_type(sys->master, topo_type);
        if (!sub || !sub->net || node_id < 0 || node_id >= sub->net->node_count)
            continue;
        ReasoningNode* node = sub->net->nodes[node_id];
        if (node && node->concept) {
            pred_pos += snprintf(predicted + pred_pos, sizeof(predicted) - pred_pos, "%s", node->concept);
            if (pred_pos >= 200) break;
            // 从该节点走一步，看下一步会到哪个节点
            float best_w = 0;
            int best_next = -1;
            for (int e = 0; e < node->edge_count; e++) {
                if (node->edges[e].target && node->edges[e].weight > best_w) {
                    best_w = node->edges[e].weight;
                    best_next = node->edges[e].target->node_id;
                }
            }
            if (best_next >= 0 && best_next < sub->net->node_count && sub->net->nodes[best_next]) {
                ReasoningNode* next = sub->net->nodes[best_next];
                if (next && next->concept && next->node_id != node_id) {
                    pred_pos += snprintf(predicted + pred_pos, sizeof(predicted) - pred_pos, "%s", next->concept);
                }
            }
        }
    }

    if (pred_pos == 0) return 0.5f;

    // 计算字符重合度：实际输入中有多少字符在预期延续中出现
    // 使用UTF-8感知的中文字符匹配
    int match_count = 0;
    int total_chars = 0;
    const char* p = actual_input;
    while (*p && total_chars < 50) {
        int len = 1;
        unsigned char c = (unsigned char)*p;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        // 只比较中文字符（3字节）
        if (len == 3) {
            char ch[4] = {0};
            strncpy(ch, p, 3);
            if (strstr(predicted, ch)) {
                match_count++;
            }
            total_chars++;
        }
        p += len;
    }

    if (total_chars == 0) return 0.5f;
    float overlap = (float)match_count / (float)total_chars;
    // 误差 = 1 - 重合度，但截断到 [0, 1]
    float error = 1.0f - overlap;
    if (error < 0.0f) error = 0.0f;
    if (error > 1.0f) error = 1.0f;
    return error;
}

/**
 * 根据预测误差对上一轮的路径施加反馈
 * 信用分配：越靠近输出端的边，调整幅度越大（线性衰减）
 */
void apply_prediction_feedback(DialogSystem* sys, float error) {
    if (!sys->has_last_turn || sys->last_path_count <= 0) return;
    if (!sys->master) return;

    // 自适应学习率：连续失败则提高学习率（想学新东西）
    //              连续成功则降低学习率（趋于稳定）
    if (error > 0.5f) {
        sys->consecutive_failures++;
        sys->consecutive_success = 0;
        // 连续失败：拉高学习率和好奇心
        if (sys->consecutive_failures >= 3) {
            sys->prediction_lr = 0.15f;
            sys->curiosity = 0.4f;
        }
    } else {
        sys->consecutive_success++;
        sys->consecutive_failures = 0;
        // 连续成功：学习率衰减，趋向稳定
        if (sys->consecutive_success >= 5) {
            sys->prediction_lr *= 0.95f;
            if (sys->prediction_lr < 0.01f) sys->prediction_lr = 0.01f;
        }
        // 好奇心在稳定期逐渐降低
        if (sys->consecutive_success >= 3) {
            sys->curiosity *= 0.98f;
            if (sys->curiosity < 0.05f) sys->curiosity = 0.05f;
        }
        // 遇到连续失败时临时拉高好奇心
        if (sys->consecutive_failures >= 2) {
            sys->curiosity = 0.3f;
        }
    }

    // 对路径上的每条边施加调整
    // 信用分配：靠近输出的边（路径后半段）调整幅度更大
    for (int i = 0; i < sys->last_path_count; i++) {
        int topo_type = sys->last_path_topo_types[i];
        int node_id = sys->last_path_node_ids[i];
        int edge_id = sys->last_path_edge_ids[i];

        SubTopology* sub = master_get_sub_topology_by_type(sys->master, topo_type);
        if (!sub || !sub->net || node_id < 0 || node_id >= sub->net->node_count)
            continue;

        ReasoningNode* node = sub->net->nodes[node_id];
        if (!node || edge_id < 0 || edge_id >= node->edge_count)
            continue;

        // 信用权重: 越靠近输出端, 权重越大
        float credit_weight = 0.3f + 0.7f * ((float)i / (float)sys->last_path_count);
        float adjustment = sys->prediction_lr * credit_weight;

        if (error <= 0.4f) {
            // 预测准确 → 奖励：涨置信度
            float bonus = adjustment * (1.0f - error);  // 误差越小奖励越大
            node->edges[edge_id].confidence += bonus;
            if (node->edges[edge_id].confidence > 1.0f)
                node->edges[edge_id].confidence = 1.0f;
        } else {
            // 预测错误 → 惩罚：降置信度
            float penalty = adjustment * error;  // 误差越大惩罚越大
            node->edges[edge_id].confidence -= penalty;
            if (node->edges[edge_id].confidence < 0.1f)
                node->edges[edge_id].confidence = 0.1f;  // 保留最低值，不打死
        }
    }

    // 误差 > 0.7 时额外削弱整条路径的权重一致性
    // 这是"信用弥散"——整条路径一起降，不只是边缘
    if (error > 0.7f) {
        for (int i = 0; i < sys->last_path_count; i++) {
            int topo_type = sys->last_path_topo_types[i];
            int node_id = sys->last_path_node_ids[i];
            SubTopology* sub = master_get_sub_topology_by_type(sys->master, topo_type);
            if (!sub || !sub->net || node_id < 0 || node_id >= sub->net->node_count)
                continue;
            ReasoningNode* node = sub->net->nodes[node_id];
            if (!node) continue;
            // 整条路径节点的置信度微降
            node->confidence -= 0.02f;
            if (node->confidence < 0.3f) node->confidence = 0.3f;
        }
    }

    // 如果连续失败太多次（>5），重置学习曲线
    if (sys->consecutive_failures > 5) {
        sys->prediction_lr = 0.1f;
        sys->curiosity = 0.5f;
        sys->consecutive_failures = 5;  // 卡住上限
    }
}

// ==================== 主对话流程 ====================

DialogSystem* dialog_system_create(MasterTopology* master, MemorySystem* memory,
                                CausalGraph* causal_graph, ActiveLearner* learner) {
    DialogSystem* sys = (DialogSystem*)calloc(1, sizeof(DialogSystem));
    if (!sys) return NULL;

    sys->master = master;
    sys->memory = memory;
    sys->causal_graph = causal_graph;
    sys->learner = learner;
    sys->concept_hierarchy = concept_hierarchy_create(500);
    sys->str_pool = string_pool_create(500);
    sys->session_id = time(NULL);
    sys->turn_count = 0;
    sys->max_hop_count = 3;
    sys->activation_threshold = 0.3f;
    sys->decay_rate = 0.7f;

    sys->cognitive_state = cognitive_state_create();
    if (sys->cognitive_state) {
        cognitive_state_init(sys->cognitive_state);
        LOG_INFO("[对话系统] 认知状态（情感/动机系统）: 已就绪");
    }

    LOG_INFO("[对话系统] 创建成功，会话ID: %ld", sys->session_id);
    LOG_INFO("[对话系统] 因果图: %s", causal_graph ? "已连接" : "未连接");
    // 初始化认知调度中心
    sys->controller = cognitive_controller_create(sys->master, sys->memory);
    if (sys->controller) {
        // 注入因果图和概念层次（用于输出约束评估）
        sys->controller->causal_graph = sys->causal_graph;
        sys->controller->concept_hierarchy = sys->concept_hierarchy;
        LOG_INFO("[对话系统] 认知调度中心: 已就绪（语义-因果约束已注入）");
    } else {
        LOG_INFO("[对话系统] 认知调度中心: 创建失败");
    }

    // 初始化句式拓扑（16种固定句式 + POS scaffolding）
    if (sys->controller) {
        cc_init_sentence_topology(sys->controller);
        LOG_INFO("[对话系统] 句式拓扑: 已就绪（16种基础句式）");
    }

    // 初始化 BPTT 学习器（RNN 在线反向传播）
#define BPTT_HIDDEN_DIM 256
    sys->bptt = bptt_learner_create(sys->master, BPTT_HIDDEN_DIM, 32);
    if (sys->bptt) {
        LOG_INFO("[对话系统] BPTT 学习器: 已就绪（RNN %d→%d→%d, Adam）",
                  NODE_FEATURE_DIM, BPTT_HIDDEN_DIM, NODE_FEATURE_DIM);
    }

    // 初始化预测误差反馈环
    sys->has_last_turn = 0;
    sys->last_path_count = 0;
    sys->last_input[0] = '\0';
    sys->last_response[0] = '\0';
    sys->prediction_lr = 0.1f;          // 初始学习率
    sys->curiosity = 0.3f;              // 初始好奇心
    sys->consecutive_success = 0;
    sys->consecutive_failures = 0;

    /* 初始化自主学习刷盘状态 */
    {
        AutonomicState* as = (AutonomicState*)calloc(1, sizeof(AutonomicState));
        if (as) {
            autonomic_state_init(as);
            as->flush_threshold = 10000;  // 对话中积累1万次更新才刷盘（避免频繁I/O）
            sys->auto_state = as;
        }
    }

    return sys;
}

void dialog_system_destroy(DialogSystem* sys) {
    if (!sys) return;
    LOG_INFO("[对话系统] 销毁，会话ID: %ld, 对话轮数: %d", 
           sys->session_id, sys->turn_count);
    if (sys->concept_hierarchy) {
        concept_hierarchy_destroy((ConceptHierarchy*)sys->concept_hierarchy);
    }
    if (sys->str_pool) {
        string_pool_destroy((StringPool*)sys->str_pool);
    }
    if (sys->cognitive_state) {
        cognitive_state_destroy(sys->cognitive_state);
    }
    if (sys->controller) {
        cognitive_controller_destroy(sys->controller);
    }
    if (sys->bptt) {
        float avg_loss;
        int steps;
        bptt_learner_stats(sys->bptt, &avg_loss, &steps);
        if (steps > 0)
            LOG_INFO("[对话系统] BPTT 统计: %d 步, avg_loss=%.6f", steps, avg_loss);
        bptt_learner_destroy(sys->bptt);
    }
    if (sys->auto_state) {
        free(sys->auto_state);
    }
    free(sys);
}

#define CTX_MAX_INSTANCES  16    /* 上下文拓扑最大实例节点数 */
#define CTX_EVICT_HEAT     0.02f /* 实例节点热度低于此值可被淘汰 */

/** 意图 → 上下文种子节点名映射 */
static const char* context_seed_for_intent(DialogIntent intent) {
    switch (intent) {
        case INTENT_QUERY:   return "Q&A";
        case INTENT_EXPLAIN: return "EXPLAIN";
        case INTENT_HOWTO:   return "HOWTO";
        case INTENT_CHAT:    return "CHAT";
        default:             return "CHAT";
    }
}

/** 对话中联网搜索概念：超短超时（1秒），不阻塞对话 */
static WebResult* dialog_search_concept(const char* concept) {
    if (!concept || !concept[0]) return NULL;
    char url[1024];
    snprintf(url, sizeof(url), "https://baike.baidu.com/item/%s", concept);
    return web_search(url, 1000, 32768);  /* 1秒超时，不阻塞对话 */
}

/**
 * 上下文实例节点管理 - 话题追踪 + 整理
 *
 * 设计：
 *   - 种子节点（4个固定）：Q&A/EXPLAIN/CHAT/HOWTO - 类型锚点
 *   - 实例节点（最大16个）：每话题创建一个，命名 "TYPE#N|话题摘要"
 *   - 实例→种子：双向连接（属于该类型）
 *   - 实例→实例：时间线串联
 *   - 整理策略：LRU淘汰 + 热度衰减 + 同话题复用
 */
static void dialog_activate_context(MasterTopology* master, DialogIntent intent) {
    SubTopology* ctx = master_get_sub_topology_by_type(master, TOPO_CONTEXT);
    if (!ctx || !ctx->net || !ctx->node_hash) return;

    const char* type_name = context_seed_for_intent(intent);

    /* 1. 确保种子节点存在 */
    ReasoningNode* seed = node_hash_find(ctx->node_hash, type_name);
    if (!seed) {
        seed = huarong_net_add_node(ctx->net, type_name, NULL, 0);
        if (seed) { seed->confidence = 0.7f; node_hash_add(ctx->node_hash, seed); }
    }
    if (!seed) return;
    seed->activation = 0.6f;
    seed->heat = 1.0f;  /* 种子节点热度永不清除 */

    /* 2. 提取当前话题摘要（词汇拓扑 Top2 高激活概念） */
    char topic[128] = "";
    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    if (vocab && vocab->net) {
        int found = 0, pos = 0;
        for (int i = 0; i < vocab->net->node_count && found < 2; i++) {
            ReasoningNode* vn = vocab->net->nodes[i];
            if (!vn || !vn->concept || vn->activation < 0.5f) continue;
            if (found > 0 && pos < (int)sizeof(topic) - 1) topic[pos++] = ',';
            int n = snprintf(topic + pos, sizeof(topic) - pos, "%s", vn->concept);
            if (n > 0) pos += n;
            found++;
        }
    }
    if (!topic[0]) snprintf(topic, sizeof(topic), "?");

    /* 3. 查找是否已有同类型 + 同话题的实例节点（复用） */
    ReasoningNode* inst = NULL;
    for (int i = 0; i < ctx->net->node_count; i++) {
        ReasoningNode* n = ctx->net->nodes[i];
        if (!n || !n->concept || n == seed) continue;
        /* 同类型且话题摘要匹配 → 复用 */
        if (strstr(n->concept, type_name) && strstr(n->concept, topic)) {
            inst = n;
            break;
        }
    }

    /* 4. 无匹配实例 → 创建新实例节点 */
    if (!inst) {
        /* 先检查容量：超过上限则淘汰最冷实例 */
        int inst_count = 0;
        for (int i = 0; i < ctx->net->node_count; i++) {
            ReasoningNode* n = ctx->net->nodes[i];
            if (n && n->concept && n != seed && strchr(n->concept, '#')
                && n->heat > 0.0f) inst_count++;  /* 不计数已淘汰的死节点 */
        }
        if (inst_count >= CTX_MAX_INSTANCES) {
            /* LRU: 淘汰热度最低的实例节点 */
            int evict_id = -1;
            float min_heat = 2.0f;
            for (int i = 0; i < ctx->net->node_count; i++) {
                ReasoningNode* n = ctx->net->nodes[i];
                if (!n || !n->concept || n == seed || !strchr(n->concept, '#')
                    || n->heat <= 0.0f) continue;  /* 跳过已淘汰的死节点 */
                if (n->heat < min_heat) { min_heat = n->heat; evict_id = n->node_id; }
            }
            if (evict_id >= 0 && min_heat < CTX_EVICT_HEAT) {
                ReasoningNode* evict = ctx->net->nodes[evict_id];
                if (evict && evict->concept) {
                    node_hash_remove(ctx->node_hash, evict->concept);
                    evict->activation = 0.0f;
                    evict->heat = 0.0f;
                }
            }
        }

        /* 创建实例节点名 */
        static int ctx_round = 0;
        ctx_round++;
        char name[256];
        snprintf(name, sizeof(name), "%s#%d|%s", type_name, ctx_round, topic);
        inst = huarong_net_add_node(ctx->net, name, NULL, 0);
        if (!inst) return;
        inst->confidence = 0.5f;
        node_hash_add(ctx->node_hash, inst);

        /* 实例→种子：双向连接 */
        int already_seed = 0;
        for (int c = 0; c < inst->edge_count; c++)
            if (inst->edges[c].target == seed) { already_seed = 1; break; }
        if (!already_seed) {
            huarong_net_add_connection(ctx->net, seed->node_id, inst->node_id, 0.7f);
            huarong_net_add_connection(ctx->net, inst->node_id, seed->node_id, 0.7f);
        }

        /* 时间线串联：上一实例 → 当前实例 */
        if (master->_legacy_context_node >= 0 &&
            master->_legacy_context_node < ctx->net->node_count &&
            master->_legacy_context_node != inst->node_id) {
            ReasoningNode* prev = ctx->net->nodes[master->_legacy_context_node];
            if (prev && prev != seed && prev->heat > 0.0f) {
                int already = 0;
                for (int c = 0; c < inst->edge_count; c++)
                    if (inst->edges[c].target == prev) { already = 1; break; }
                if (!already) {
                    huarong_net_add_connection(ctx->net, prev->node_id, inst->node_id, 0.5f);
                    huarong_net_add_connection(ctx->net, inst->node_id, prev->node_id, 0.5f);
                }
            }
        }
    }

    /* 5. 激活实例节点 */
    inst->activation = 0.85f;
    inst->heat = (inst->heat < 0.05f) ? 0.5f : fminf(1.0f, inst->heat + 0.1f);
    inst->selection_count++;
    master_activate_node(master, ctx->topo_id, inst->node_id, 0.85f);
    master->_legacy_context_node = inst->node_id;

    /* 6. 跨拓扑连接：实例 → 词汇（上限5） */
    if (vocab && vocab->net && master->cross_link_count < 50000) {
        int linked = 0;
        for (int i = 0; i < vocab->net->node_count && linked < 5; i++) {
            ReasoningNode* vn = vocab->net->nodes[i];
            if (!vn || vn->activation < 0.3f) continue;
            if (!cross_link_exists(master, ctx->topo_id, inst->node_id,
                                   vocab->topo_id, vn->node_id)) {
                master_add_cross_link(master, ctx->topo_id, inst->node_id,
                                     vocab->topo_id, vn->node_id, 0.45f, "ctx-vocab");
                linked++;
            }
        }
    }

    /* 7. 跨拓扑连接：实例 → 语义（上限3） */
    SubTopology* semantic = master_get_sub_topology_by_type(master, TOPO_SEMANTIC);
    if (semantic && semantic->net && master->cross_link_count < 50000) {
        int linked = 0;
        for (int i = 0; i < semantic->net->node_count && linked < 3; i++) {
            ReasoningNode* sn = semantic->net->nodes[i];
            if (!sn || sn->activation < 0.2f) continue;
            if (!cross_link_exists(master, ctx->topo_id, inst->node_id,
                                   semantic->topo_id, sn->node_id)) {
                master_add_cross_link(master, ctx->topo_id, inst->node_id,
                                     semantic->topo_id, sn->node_id, 0.40f, "ctx-semantic");
                linked++;
            }
        }
    }

    /* 8. 全局衰减：所有实例节点热度衰减（模拟遗忘） */
    for (int i = 0; i < ctx->net->node_count; i++) {
        ReasoningNode* n = ctx->net->nodes[i];
        if (!n || n == seed || !n->concept || !strchr(n->concept, '#')) continue;
        n->heat *= 0.92f;
        if (n->activation > 0.01f) n->activation *= 0.85f;
    }
}



// ==================== 对话系统核心 ====================

char* dialog_process(DialogSystem* sys, const char* user_input, DialogReasoning** out_reasoning) {
    if (!sys || !user_input) return strdup("系统错误...");

    if (out_reasoning) *out_reasoning = NULL;
    
    sys->turn_count++;

    // ===== 预测误差反馈环：利用上轮信息对比当前输入 =====
    if (sys->has_last_turn && sys->master) {
        float prediction_error = compute_prediction_error(sys, user_input);
        apply_prediction_feedback(sys, prediction_error);
        LOG_INFO("[反馈] 预测误差=%.3f  lr=%.3f  好奇心=%.3f",
               prediction_error, sys->prediction_lr, sys->curiosity);
    }

    ui_print_user_input(user_input);
    ui_print_thinking_start();

    char* response = NULL;

    ui_print_thinking_line("理解", "正在分析用户输入...");
    
    SemanticUnderstanding* sem = semantic_understand(user_input);

    if (!sem) {
        response = strdup("语义理解失败，请再说一次？");
        ui_print_thinking_line("错误", "语义理解失败");
        ui_print_thinking_end();
        return response;
    }

    char intent_str[64] = {0};
    const char* intent_name[] = {"查询", "定义", "解释", "比较", "如何做", "闲聊", "学习", "测试"};
    if (sem->intent.intent >= 0 && sem->intent.intent < 8) {
        snprintf(intent_str, sizeof(intent_str), "%s (置信度: %.2f)", 
                 intent_name[sem->intent.intent], sem->intent.confidence);
    }
    ui_print_thinking_line("意图", intent_str);

    char tokens_info[256] = {0};
    int pos = 0;
    for (int i = 0; i < sem->token_count && i < 5; i++) {
        pos += snprintf(tokens_info + pos, sizeof(tokens_info) - pos, "%s ", sem->tokens[i]);
    }
    if (sem->token_count > 5) {
        snprintf(tokens_info + pos, sizeof(tokens_info) - pos, "...(+%d)", sem->token_count - 5);
    }
    ui_print_thinking_line("分词", tokens_info);

    if (sem->entity_count > 0) {
        char entities_info[256] = {0};
        pos = 0;
        for (int i = 0; i < sem->entity_count && i < 3; i++) {
            pos += snprintf(entities_info + pos, sizeof(entities_info) - pos, "%s ", sem->entities[i].text);
        }
        ui_print_thinking_line("实体", entities_info);
    }

    if (sem->causal_query) {
        ui_print_thinking_line("因果", "检测到因果查询，正在构建因果图...");
        
        // 激活因果推理：解析因果查询
        if (sys->master) {
            resolve_causal_query(sem, sys->master);
            
            // 运行 A* 最强路径因果联想搜索
            int causal_count = 0;
            CausalSearchResult* causal_results = causal_associative_search(
                sys->master, user_input, 5, 10, &causal_count);
            
            if (causal_results && causal_count > 0) {
                char causal_info[512] = {0};
                int cpos = 0;
                for (int i = 0; i < causal_count && i < 3; i++) {
                    cpos += snprintf(causal_info + cpos, sizeof(causal_info) - cpos,
                                    "[%.2f] ", causal_results[i].total_strength);
                }
                if (causal_count > 3) {
                    snprintf(causal_info + cpos, sizeof(causal_info) - cpos,
                            "...(+%d条)", causal_count - 3);
                }
                ui_print_thinking_line("因果链", causal_info);

                /* 存入记忆系统：因果搜索结果不再只打印就丢掉 */
                if (sys->memory) {
                    for (int i = 0; i < causal_count && i < 3; i++) {
                        char cause_key[256], effect_key[256];
                        snprintf(cause_key, 255, "%s_cause_%d", user_input, i);
                        snprintf(effect_key, 255, "%.2f", causal_results[i].total_strength);
                        memory_store_causal_rule(sys->memory, cause_key,
                            effect_key, causal_results[i].total_strength, NULL);
                    }
                }
            }
            causal_search_results_free(causal_results, causal_count);
        }
    }

    // 概念处理：检测数学表达式
    if (concept_is_math_expression(user_input) || concept_is_number(user_input)) {
        ui_print_thinking_line("概念", "检测到数学表达式");
        char* calc_result = concept_calculate(user_input);
        if (calc_result) {
            response = malloc(256);
            if (!response) {
                free(calc_result);
                semantic_understanding_destroy(sem);
                ui_print_thinking_end();
                return strdup("系统错误：内存不足");
            }
            snprintf(response, 256, "计算结果是: %s", calc_result);
            ui_print_thinking_line("计算", calc_result);
            free(calc_result);
            semantic_understanding_destroy(sem);
            ui_print_thinking_end();
            ui_print_ai_response(response);
            fflush(stdout);
            // 学习这条规则
            concept_learn_rule(sys->master, user_input, response);
            return response;
        }
    }

    // 概念解析：增强实体类型识别
    ConceptValue* cv = concept_parse(user_input);
    if (cv && cv->type == CONCEPT_TYPE_CAUSAL) {
        sem->causal_query = 1;
        ui_print_thinking_line("概念", "检测到因果关系");
    } else if (cv && cv->type == CONCEPT_TYPE_RULE) {
        ui_print_thinking_line("概念", "检测到规则定义");
    }
    concept_value_free(cv);

    ui_print_thinking_line("推理", "搜索关联概念...");

    DialogInput* input = dialog_parse_input(user_input);
    DialogReasoning* reasoning = NULL;
    
    // ===== 认知调度：计算意图向量（输出到会话局部 InferenceContext） =====
    const float* intent_ptr = NULL;
    InferenceContext ctx = {0};
    if (sys->controller) {
        cognitive_controller_reset_round(sys->controller);
        cognitive_controller_set_context(sys->controller, user_input, NULL);
        // 注入语义意图类型（从 semantic_understand 的结果）
        if (sem && sem->intent.intent >= 0) {
            cognitive_controller_set_intent(sys->controller, sem->intent.intent);
            LOG_INFO("[认知调度] 语义意图: %d (置信度: %.2f)",
                   sem->intent.intent, sem->intent.confidence);
            /* 意图结果写入记忆（用于自适应调整调度器策略） */
            if (sys->controller->memory) {
                char intent_key[128];
                snprintf(intent_key, 127, "last_intent:%s", user_input);
                memory_store(sys->controller->memory, intent_key,
                    strdup((char*)intent_name[sem->intent.intent]),
                    strlen(intent_name[sem->intent.intent])+1, MEMORY_TYPE_STRING, sem->intent.confidence);
            }
        }
        compute_intent_local(sys->controller, NULL, ctx.intent_weights);
        intent_ptr = ctx.intent_weights;
        ui_print_thinking_line("调度", "认知调度已激活");
    }
    
    if (input && input->token_count > 0) {
        reasoning = dialog_reason(input, sys->master, intent_ptr);
        if (!reasoning) {
            response = strdup("推理失败...");
            dialog_input_destroy(input);
        } else {
            // ===== 认知调度：retry 循环 =====
            int done = 0;
            response = NULL;

            if (reasoning->assoc_count > 0) {
                char assoc_info[128] = {0};
                snprintf(assoc_info, sizeof(assoc_info), "找到 %d 个关联", reasoning->assoc_count);
                ui_print_thinking_line("联想", assoc_info);
            }

            while (!done) {
                // --- 自我验证：检查知识是否足够 ---
                float knowledge_quality = 0.0f;
                if (reasoning->assoc_count > 0) {
                    float total_conf = 0.0f;
                    int count = 0;
                    for (int i = 0; i < reasoning->assoc_count && i < 10; i++) {
                        total_conf += reasoning->associations[i].activation;
                        count++;
                    }
                    knowledge_quality = count > 0 ? total_conf / count : 0.0f;
                }
                reasoning->knowledge_quality = knowledge_quality;
                sys->last_knowledge_quality = knowledge_quality;

                SelfVerificationResult verify = self_verify_knowledge(reasoning, sys->memory);
                reasoning->is_verified = 1;
                if (verify.confidence < 0.4f) {
                    ui_print_thinking_line("自检", verify.suggestion);
                }

                // --- 生成回复 ---
                if (response) {
                    free(response);
                    response = NULL;
                }

                /* Phase 3: BPTT RNN 预激活词汇节点 — 偏置生成分布 */
                if (sys->bptt && sys->master) {
                    int biased = bptt_bias_vocab_activation(sys->bptt, user_input);
                    if (biased > 0 && sys->turn_count <= 5) {
                        LOG_INFO("[BPTT] RNN 预激活了 %d 个词汇节点", biased);
                    }
                }

                LOG_DEBUG("[proc] calling dialog_generate...");
                response = dialog_generate(reasoning, user_input, sys->memory,
                                        MAX_RESPONSE_LENGTH, sys);
                LOG_DEBUG("[proc] dialog_generate returned: %s", response ? response : "NULL");

                // --- 如果没有认知调度器，直接跳出循环 ---
                if (!sys->controller) {
                    done = 1;
                    break;
                }

                // --- 构建草案用于内感受评估 ---
                // 修复：evaluate_draft 应该评估 dialog_generate() 实际产出的
                // walk 路径（dsys->last_path_*），而非 associations 列表。
                // associations 是激活传播的关联概念，walk 才是最终输出路径。
                PathResult draft;
                memset(&draft, 0, sizeof(draft));
                
                /* 安全兜底：last_path_count 可能未正确设置 */
                int use_last_path = (sys->last_path_count > 0 && sys->has_last_turn
                    && sys->last_path_topo_types[0] >= 0
                    && sys->last_path_topo_types[0] < sys->master->sub_topo_count);
                
                if (use_last_path) {
                    // 使用实际 walk 路径（dialog_generate 存入 sys->last_path_*）
                    draft.topo_id = (sys->last_path_count > 0)
                                    ? sys->last_path_topo_types[0] : 0;
                    draft.length = (sys->last_path_count < MAX_PATH_LENGTH)
                                   ? sys->last_path_count : MAX_PATH_LENGTH;
                    draft.act_sum = 0.0f;
                    draft.conf_sum = 0.0f;
                    SubTopology* eval_sub = (draft.topo_id >= 0 && draft.topo_id < sys->master->sub_topo_count)
                        ? sys->master->sub_topologies[draft.topo_id] : NULL;
                    for (int i = 0; i < draft.length; i++) {
                        draft.node_ids[i] = sys->last_path_node_ids[i];
                        if (eval_sub && eval_sub->net && 
                            sys->last_path_node_ids[i] >= 0 &&
                            sys->last_path_node_ids[i] < eval_sub->net->node_count) {
                            ReasoningNode* n = eval_sub->net->nodes[sys->last_path_node_ids[i]];
                            if (n) {
                                draft.act_sum += n->activation;
                                draft.conf_sum += n->confidence;
                            }
                        }
                    }
                    draft.score = (draft.length > 0) ? draft.act_sum / draft.length : 0.0f;
                } else {
                    // 回退：无 walk 路径时用 associations
                    draft.topo_id = (reasoning->assoc_count > 0)
                                    ? reasoning->associations[0].topo_type : 0;
                    draft.length = (reasoning->assoc_count < MAX_PATH_LENGTH)
                                   ? reasoning->assoc_count : MAX_PATH_LENGTH;
                    for (int i = 0; i < draft.length; i++) {
                        draft.node_ids[i] = reasoning->associations[i].node_id;
                        draft.act_sum += reasoning->associations[i].activation;
                    }
                    draft.score = (draft.length > 0) ? draft.act_sum / draft.length : 0.0f;
                }

                // --- 内感受评估 ---
                float satisfaction = evaluate_draft(sys->controller, &draft, draft.length);
                
                RetryStatus retry_status = revise_and_retry(sys->controller, &draft, satisfaction);
                
                LOG_DEBUG("[proc] retry_status=%d satisfaction=%.2f", retry_status, satisfaction);

                if (retry_status == RETRY_OK) {
                    done = 1;
                    break;
                }

                if (retry_status == RETRY_FAILED) {
                    if (sys->controller->retry_count >= sys->controller->max_retry) {
                        ui_print_thinking_line("调度", "已达修正上限，强制输出");
                    }
                    done = 1;
                    break;
                }

                // --- RETRY_FROM_POOL: 从当前推理结果重排（不重搜） ---
                if (retry_status == RETRY_FROM_POOL) {
                    ui_print_thinking_line("调度", "修正: 候选池重排");
                    // 当前推理结果不变，仅用新意图权重重新生成回复
                    // （未来接入束搜索候选池后，此处改为池内重选）
                    continue;
                }

                // --- RETRY_WITH_SEARCH: 缩域重搜 ---
                if (retry_status == RETRY_WITH_SEARCH) {
                    ui_print_thinking_line("调度", "修正: 缩域重搜");
                    // 释放旧推理，用新意图权重重新搜索
                    if (response) {
                        free(response);
                        response = NULL;
                    }
                    dialog_reasoning_destroy(reasoning);
                    reasoning = dialog_reason(input, sys->master,
                                             sys->controller->intent_weights);
                    if (!reasoning) {
                        response = strdup("重搜失败...");
                        done = 1;
                        break;
                    }

                    // 重新记录关联信息（仅日志，非首次不重打推理链）
                    if (reasoning->assoc_count > 0) {
                        char assoc_info[128] = {0};
                        snprintf(assoc_info, sizeof(assoc_info), "重搜: %d 个关联",
                                 reasoning->assoc_count);
                        ui_print_thinking_line("联想", assoc_info);
                    }
                    continue;  // 回到循环顶部重新生成
                }
            }  // while (!done)

            // 在线学习：推理成功完成后，活跃拓扑的基准权重提升
            if (reasoning && reasoning->path_depth > 0 && sys->controller) {
                int used_topos[MAX_SUBTOPOS];
                int topo_set[MAX_SUBTOPOS];
                memset(topo_set, 0, sizeof(topo_set));
                int unique_count = 0;
                for (int i = 0; i < reasoning->path_depth && i < MAX_ASSOCIATIONS; i++) {
                    int t = reasoning->path_topos[i];
                    if (t >= 0 && t < MAX_SUBTOPOS && !topo_set[t]) {
                        topo_set[t] = 1;
                        used_topos[unique_count++] = t;
                    }
                }
                if (unique_count > 0) {
                    // 成功退出的循环 → 反馈=1.0（完全满意）
                    intent_base_learn(sys->controller, used_topos, unique_count, 1.0f);
                }
            }

            // 循环结束后处理 lifecycle
            if (out_reasoning && *out_reasoning == NULL) {
                *out_reasoning = reasoning;
            } else {
                dialog_reasoning_destroy(reasoning);
            }
            dialog_input_destroy(input);

            /* 自主学习（在跳过崩溃区之前执行） */
            if (sys->master && user_input && response) {
                autonomic_learn_from_dialog(sys->master, user_input, response,
                                     (AutonomicState*)sys->auto_state,
                                     sys->controller ? sys->controller->causal_graph : NULL,
                                     sys->controller ? sys->controller->memory : NULL);

                /* 主动学习：不满意时压制走错的边 */
                if (sys->learner && sys->controller && sys->last_knowledge_quality < 0.5f) {
                    char fb[64];
                    snprintf(fb, 63, "low_quality=%.2f", sys->last_knowledge_quality);
                    feedback_correct(sys->learner, user_input, response, fb);
                }
            }

            /* 后台时钟：上一轮对话中高激活节点，作为"自发性思维"注入下轮 */
            if (sys->controller && sys->master) {
                master_decay_activations(sys->master, 0.97f);
            }

            /* 跳过在线学习 — 调试用 */
            goto skip_postprocess;
        }
    } else {
        response = strdup("我理解了，但暂时不知道如何回答。");
        sys->last_knowledge_quality = 0.0f;
    }

    if (sys->memory && response) {
        char key[PM_KEY_BUF] = {0};
        snprintf(key, sizeof(key), "input:%s", user_input);

        MemoryEntry* existing = memory_retrieve(sys->memory, key);
        if (!existing) {
            memory_store(sys->memory, key, strdup(response),
                        strlen(response) + 1, MEMORY_TYPE_STRING, 0.3f);
            ui_print_thinking_line("学习", "已存入");
        }
        
        if (sys->master && user_input && response) {
            // 自主学习：同时激活→涨置信度（不需要反馈）
            autonomic_learn_from_dialog(sys->master, user_input, response,
                                     (AutonomicState*)sys->auto_state,
                                     sys->controller ? sys->controller->causal_graph : NULL,
                                     sys->controller ? sys->controller->memory : NULL);
            // BPTT 在线学习：RNN 反向传播（与拓扑学习互补）
            if (sys->bptt) {
                float loss = bptt_learn_from_dialog(sys->bptt, user_input, response);
                if (loss >= 0.0f && sys->turn_count % 10 == 0) {
                    LOG_INFO("[BPTT] step=%d loss=%.6f",
                           sys->bptt->steps, loss);
                }
            }
        }
        if (sys->master && user_input) {
            auto_learn_concepts(sys->master, user_input, sys->str_pool);
            
            // 更新概念层次结构
            if (sys->concept_hierarchy) {
                SubTopology* vocab = master_get_sub_topology_by_type(sys->master, TOPO_VOCABULARY);
                if (vocab && vocab->net && vocab->net->node_count >= 2) {
                    build_concept_hierarchy(vocab->net,
                        (ConceptHierarchy*)sys->concept_hierarchy, NULL);
                    
                    // 基于概念层级推断因果方向
                    if (sys->causal_graph) {
                        int directed = infer_causal_direction_from_hierarchy(
                            sys->causal_graph,
                            (ConceptHierarchy*)sys->concept_hierarchy,
                            sys->master, 0.5f);
                        if (directed > 0) {
                            char dir_info[64];
                            snprintf(dir_info, sizeof(dir_info),
                                    "%d 条边已确定方向", directed);
                            ui_print_thinking_line("因果方向", dir_info);
                        }
                    }
                }
            }
            
            // 上下文拓扑激活：意图→种子节点→跨拓扑连接→话题追踪
            if (sys->master) {
                DialogIntent intent = (sem && sem->intent.intent >= INTENT_QUERY)
                    ? sem->intent.intent : INTENT_CHAT;
                dialog_activate_context(sys->master, intent);

                /* 定期修剪跨拓扑连接表：每 20 轮清理低质量连接 */
                if (sys->turn_count > 0 && sys->turn_count % 20 == 0) {
                    int pruned = master_prune_cross_links(
                        sys->master, 0.15f, 3);
                    if (pruned > 0) {
                        LOG_INFO("[上下文清理] 第%d轮 修剪%d条低质量跨拓扑连接",
                                sys->turn_count, pruned);
                    }
                }
            }
        }
        if (sys->master && response) {
            auto_learn_concepts(sys->master, response, sys->str_pool);
        }
    }

    // === 更新认知状态（情感/动机系统）===
    if (sys->cognitive_state) {
        Interaction interaction;
        memset(&interaction, 0, sizeof(Interaction));
        interaction.user_input = (char*)user_input;
        interaction.system_response = response;
        interaction.timestamp = time(NULL);
        // 使用 knowledge_quality 作为 outcome 信号
        cognitive_state_update(sys->cognitive_state, &interaction, sys->last_knowledge_quality);
    }

    // === 认知调度快照（供下轮使用）===
    if (sys->controller && response) {
        cognitive_controller_snapshot(sys->controller, sys->last_knowledge_quality);
        // 存储本轮回复供下轮连贯性计算
        // （memory系统已存储，调度中心通过memory查询）
    }

skip_postprocess:
    fprintf(stderr, "[proc] before ui_print_ai_response, resp=%s\n", response ? response : "NULL"); fflush(stderr);
    ui_print_thinking_end();
    ui_print_ai_response(response);
    fflush(stdout);
    LOG_DEBUG("[proc] after ui_print_ai_response");
    
    semantic_understanding_destroy(sem);
    fprintf(stderr, "[proc] after sem destroy\n"); fflush(stderr);
    
    // 使用 strdup 复制，避免悬挂指针问题
    char* result = response ? strdup(response) : strdup("(null)");
    LOG_DEBUG("[proc] returning result: %s", result);
    free(response);
    return result;
}

// ==================== 测试函数 ====================

void dialog_test(MasterTopology* master, MemorySystem* memory, CausalGraph* causal_graph, ActiveLearner* learner) {
    printf("\n");
    printf("##########################################\n");
    printf("#       对话系统测试                     #\n");
    printf("##########################################\n");

    DialogSystem* sys = dialog_system_create(master, memory, causal_graph, learner);
    
    // 测试对话
    const char* test_inputs[] = {
        "量子计算是什么",
        "人工智能的发展",
        "什么是深度学习",
        "你觉得哲学有用吗",
        "中国文化有什么特点"
    };
    
    for (int i = 0; i < 5; i++) {
        char* response = dialog_process(sys, test_inputs[i], NULL);
        free(response);
        printf("\n");
    }

    dialog_system_destroy(sys);
}

// ==================== 因果推理联动 ====================

/**
 * 安全追加字符串到缓冲区，自动防溢出
 * @return 返回实际追加的字节数，缓冲区满返回 0
 */
static size_t safe_str_append(char* buf, size_t bufsz, const char* src) {
    size_t dlen = strlen(buf);
    if (dlen + 1 >= bufsz) return 0;  /* 缓冲区已满 */
    size_t remain = bufsz - dlen - 1;
    size_t slen = strlen(src);
    size_t copy = (slen < remain) ? slen : remain;
    memcpy(buf + dlen, src, copy);
    buf[dlen + copy] = '\0';
    return copy;
}

/**
 * 基于语义理解执行因果推理
 * @param sem 语义理解结果
 * @param graph 因果图
 * @param memory 记忆系统
 * @return 因果推理结果描述（需调用者释放）
 */
char* causal_reason_from_semantic(SemanticUnderstanding* sem, CausalGraph* graph,
                                   MemorySystem* memory) {
    if (!sem || !sem->causal_query) return NULL;

    #define RESP_SIZE 4096
    #define PATH_DESC_SIZE 512
    char* response = (char*)malloc(RESP_SIZE);
    if (!response) return NULL;
    response[0] = '\0';

    const char* cause_concept = (sem->key_concept_count >= 1) ? sem->key_concepts[0] : "未知";
    const char* effect_concept = (sem->key_concept_count >= 2) ? sem->key_concepts[1] :
                                 (sem->key_concept_count >= 1) ? sem->key_concepts[0] : "未知";

    // 如果有因果图，执行因果推理
    if (graph && graph->edge_count > 0) {
        int cause_id = sem->cause_node_id;
        int effect_id = sem->effect_node_id;

        // 如果节点ID有效，查找因果路径
        if (cause_id >= 0 && effect_id >= 0 && cause_id != effect_id) {
            int path_count = 0;
            CausalPath** paths = find_causal_paths_astar(graph, cause_id, effect_id,
                                                       MAX_PATH_LENGTH, 5, &path_count);

            if (path_count > 0) {
                snprintf(response, RESP_SIZE, "根据因果分析，「%s」→「%s」存在 %d 条因果路径：\n\n",
                        cause_concept, effect_concept, path_count);

                for (int i = 0; i < path_count && i < 3; i++) {
                    CausalPath* path = paths[i];
                    char path_desc[PATH_DESC_SIZE];

                    // 描述路径（用概念名称）
                    if (path->length >= 2) {
                        snprintf(path_desc, PATH_DESC_SIZE,
                                "  路径%d: %s", i + 1, cause_concept);
                    } else {
                        snprintf(path_desc, PATH_DESC_SIZE,
                                "  路径%d: %s → %s", i + 1, cause_concept, effect_concept);
                    }

                    for (int j = 1; j < path->length - 1 && j < 5; j++) {
                        safe_str_append(path_desc, PATH_DESC_SIZE, " → ... → ");
                    }
                    if (path->length > 2) {
                        safe_str_append(path_desc, PATH_DESC_SIZE, effect_concept);
                    }

                    char strength_info[128];
                    snprintf(strength_info, sizeof(strength_info),
                            " (因果强度: %.2f)\n", path->total_strength);
                    safe_str_append(path_desc, PATH_DESC_SIZE, strength_info);
                    safe_str_append(response, RESP_SIZE, path_desc);

                    // 释放路径
                    free(path->node_ids);
                    free(path->edge_strengths);
                    free(path);
                }
                free(paths);
            } else {
                // 没有找到直接路径，检查是否有单条直接边
                CausalEdge* direct_edge = get_causal_edge(graph, cause_id, effect_id);
                if (direct_edge) {
                    snprintf(response, RESP_SIZE,
                            "根据因果分析，存在直接的因果关系：\n\n"
                            "  %s → %s (因果强度: %.2f, 置信度: %.2f)\n\n"
                            "解释: %s 会直接影响 %s。\n",
                            cause_concept, effect_concept,
                            direct_edge->strength, direct_edge->confidence,
                            cause_concept, effect_concept);
                } else {
                    safe_str_append(response, RESP_SIZE, "我没有找到从 ");
                    safe_str_append(response, RESP_SIZE, cause_concept);
                    safe_str_append(response, RESP_SIZE, " 到 ");
                    safe_str_append(response, RESP_SIZE, effect_concept);
                    safe_str_append(response, RESP_SIZE, " 的明确因果路径。\n");
                }
            }
        } else {
            // 节点ID无效，尝试使用因果图搜索相关边
            snprintf(response, RESP_SIZE,
                    "我正在分析「%s」和「%s」之间的因果关系...\n\n",
                    cause_concept, effect_concept);

            // 查找所有从原因节点出发的边
            if (cause_id >= 0 && graph->outgoing_count && graph->outgoing_count[cause_id] > 0) {
                safe_str_append(response, RESP_SIZE, "从该原因出发的因果链条：\n");
                for (int i = 0; i < graph->outgoing_count[cause_id] && i < 3; i++) {
                    int target = graph->outgoing[cause_id][i];
                    CausalEdge* edge = get_causal_edge(graph, cause_id, target);
                    if (edge) {
                        char chain_desc[256];
                        snprintf(chain_desc, sizeof(chain_desc),
                                "  - %s → [节点%d] (强度: %.2f)\n",
                                cause_concept, target, edge->strength);
                        safe_str_append(response, RESP_SIZE, chain_desc);
                    }
                }
            }
        }
    }

    // 检查记忆中的因果规则
    if (memory && sem->key_concept_count >= 2) {
        CausalConfidence rule_conf;
        float strength = memory_get_causal_rule(memory,
            sem->key_concepts[0], sem->key_concepts[1], &rule_conf);

        if (strength > 0) {
            char rule_info[512];
            snprintf(rule_info, sizeof(rule_info),
                    "\n记忆中已学习到的因果关系：\n"
                    "  %s → %s\n"
                    "  因果强度: %.2f\n"
                    "  观察次数: %d次\n"
                    "  置信度: %.2f (%s)\n",
                    sem->key_concepts[0], sem->key_concepts[1],
                    strength, rule_conf.observation_count,
                    compute_causal_confidence(&rule_conf),
                    get_confidence_level_name(get_confidence_level(compute_causal_confidence(&rule_conf))));
            safe_str_append(response, RESP_SIZE, rule_info);
        }
    }

    // 如果没有找到任何因果信息
    if (strlen(response) == 0 || strstr(response, "没有找到") != NULL) {
        safe_str_append(response, RESP_SIZE, "\n我还没有学习到这条因果知识。\n");
        safe_str_append(response, RESP_SIZE, "如果你知道它们之间的关系，请告诉我：\n");
        safe_str_append(response, RESP_SIZE, "例如：「因为 A 所以 B」或「A 会导致 B」\n");
    }

    #undef RESP_SIZE
    #undef PATH_DESC_SIZE
    return response;
}

/**
 * 获取置信度级别名称
 */
static const char* get_confidence_level_name(CausalConfidenceLevel level) {
    switch (level) {
        case CAUSAL_CONF_CONTEXT: return "上下文记忆";
        case CAUSAL_CONF_SHORT_TERM: return "短期记忆";
        case CAUSAL_CONF_PERMANENT: return "永久记忆";
        case CAUSAL_CONF_CORE: return "核心知识";
        default: return "未知";
    }
}

/**
 * 处理因果查询（为什么/原因）
 */
char* process_causal_query(SemanticUnderstanding* sem, CausalGraph* graph,
                          MemorySystem* memory) {
    if (!sem || !sem->causal_query) return NULL;

    char* causal_result = causal_reason_from_semantic(sem, graph, memory);
    if (!causal_result) return NULL;

    // 构建完整回复
    #define FRESP_SIZE 4096
    char* full_response = (char*)malloc(FRESP_SIZE);
    if (!full_response) {
        free(causal_result);
        return NULL;
    }
    full_response[0] = '\0';

    // 添加解释前缀
    if (sem->key_concept_count >= 2) {
        snprintf(full_response, FRESP_SIZE, "关于「%s → %s」的因果关系：\n\n",
                sem->key_concepts[0], sem->key_concepts[1]);
    }

    safe_str_append(full_response, FRESP_SIZE, causal_result);
    free(causal_result);

    // 添加建议学习
    safe_str_append(full_response, FRESP_SIZE, "\n如果想让我学习更多因果知识，请告诉我具体的关系。");
    #undef FRESP_SIZE

    return full_response;
}