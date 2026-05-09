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
// #include "network_tool.h"  // Windows-only, excluded on Linux
#include "concept_processor.h"
#include "concept_abstraction.h"
#include "string_pool.h"
#include "generative_model.h"
#include "tensor.h"
#include "common.h"
#include "autonomic_learner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdbool.h>
#include <pthread.h>

#include "thread_pool.h"

#ifdef _WIN32
#ifdef _WIN32
#include <windows.h>
#endif
#endif

// 前向声明
static const char* get_confidence_level_name(CausalConfidenceLevel level);

// ==================== 推理常量 ====================

#define MAX_TOKENS 64
#define MAX_ASSOCIATIONS 100
#define MAX_RESPONSE_LENGTH 2048
#define MAX_PATH_LENGTH 10
#define ACTIVATION_THRESHOLD 0.3f
#define DECAY_RATE 0.7f
#define DEFAULT_HOP_COUNT 3

// ==================== 并行拓扑传播任务（每跳内部） ====================

/** 单跳内子拓扑传播任务 */
typedef struct {
    MasterTopology* master;
    DialogReasoning* reasoning;
    int topo_id;
    int hop;
    pthread_mutex_t* assoc_mutex;
    volatile int* hop_propagated;
} DialogTopoTask;

/** Worker：在指定子拓扑内执行一跳传播 */
static void dialog_topo_worker(void* arg) {
    DialogTopoTask* task = (DialogTopoTask*)arg;
    MasterTopology* master = task->master;
    DialogReasoning* reasoning = task->reasoning;
    SubTopology* sub = master_get_sub_topology(master, task->topo_id);
    if (!sub || !sub->net) return;

    for (int n = 0; n < sub->net->node_count; n++) {
        ReasoningNode* node = sub->net->nodes[n];
        if (!node || node->activation < 0.15f) continue;
        if (task->hop > 1 && node->is_visited) continue;

        node->is_visited = 1;

        for (int c = 0; c < node->connection_count; c++) {
            ReasoningNode* connected = node->connections[c];
            if (!connected) continue;

            float edge_confidence = node->connection_confidences[c];
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

            float new_activation = node->connection_weights[c] *
                                  node->activation *
                                  confidence_factor *
                                  activation_multiplier *
                                  embed_factor *
                                  DECAY_RATE;

            if (new_activation > ACTIVATION_THRESHOLD) {
                if (new_activation > connected->activation)
                    connected->activation = new_activation;

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
static void resolve_causal_query(SemanticUnderstanding* sem, MasterTopology* master) {
    if (!sem || !sem->causal_query || !master) return;

    // 为所有关键概念查找节点ID
    for (int i = 0; i < sem->key_concept_count && i < 50; i++) {
        sem->key_concept_ids[i] = find_node_id_by_concept(master, sem->key_concepts[i]);
        if (sem->key_concept_ids[i] >= 0) {
            printf("  概念 [%s] → 节点ID %d\n", sem->key_concepts[i], sem->key_concept_ids[i]);
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

    printf("  因果查询: 节点%d → 节点%d\n", sem->cause_node_id, sem->effect_node_id);
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

    char* tokens_buf[64];
    sem->token_count = utf8_tokenize(text, tokens_buf, 64);
    sem->tokens = (char**)malloc(sem->token_count * sizeof(char*));
    for (int i = 0; i < sem->token_count; i++) {
        sem->tokens[i] = strdup(tokens_buf[i]);
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

        printf("  原因位置: %s\n", cause_pos ? cause_pos : "(使用关键词)");
        printf("  结果位置: %s\n", effect_pos ? effect_pos : "(使用关键词)");
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
    char* tokens_buf[64];  // 临时缓冲区
    input->token_count = utf8_tokenize(text, tokens_buf, 64);
    
    if (input->token_count == 0) {
        input->token_count = 0;
        return input;
    }
    
    // 复制tokens到动态内存
    input->tokens = (char**)malloc(input->token_count * sizeof(char*));
    for (int i = 0; i < input->token_count; i++) {
        input->tokens[i] = strdup(tokens_buf[i]);
    }
    
    if (input->token_count == 0 || !input->tokens) {
        input->token_count = 0;
        return input;
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

DialogReasoning* dialog_reason(DialogInput* input, MasterTopology* master) {
    if (!input || !master || input->token_count == 0) return NULL;
    
    master_decay_activations(master, 0.5f);
    
    DialogReasoning* reasoning = (DialogReasoning*)calloc(1, sizeof(DialogReasoning));
    if (!reasoning) return NULL;
    
    // 先收集所有找到/创建的节点
    ReasoningNode* found_nodes[64];
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
                
                if (found_count < 64) {
                    found_nodes[found_count++] = node;
                }
            } else {
                // 找不到节点？像人脑一样，创建新节点来学习这个概念！
                int new_id = huarong_net_dynamic_add_node(sub->net, input->tokens[i], NULL, 0);
                if (new_id >= 0 && sub->node_hash) {
                    ReasoningNode* new_node = sub->net->nodes[sub->net->node_count - 1];
                    new_node->confidence = 0.3f;
                    new_node->activation = 0.5f;
                    node_hash_add(sub->node_hash, new_node);
                    
                    dialog_add_association(reasoning, 
                        new_node->concept, 0.5f, sub->type, 0,
                        new_node->node_id, -1);
                    
                    if (found_count < 64) {
                        found_nodes[found_count++] = new_node;
                    }
                }
            }
        }
    }
    
    // 同一句话中出现的概念应该互相连接（像人脑的Hebbian学习）
    // 需要在每个拓扑子网络中建立连接
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        
        // 收集这个子拓扑中的节点
        ReasoningNode* sub_nodes[64];
        int sub_count = 0;
        
        for (int i = 0; i < found_count; i++) {
            // 检查found_nodes[i]是否属于当前子拓扑
            ReasoningNode* a = found_nodes[i];
            if (!a) continue;
            for (int n = 0; n < sub->net->node_count; n++) {
                if (sub->net->nodes[n] == a) {
                    sub_nodes[sub_count++] = a;
                    break;
                }
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
                    for (int c = 0; c < a->connection_count; c++) {
                        if (a->connections[c] == b) {
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
        // 1. 统计活跃子拓扑
        int topo_ids[16], active_topos = 0;
        for (int t = 0; t < master->sub_topo_count && active_topos < 16; t++) {
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

        // 2. 构建并行任务
        pthread_mutex_t assoc_mutex = PTHREAD_MUTEX_INITIALIZER;
        int hop_propagated = 0;

        DialogTopoTask tasks[16];
        ThreadTask th_tasks[16];
        for (int i = 0; i < active_topos; i++) {
            tasks[i].master = master;
            tasks[i].reasoning = reasoning;
            tasks[i].topo_id = topo_ids[i];
            tasks[i].hop = hop;
            tasks[i].assoc_mutex = &assoc_mutex;
            tasks[i].hop_propagated = &hop_propagated;
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

        if (hop_propagated == 0) break;
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

// ==================== 回复生成 ====================

char* dialog_generate(DialogReasoning* reasoning, const char* input,
                     MemorySystem* memory, int max_len, void* sys) {
    if (!reasoning) return strdup("我需要重新整理一下思路...");
    
    DialogSystem* dsys = (DialogSystem*)sys;
    
    // 优先检查记忆系统：精确匹配完整输入
    if (memory && input && input[0]) {
        char exact_key[512] = {0};
        snprintf(exact_key, sizeof(exact_key), "response:%s", input);
        MemoryEntry* exact = memory_retrieve(memory, exact_key);
        if (exact && exact->data) {
            printf("[响应模式] 精确匹配: %s\n", (char*)exact->data);
            return strdup((char*)exact->data);
        }
    }
    
    // 拓扑驱动生成：认知网络做联想推理
    if (dsys && dsys->master && dsys->master->sub_topo_count > 0) {
        int total_nodes = 0;
        for (int t = 0; t < dsys->master->sub_topo_count; t++) {
            SubTopology* sub = dsys->master->sub_topologies[t];
            if (sub && sub->net) total_nodes += sub->net->node_count;
        }
        if (total_nodes >= 10) {
            char* topo_response = master_generate_response(
                dsys->master, input, max_len);
            if (topo_response && strlen(topo_response) > 0) {
                char* safe = strdup(topo_response);
                free(topo_response);
                return safe;
            }
            if (topo_response) free(topo_response);
        }
    }
    
    char* response = (char*)malloc(max_len);
    if (!response) return strdup("内存不足...");

    response[0] = '\0';
    int pos = 0;
    
    // 按拓扑分类收集概念
    #define MAX_PER_CATEGORY 20
    char* semantic_concepts[MAX_PER_CATEGORY];
    char* emotion_concepts[MAX_PER_CATEGORY];
    char* cultural_concepts[MAX_PER_CATEGORY];
    char* context_concepts[MAX_PER_CATEGORY];
    char* other_concepts[MAX_PER_CATEGORY];
    
    int semantic_count = 0, emotion_count = 0, cultural_count = 0;
    int context_count = 0, other_count = 0;
    
    float semantic_activation = 0, emotion_activation = 0;
    float cultural_activation = 0, context_activation = 0;
    
    for (int i = 0; i < reasoning->assoc_count; i++) {
        float act = reasoning->associations[i].activation;
        char* concept = reasoning->associations[i].concept;
        
        if (act < ACTIVATION_THRESHOLD) continue;
        
        switch (reasoning->associations[i].topo_type) {
            case TOPO_SEMANTIC:
                if (semantic_count < MAX_PER_CATEGORY) {
                    semantic_concepts[semantic_count++] = concept;
                    semantic_activation += act;
                }
                break;
            case TOPO_EMOTION:
                if (emotion_count < MAX_PER_CATEGORY) {
                    emotion_concepts[emotion_count++] = concept;
                    emotion_activation += act;
                }
                break;
            case TOPO_CULTURE:
                if (cultural_count < MAX_PER_CATEGORY) {
                    cultural_concepts[cultural_count++] = concept;
                    cultural_activation += act;
                }
                break;
            case TOPO_CONTEXT:
                if (context_count < MAX_PER_CATEGORY) {
                    context_concepts[context_count++] = concept;
                    context_activation += act;
                }
                break;
            default:
                if (other_count < MAX_PER_CATEGORY) {
                    other_concepts[other_count++] = concept;
                }
        }
    }
    
    // 根据激活情况生成回复
    
    // 语义层回复
    if (semantic_count > 0) {
        semantic_activation /= semantic_count;
        pos += snprintf(response + pos, max_len - pos, "关于这个，");
        
        int use_count = semantic_count > 3 ? 3 : semantic_count;
        for (int i = 0; i < use_count; i++) {
            pos += snprintf(response + pos, max_len - pos, "%s", semantic_concepts[i]);
            if (i < use_count - 1) {
                pos += snprintf(response + pos, max_len - pos, "、");
            }
        }
        
        if (semantic_activation > 0.6f) {
            pos += snprintf(response + pos, max_len - pos, "是核心相关的概念");
        } else {
            pos += snprintf(response + pos, max_len - pos, "这些是相关的概念");
        }
    }
    
    // 情绪层回复
    if (emotion_count > 0) {
        if (pos > 0) {
            pos += snprintf(response + pos, max_len - pos, "，我能感受到");
        } else {
            pos += snprintf(response + pos, max_len - pos, "我能感受到");
        }
        
        int use_count = emotion_count > 2 ? 2 : emotion_count;
        for (int i = 0; i < use_count; i++) {
            pos += snprintf(response + pos, max_len - pos, "%s", emotion_concepts[i]);
            if (i < use_count - 1) {
                pos += snprintf(response + pos, max_len - pos, "和");
            }
        }
    }
    
    // 文化层回复  
    if (cultural_count > 0) {
        if (pos > 0) {
            pos += snprintf(response + pos, max_len - pos, "，从文化角度看");
        } else {
            pos += snprintf(response + pos, max_len - pos, "从文化角度看");
        }
        
        int use_count = cultural_count > 2 ? 2 : cultural_count;
        for (int i = 0; i < use_count; i++) {
            pos += snprintf(response + pos, max_len - pos, "%s", cultural_concepts[i]);
            if (i < use_count - 1) {
                pos += snprintf(response + pos, max_len - pos, "和");
            }
        }
    }
    
    // 上下文层回复
    if (context_count > 0) {
        if (pos > 0) {
            pos += snprintf(response + pos, max_len - pos, "，在当前语境下");
        } else {
            pos += snprintf(response + pos, max_len - pos, "在当前语境下");
        }
        
        int use_count = context_count > 2 ? 2 : context_count;
        for (int i = 0; i < use_count; i++) {
            pos += snprintf(response + pos, max_len - pos, "%s", context_concepts[i]);
            if (i < use_count - 1) {
                pos += snprintf(response + pos, max_len - pos, "、");
            }
        }
    }
    
    // 其他层回复
    if (other_count > 0) {
        if (pos > 0) {
            pos += snprintf(response + pos, max_len - pos, "，此外还涉及");
        } else {
            pos += snprintf(response + pos, max_len - pos, "此外还涉及");
        }
        
        int use_count = other_count > 3 ? 3 : other_count;
        for (int i = 0; i < use_count; i++) {
            pos += snprintf(response + pos, max_len - pos, "%s", other_concepts[i]);
            if (i < use_count - 1) {
                pos += snprintf(response + pos, max_len - pos, "、");
            }
        }
    }
    
    // 先检查是否是概念型知识（数学、规则等）
    if (concept_is_math_expression(input)) {
        printf("[概念处理] 检测到数学表达式\n");
        char* calc_result = concept_calculate(input);
        
        if (calc_result) {
            pos += snprintf(response + pos, max_len - pos, 
                "计算结果: %s", calc_result);
            
            // 学习这个规则到概念拓扑
            if (sys && ((DialogSystem*)sys)->master) {
                concept_learn_rule(((DialogSystem*)sys)->master, input, calc_result);
            }
            
            free(calc_result);
        }
    }
    
    // 临时禁用联网搜索（网络不通）
    /*
    // 如果知识不足，自动联网搜索
    if (pos == 0 && reasoning->assoc_count < 3) {
        printf("[联网] 知识不足，正在搜索: %s\n", input);
        
        network_init();
        char* search_result = network_search(input);
        
        if (search_result && strlen(search_result) > 100) {
            printf("[联网] 搜索到结果，正在学习...\n");
            
            // 解析搜索结果并学习
            // 这里简化处理：直接把搜索结果作为新知识
            if (memory) {
                char key[512] = {0};
                snprintf(key, sizeof(key), "web:%s", input);
                memory_store(memory, key, search_result, strlen(search_result) + 1, 
                           MEMORY_TYPE_STRING, 0.5f);
            }
            
            pos += snprintf(response + pos, max_len - pos, 
                "让我查一下...根据搜索结果：%s", 
                strlen(search_result) > 200 ? "..." : search_result);
            
            free(search_result);
        } else {
            printf("[联网] 搜索失败或无结果\n");
        }
        
        network_cleanup();
    }
    */
    
    // 如果没有足够的联想，但记忆系统有相关记忆
    if (pos == 0 && memory) {
        // 尝试在记忆中查找
        char key[256] = {0};
        snprintf(key, sizeof(key) - 1, "input:%s", input);
        
        MemoryEntry* mem = memory_retrieve(memory, key);
        if (mem && mem->data && mem->importance > 0.6f) {
            pos += snprintf(response + pos, max_len - pos, 
                "我记得你教过我这个：%s", (char*)mem->data);
            printf("从记忆系统检索: 重要性%.3f\n", mem->importance);
        } else {
            // 学习邀请
            pos += snprintf(response + pos, max_len - pos, 
                "我还在学习中，关于这个你能教我吗？");
            
            // 存入记忆系统（低重要性，待学习）
            if (mem) {
                memory_update_confidence(memory, key, 0.2f);
            } else {
                memory_store(memory, key, strdup("待学习"), 10, 
                           MEMORY_TYPE_STRING, 0.1f);  // importance = 0.1f
            }
            printf("新概念，存入记忆系统待学习\n");
        }
    }
    
    // 最基础的回复
    if (pos == 0) {
        pos += snprintf(response + pos, max_len - pos, 
            "我正在思考这个问题...");
    }
    
    return response;
}

// ==================== 自动学习概念到拓扑网络 ====================
// 真正的学习应该像人脑一样，通过对话自然发生，而不是人为预先连接神经回路
// 实现：从对话文本中提取有意义的概念，加入拓扑网络并建立共现连接

// 中文停用词（过滤高频无意义字词）
static const char* STOP_WORDS[] = {
    "的", "了", "是", "在", "有", "和", "就", "不", "都", "而",
    "及", "与", "着", "或", "也", "很", "会", "可", "但", "这",
    "那", "上", "下", "到", "去", "来", "为", "以", "能", "要",
    "我", "你", "他", "她", "它", "们", "个", "之", "对", "被",
    "把", "让", "向", "从", "比", "还", "又", "再", "才", "啊",
    "吧", "吗", "呢", "哈", "呀", "哦", "嗯", "嘛", "啦", "哇",
    "什么", "怎么", "为什么", "如何", "哪个", "哪些",
    // 标点符号
    "，", "。", "、", "；", "：", "？", "！", "…", "—", "～",
    "·", "．", "（", "）", "【", "】", "《", "》", "”", "“",
    "‘", "’", "　"
};
#define STOP_WORDS_COUNT (sizeof(STOP_WORDS) / sizeof(STOP_WORDS[0]))

static int is_stop_word(const char* word) {
    if (!word || strlen(word) == 0) return 1;
    for (int i = 0; i < (int)STOP_WORDS_COUNT; i++) {
        if (strcmp(word, STOP_WORDS[i]) == 0) return 1;
    }
    return 0;
}

// 检查字符串是否包含中文或英文标点
static int contains_punctuation(const char* s) {
    if (!s) return 0;
    const char* punct = "，。、；：？！…—～·．（）【】《》”“‘’　,.;:!?\"'()[]{}<>/\\@#$%^&*_+-=~`";
    for (const char* p = punct; *p; p++) {
        if (strchr(s, *p)) return 1;
    }
    return 0;
}

static int concept_exists(HuarongTopologyNet* net, const char* concept) {
    if (!net || !concept) return -1;
    for (int i = 0; i < net->node_count; i++) {
        if (net->nodes[i] && net->nodes[i]->concept &&
            strcmp(net->nodes[i]->concept, concept) == 0) {
            return i;  // 返回已有节点ID
        }
    }
    return -1;  // 不存在
}

static int get_or_create_concept(SubTopology* topo, const char* concept) {
    if (!topo || !topo->net || !concept) return -1;
    int existing = concept_exists(topo->net, concept);
    if (existing >= 0) return existing;
    float feat[NODE_FEATURE_DIM];
    for (int i = 0; i < NODE_FEATURE_DIM; i++)
        feat[i] = ((float)rand() / RAND_MAX - 0.5f) * 0.1f;
    ReasoningNode* node = huarong_net_add_node(topo->net, concept, feat, NODE_FEATURE_DIM);
    return node ? node->node_id : -1;
}

void auto_learn_concepts(MasterTopology* master, const char* text, void* str_pool) {
    (void)str_pool;  // 保留供后续字符串池优化使用
    if (!master || !text || strlen(text) == 0) return;

    // 获取词汇拓扑和语义拓扑
    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    SubTopology* semantic = master_get_sub_topology_by_type(master, TOPO_SEMANTIC);
    if (!vocab || !vocab->net || !semantic || !semantic->net) return;

    // 分词
    char* tokens[64];
    int token_count = utf8_tokenize(text, tokens, 64);
    if (token_count <= 0) return;

    // 第一步：从单个字符组合成有意义的双字/三字概念
    // 对于中文，连续中文字符合并为2~3字窗口
    char* concepts[64];
    int concept_count = 0;

    for (int i = 0; i < token_count && concept_count < 64; i++) {
        // 跳过停用词和含标点的词
        if (is_stop_word(tokens[i]) || contains_punctuation(tokens[i])) continue;

        // 英文/ASCII词直接作为概念
        if (!tokens[i] || strlen(tokens[i]) == 0) continue;
        unsigned char c = (unsigned char)tokens[i][0];
        if ((c & 0x80) == 0) {
            // ASCII token（英文词、数字等）
            if (strlen(tokens[i]) >= 2) {  // 至少2个字符才有意义
                concepts[concept_count++] = strdup(tokens[i]);
            }
        }
    }

    // 对于中文单字，尝试组合成双字词
    // 收集所有中文单字位置
    int chinese_pos[64], chinese_count = 0;
    for (int i = 0; i < token_count && chinese_count < 64; i++) {
        if (!tokens[i]) continue;
        unsigned char c = (unsigned char)tokens[i][0];
        if ((c & 0x80) != 0 && strlen(tokens[i]) == 3) {  // 中文字符
            chinese_pos[chinese_count++] = i;
        }
    }

    // 组合双字词（前后相邻的中文字符）
    for (int i = 0; i < chinese_count - 1 && concept_count < 64; i++) {
        char bigram[7];  // 2个中文字符(3+3) + 终止符
        snprintf(bigram, sizeof(bigram), "%s%s",
                 tokens[chinese_pos[i]], tokens[chinese_pos[i + 1]]);
        if (!is_stop_word(bigram) && !contains_punctuation(bigram)) {
            concepts[concept_count++] = strdup(bigram);
        }
    }

    // 第二步：将概念加入词汇拓扑
    int concept_ids[64];
    int valid_count = 0;
    for (int i = 0; i < concept_count; i++) {
        int id = get_or_create_concept(vocab, concepts[i]);
        if (id >= 0) {
            concept_ids[valid_count++] = id;
        }
    }

    // 第三步：在语义拓扑中建立共现连接
    // 同一次对话中出现的概念之间建立/增强连接
    for (int i = 0; i < valid_count; i++) {
        for (int j = i + 1; j < valid_count; j++) {
            // 检查语义拓扑中是否已有连接
            int conn_exists = 0;
            ReasoningNode* node_a = vocab->net->nodes[concept_ids[i]];
            if (node_a) {
                for (int k = 0; k < node_a->connection_count; k++) {
                    ReasoningNode* target = node_a->connections[k];
                    if (target && target->node_id == concept_ids[j]) {
                        // 已有连接，增强权重
                        node_a->connection_weights[k] += 0.1f;
                        if (node_a->connection_weights[k] > 1.0f)
                            node_a->connection_weights[k] = 1.0f;
                        conn_exists = 1;
                        break;
                    }
                }
            }
            if (!conn_exists) {
                huarong_net_add_connection(vocab->net,
                    concept_ids[i], concept_ids[j], 0.5f);
            }
        }
    }
    
    // 第四步：在线更新节点 embedding（Hebbian: 共现节点互相拉近）
    float lr = 0.02f;
    for (int i = 0; i < valid_count; i++) {
        ReasoningNode* ni = vocab->net->nodes[concept_ids[i]];
        if (!ni || !ni->features || ni->feature_dim != NODE_FEATURE_DIM) continue;
        for (int j = i + 1; j < valid_count; j++) {
            ReasoningNode* nj = vocab->net->nodes[concept_ids[j]];
            if (!nj || !nj->features || nj->feature_dim != NODE_FEATURE_DIM) continue;
            hebbian_update(ni->features, nj->features, NODE_FEATURE_DIM, lr);
        }
    }

    // 清理
    for (int i = 0; i < token_count; i++) {
        free(tokens[i]);
    }
    for (int i = 0; i < concept_count; i++) {
        free(concepts[i]);
    }
}

// ==================== 自我验证机制 ====================
// AI 检查自己的知识是否一致、是否可靠

typedef struct {
    int is_consistent;        // 知识是否一致
    float confidence;        // 置信度 0-1
    char conflicts[5][256];   // 发现的冲突
    int conflict_count;
    char suggestion[512];      // 改进建议
} SelfVerificationResult;

// 检查两个概念是否矛盾

// 自我验证：检查知识一致性
static SelfVerificationResult self_verify_knowledge(DialogReasoning* reasoning, MemorySystem* memory) {
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
    // 创建轻量级Seq2Seq神经模型: vocab=200, emb=32, hidden=64, max_seq=20
    sys->gen_vocab = gen_vocab_create(200);
    sys->seq2seq = seq2seq_create(200, 32, 64, 20);
    printf("[对话系统] 神经模型: %s\n", sys->seq2seq ? "已就绪" : "创建失败");
    sys->session_id = time(NULL);
    sys->turn_count = 0;
    sys->max_hop_count = 3;
    sys->activation_threshold = 0.3f;
    sys->decay_rate = 0.7f;

    sys->cognitive_state = cognitive_state_create();
    if (sys->cognitive_state) {
        cognitive_state_init(sys->cognitive_state);
        printf("[对话系统] 认知状态（情感/动机系统）: 已就绪\n");
    }

    printf("[对话系统] 创建成功，会话ID: %ld\n", sys->session_id);
    printf("[对话系统] 因果图: %s\n", causal_graph ? "已连接" : "未连接");
    printf("[对话系统] 主动学习器: %s\n", learner ? "已连接" : "未连接");

    return sys;
}

void dialog_system_destroy(DialogSystem* sys) {
    if (!sys) return;
    printf("[对话系统] 销毁，会话ID: %ld, 对话轮数: %d\n", 
           sys->session_id, sys->turn_count);
    if (sys->concept_hierarchy) {
        concept_hierarchy_destroy((ConceptHierarchy*)sys->concept_hierarchy);
    }
    if (sys->str_pool) {
        string_pool_destroy((StringPool*)sys->str_pool);
    }
    if (sys->seq2seq) {
        seq2seq_destroy((Seq2SeqModel*)sys->seq2seq);
    }
    if (sys->gen_vocab) {
        gen_vocab_destroy((GenVocabulary*)sys->gen_vocab);
    }
    if (sys->cognitive_state) {
        cognitive_state_destroy(sys->cognitive_state);
    }
    free(sys);
}

char* dialog_process(DialogSystem* sys, const char* user_input, DialogReasoning** out_reasoning) {
    if (!sys || !user_input) return strdup("系统错误...");

    if (out_reasoning) *out_reasoning = NULL;
    
    sys->turn_count++;
    
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
    
    if (input && input->token_count > 0) {
        reasoning = dialog_reason(input, sys->master);
        
        char assoc_info[128] = {0};
        snprintf(assoc_info, sizeof(assoc_info), "找到 %d 个关联", reasoning->assoc_count);
        ui_print_thinking_line("联想", assoc_info);
        
        // 输出推理链
        if (reasoning->chain_length > 0) {
            char chain_info[256] = {0};
            int pos = 0;
            int show_count = reasoning->chain_length > 3 ? 3 : reasoning->chain_length;
            for (int i = 0; i < show_count; i++) {
                pos += snprintf(chain_info + pos, sizeof(chain_info) - pos, "%s ", 
                    reasoning->reasoning_chain[i]);
            }
            if (reasoning->chain_length > 3) {
                snprintf(chain_info + pos, sizeof(chain_info) - pos, "...(+%d)", 
                    reasoning->chain_length - 3);
            }
            ui_print_thinking_line("推理链", chain_info);
        }
        
        // 自我验证：检查知识是否足够
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
        
        // 自我验证结果用于指导回复生成
        reasoning->knowledge_quality = knowledge_quality;
        sys->last_knowledge_quality = knowledge_quality;
        
        // 进行自我验证
        SelfVerificationResult verify = self_verify_knowledge(reasoning, sys->memory);
        reasoning->is_verified = 1;
        
        // 如果置信度低，提示需要学习
        if (verify.confidence < 0.4f) {
            ui_print_thinking_line("自检", verify.suggestion);
        }
        
        response = dialog_generate(reasoning, user_input, sys->memory,
                                MAX_RESPONSE_LENGTH, sys);
        if (out_reasoning && *out_reasoning == NULL) {
            *out_reasoning = reasoning;
        } else {
            dialog_reasoning_destroy(reasoning);
        }
        dialog_input_destroy(input);
    } else {
        response = strdup("我理解了，但暂时不知道如何回答。");
        sys->last_knowledge_quality = 0.0f;
    }

    if (sys->memory && response) {
        char key[512] = {0};
        snprintf(key, 511, "input:%s", user_input);

        MemoryEntry* existing = memory_retrieve(sys->memory, key);
        if (!existing) {
            memory_store(sys->memory, key, strdup(response),
                        strlen(response) + 1, MEMORY_TYPE_STRING, 0.3f);
            ui_print_thinking_line("学习", "已存入");
        }
        
        if (sys->master && user_input && response) {
            // 自主学习：同时激活→涨置信度（不需要反馈）
            autonomic_learn_from_dialog(sys->master, user_input, response, NULL);
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
            
            // 存入上下文拓扑：理解用户输入的场景
            SubTopology* context_sub = master_get_sub_topology(sys->master, TOPO_CONTEXT);
            if (context_sub && context_sub->net) {
                char context_key[512];
                snprintf(context_key, sizeof(context_key), "用户:%s", user_input);
                huarong_net_add_node(context_sub->net, context_key, NULL, 0);
            }
        }
        if (sys->master && response) {
            auto_learn_concepts(sys->master, response, sys->str_pool);
            
            // 存入上下文拓扑：理解AI回复的场景
            SubTopology* context_sub = master_get_sub_topology(sys->master, TOPO_CONTEXT);
            if (context_sub && context_sub->net) {
                char context_key[512];
                snprintf(context_key, sizeof(context_key), "AI:%s", response);
                huarong_net_add_node(context_sub->net, context_key, NULL, 0);
            }
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

    ui_print_thinking_end();
    ui_print_ai_response(response);
    fflush(stdout);
    
    semantic_understanding_destroy(sem);
    
    // 使用 strdup 复制，避免悬挂指针问题
    char* result = response ? strdup(response) : strdup("(null)");
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
 * 基于语义理解执行因果推理
 * @param sem 语义理解结果
 * @param graph 因果图
 * @param memory 记忆系统
 * @return 因果推理结果描述（需调用者释放）
 */
char* causal_reason_from_semantic(SemanticUnderstanding* sem, CausalGraph* graph,
                                   MemorySystem* memory) {
    if (!sem || !sem->causal_query) return NULL;

    char* response = (char*)malloc(4096);
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
                snprintf(response, 1024, "根据因果分析，「%s」→「%s」存在 %d 条因果路径：\n\n",
                        cause_concept, effect_concept, path_count);

                for (int i = 0; i < path_count && i < 3; i++) {
                    CausalPath* path = paths[i];
                    char path_desc[512];

                    // 描述路径（用概念名称）
                    if (path->length >= 2) {
                        snprintf(path_desc, sizeof(path_desc),
                                "  路径%d: %s", i + 1, cause_concept);
                    } else {
                        snprintf(path_desc, sizeof(path_desc),
                                "  路径%d: %s → %s", i + 1, cause_concept, effect_concept);
                    }

                    for (int j = 1; j < path->length - 1 && j < 5; j++) {
                        // 简化：只显示中间节点数量
                        strcat(path_desc, " → ... → ");
                    }
                    if (path->length > 2) {
                        strcat(path_desc, effect_concept);
                    }

                    char strength_info[128];
                    snprintf(strength_info, sizeof(strength_info),
                            " (因果强度: %.2f)\n", path->total_strength);
                    strcat(path_desc, strength_info);
                    strcat(response, path_desc);

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
                    snprintf(response, 4096,
                            "根据因果分析，存在直接的因果关系：\n\n"
                            "  %s → %s (因果强度: %.2f, 置信度: %.2f)\n\n"
                            "解释: %s 会直接影响 %s。\n",
                            cause_concept, effect_concept,
                            direct_edge->strength, direct_edge->confidence,
                            cause_concept, effect_concept);
                } else {
                    strcat(response, "我没有找到从 ");
                    strcat(response, cause_concept);
                    strcat(response, " 到 ");
                    strcat(response, effect_concept);
                    strcat(response, " 的明确因果路径。\n");
                }
            }
        } else {
            // 节点ID无效，尝试使用因果图搜索相关边
            snprintf(response, 1024,
                    "我正在分析「%s」和「%s」之间的因果关系...\n\n",
                    cause_concept, effect_concept);

            // 查找所有从原因节点出发的边
            if (cause_id >= 0 && graph->outgoing_count && graph->outgoing_count[cause_id] > 0) {
                strcat(response, "从该原因出发的因果链条：\n");
                for (int i = 0; i < graph->outgoing_count[cause_id] && i < 3; i++) {
                    int target = graph->outgoing[cause_id][i];
                    CausalEdge* edge = get_causal_edge(graph, cause_id, target);
                    if (edge) {
                        char chain_desc[256];
                        snprintf(chain_desc, sizeof(chain_desc),
                                "  - %s → [节点%d] (强度: %.2f)\n",
                                cause_concept, target, edge->strength);
                        strcat(response, chain_desc);
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
            strcat(response, rule_info);
        }
    }

    // 如果没有找到任何因果信息
    if (strlen(response) == 0 || strstr(response, "没有找到") != NULL) {
        strcat(response, "\n我还没有学习到这条因果知识。\n");
        strcat(response, "如果你知道它们之间的关系，请告诉我：\n");
        strcat(response, "例如：「因为 A 所以 B」或「A 会导致 B」\n");
    }

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
    char* full_response = (char*)malloc(4096);
    full_response[0] = '\0';

    // 添加解释前缀
    if (sem->key_concept_count >= 2) {
        snprintf(full_response, 256, "关于「%s → %s」的因果关系：\n\n",
                sem->key_concepts[0], sem->key_concepts[1]);
    }

    strcat(full_response, causal_result);
    free(causal_result);

    // 添加建议学习
    strcat(full_response, "\n如果想让我学习更多因果知识，请告诉我具体的关系。");

    return full_response;
}