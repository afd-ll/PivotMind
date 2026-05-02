/**
 * @file associative_reasoning.c
 * @brief 联想推理引擎 - 基于拓扑网络的真正联想生成
 * 
 * 核心思想：
 * 1. 输入词激活词汇节点
 * 2. 词汇节点通过连接激活相关语义/文化节点
 * 3. 语义节点反向激活相关词汇节点（联想）
 * 4. 基于激活强度动态生成内容
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "multi_topology.h"
#include "node_hash.h"
#include "utf8_tokenizer.h"

#define MAX_ASSOCIATIONS 50
#define ACTIVATION_THRESHOLD 0.3f
#define DECAY_RATE 0.7f

// 联想结果
typedef struct {
    char concept[256];
    float activation;
    int topo_type;
    int hop_count;  // 跳数
} Association;

// 联想引擎
typedef struct {
    MasterTopology* topology;
    Association associations[MAX_ASSOCIATIONS];
    int assoc_count;
    
    // 激活历史（用于防止循环）
    int visited_nodes[1000];
    int visited_count;
    
    // 联想缓存
    char cache_keys[20][256];
    char cache_values[20][512];
    int cache_hits[20];
    int cache_count;
    int cache_next;
} AssociativeEngine;

// ==================== 生成模板系统 ====================

// 模板类型
typedef enum {
    TEMPLATE_DEFAULT = 0,      // 默认模板
    TEMPLATE_QUESTION = 1,     // 疑问句模板
    TEMPLATE_EMOTION = 2,      // 情绪响应模板
    TEMPLATE_KNOWLEDGE = 3,    // 知识回答模板
    TEMPLATE_CREATIVE = 4,     // 创意联想模板
    TEMPLATE_DEEP = 5          // 深度思考模板
} TemplateType;

// 拓扑类型到模板类型的映射
static TemplateType topo_type_to_template(TopologyType type) {
    switch (type) {
        case TOPO_EMOTION:   return TEMPLATE_EMOTION;
        case TOPO_SEMANTIC:  return TEMPLATE_KNOWLEDGE;
        case TOPO_CONTEXT:   return TEMPLATE_QUESTION;
        case TOPO_CULTURE:   return TEMPLATE_CREATIVE;
        case TOPO_SYNTAX:    return TEMPLATE_DEEP;
        default:             return TEMPLATE_DEFAULT;
    }
}

// 评估输入复杂度，决定联想深度
static int evaluate_input_complexity(const char* text) {
    int len = strlen(text);
    // 检查是否包含问号或感叹号（ASCII）
    int has_question = (strchr(text, '?') != NULL);
    int has_exclamation = (strchr(text, '!') != NULL);

    // 简单输入：短文本或感叹
    if (len < 5 || has_exclamation) return 1;
    // 复杂输入：问句或长文本
    if (has_question || len > 20) return 3;
    // 中等复杂度
    return 2;
}

// 根据模板类型生成回复

// 创建联想引擎
AssociativeEngine* assoc_engine_create(MasterTopology* topology) {
    AssociativeEngine* engine = (AssociativeEngine*)calloc(1, sizeof(AssociativeEngine));
    engine->topology = topology;
    return engine;
}

void assoc_engine_free(AssociativeEngine* engine) {
    free(engine);
}

// 检查节点是否已访问
bool is_visited(AssociativeEngine* engine, int topo_id, int node_id) {
    int key = topo_id * 10000 + node_id;
    for (int i = 0; i < engine->visited_count; i++) {
        if (engine->visited_nodes[i] == key) return true;
    }
    return false;
}

// 标记节点已访问
void mark_visited(AssociativeEngine* engine, int topo_id, int node_id) {
    if (engine->visited_count < 1000) {
        engine->visited_nodes[engine->visited_count++] = topo_id * 10000 + node_id;
    }
}

// 添加联想
void add_association(AssociativeEngine* engine, const char* concept, 
                     float activation, int topo_type, int hop_count) {
    if (engine->assoc_count >= MAX_ASSOCIATIONS) return;
    
    // 检查是否已存在
    for (int i = 0; i < engine->assoc_count; i++) {
        if (strcmp(engine->associations[i].concept, concept) == 0) {
            // 更新激活值（取最大）
            if (activation > engine->associations[i].activation) {
                engine->associations[i].activation = activation;
            }
            return;
        }
    }
    
    // 添加新联想
    strncpy(engine->associations[engine->assoc_count].concept, concept, 255);
    engine->associations[engine->assoc_count].activation = activation;
    engine->associations[engine->assoc_count].topo_type = topo_type;
    engine->associations[engine->assoc_count].hop_count = hop_count;
    engine->assoc_count++;
}

// 递归联想传播
void propagate_association(AssociativeEngine* engine, 
                          int topo_id, int node_id, 
                          float activation, int hop_count, int max_hops) {
    if (hop_count > max_hops) return;
    if (activation < ACTIVATION_THRESHOLD) return;
    if (is_visited(engine, topo_id, node_id)) return;
    
    mark_visited(engine, topo_id, node_id);
    
    // 获取节点概念
    SubTopology* topo = master_get_sub_topology(engine->topology, topo_id);
    if (!topo || !topo->net || node_id >= topo->net->node_count) return;
    
    ReasoningNode* node = topo->net->nodes[node_id];
    if (!node || !node->concept) return;
    
    // 添加到联想列表
    add_association(engine, node->concept, activation, topo->type, hop_count);

    // 通过跨拓扑连接传播（使用 O(1) 邻接表索引）
    int adj_idx = topo_id * 10000 + node_id;
    if (adj_idx < engine->topology->cross_adj_count) {
        CrossTopoAdjEntry* entry = engine->topology->cross_adj[adj_idx];
        while (entry) {
            CrossTopologyLink* link = engine->topology->cross_links[entry->link_index];
            if (link) {
                float new_activation = activation * link->weight * link->transfer_rate;
                propagate_association(engine,
                                     link->to_topo_id, link->to_node_id,
                                     new_activation * DECAY_RATE,
                                     hop_count + 1, max_hops);

                // 反向传播（如果连接是双向的）
                if (link->bidirectional) {
                    propagate_association(engine,
                                         link->from_topo_id, link->from_node_id,
                                         new_activation * DECAY_RATE,
                                         hop_count + 1, max_hops);
                }
            }
            entry = entry->next;
        }
    }

    // 通过拓扑内部连接传播
    if (topo->net && node->connections) {
        for (int i = 0; i < node->connection_count; i++) {
            if (node->connections[i]) {
                float new_activation = activation * node->connection_weights[i];
                propagate_association(engine, topo_id, node->connections[i]->node_id,
                                     new_activation * DECAY_RATE,
                                     hop_count + 1, max_hops);
            }
        }
    }
}

// 从输入文本开始联想（支持动态联想深度）
int associate_from_text(AssociativeEngine* engine, const char* text, int max_hops) {
    engine->assoc_count = 0;
    engine->visited_count = 0;

    // 动态计算联想深度（如果未指定或指定为0）
    if (max_hops <= 0) {
        max_hops = evaluate_input_complexity(text);
    }

    // 分词
    char* tokens[100];
    int token_count = utf8_tokenize(text, tokens, 100);

    // 获取词汇拓扑
    SubTopology* vocab_topo = master_get_sub_topology_by_type(engine->topology, TOPO_VOCABULARY);
    if (!vocab_topo || !vocab_topo->net) {
        for (int i = 0; i < token_count; i++) free(tokens[i]);
        return 0;
    }

    printf("\n[联想推理] 开始从 %d 个词汇联想，动态深度=%d...\n", token_count, max_hops);

    // 对每个词查找并激活（使用 O(1) 哈希查找）
    for (int i = 0; i < token_count; i++) {
        ReasoningNode* node = node_hash_find(vocab_topo->node_hash, tokens[i]);
        if (node) {
            // 找到匹配节点，开始联想传播
            propagate_association(engine, vocab_topo->topo_id, node->node_id,
                               1.0f, 0, max_hops);
        }
    }

    // 释放tokens
    for (int i = 0; i < token_count; i++) {
        free(tokens[i]);
    }

    printf("[联想推理] 共联想出 %d 个概念\n", engine->assoc_count);

    return engine->assoc_count;
}

// 增强的回复生成（支持多种生成策略）
static void generate_enhanced_response(char* result, int max_len,
                                       Association* assocs, int assoc_count,
                                       TemplateType tmpl, float top_activation,
                                       int dominant_topo) {
    (void)dominant_topo;  // 未使用参数
    int pos = 0;
    int count = (assoc_count < 8) ? assoc_count : 8;

    // 多样性：随机添加前缀
    static const char* prefixes[] = {
        "我想", "让我想想", "根据我的理解", "从这个角度看",
        "或许", "可以说", "总的来说", "简而言之"
    };
    int prefix_idx = (int)((assoc_count * 17 + count * 31) % 8);
    const char* prefix = prefixes[prefix_idx];

    switch (tmpl) {
        case TEMPLATE_QUESTION:
            pos = snprintf(result, max_len, "%s，这个问题涉及到", prefix);
            if (top_activation > 0.7f) {
                pos += snprintf(result + pos, max_len - pos, "【%s】", assocs[0].concept);
                if (count > 1) {
                    pos += snprintf(result + pos, max_len - pos, "与【%s】",
                                   assocs[1].concept);
                }
                pos += snprintf(result + pos, max_len - pos, "之间的深层联系。");
            } else {
                for (int i = 0; i < count && pos < max_len - 30; i++) {
                    if (i > 0) pos += snprintf(result + pos, max_len - pos, "、");
                    pos += snprintf(result + pos, max_len - pos, "【%s】", assocs[i].concept);
                }
                pos += snprintf(result + pos, max_len - pos, "等领域。");
            }
            break;

        case TEMPLATE_EMOTION:
            pos = snprintf(result, max_len, "%s能感受到", prefix);
            if (top_activation > 0.6f) {
                pos += snprintf(result + pos, max_len - pos, "【%s】对你很重要",
                               assocs[0].concept);
                if (count >= 2) {
                    pos += snprintf(result + pos, max_len - pos, "，还联想到【%s】",
                                   assocs[1].concept);
                }
                pos += snprintf(result + pos, max_len - pos, "。");
            } else {
                pos += snprintf(result + pos, max_len - pos, "关于");
                for (int i = 0; i < count && pos < max_len - 20; i++) {
                    if (i > 0) pos += snprintf(result + pos, max_len - pos, "和");
                    pos += snprintf(result + pos, max_len - pos, "【%s】", assocs[i].concept);
                }
                pos += snprintf(result + pos, max_len - pos, "。");
            }
            break;

        case TEMPLATE_KNOWLEDGE:
            pos = snprintf(result, max_len, "%s", prefix);
            pos += snprintf(result + pos, max_len - pos, "从知识角度，【%s】",
                           assocs[0].concept);
            if (count > 1) {
                pos += snprintf(result + pos, max_len - pos, "与【%s】存在紧密联系",
                               assocs[1].concept);
            }
            pos += snprintf(result + pos, max_len - pos, "，置信度约%.0f%%。",
                           top_activation * 100);
            if (count > 2 && pos < max_len - 30) {
                pos += snprintf(result + pos, max_len - pos, "同时【%s】也值得关注。",
                               assocs[2].concept);
            }
            break;

        case TEMPLATE_CREATIVE:
            pos = snprintf(result, max_len, "%s产生了有趣的联想：", prefix);
            for (int i = 0; i < count && pos < max_len - 25; i++) {
                if (i > 0) pos += snprintf(result + pos, max_len - pos, " -> ");
                pos += snprintf(result + pos, max_len - pos, "【%s】", assocs[i].concept);
            }
            pos += snprintf(result + pos, max_len - pos, "，这是跨领域的思考！");
            break;

        case TEMPLATE_DEEP:
            pos = snprintf(result, max_len, "%s进行深度分析：", prefix);
            if (count >= 3) {
                pos += snprintf(result + pos, max_len - pos, "【%s】是核心，",
                               assocs[0].concept);
                pos += snprintf(result + pos, max_len - pos, "连接了【%s】与【%s】",
                               assocs[1].concept, assocs[2].concept);
                pos += snprintf(result + pos, max_len - pos, "，形成推理链。");
            } else {
                for (int i = 0; i < count && pos < max_len - 20; i++) {
                    if (i > 0) pos += snprintf(result + pos, max_len - pos, "，");
                    pos += snprintf(result + pos, max_len - pos, "【%s】", assocs[i].concept);
                }
            }
            break;

        case TEMPLATE_DEFAULT:
        default:
            pos = snprintf(result, max_len, "%s，联想到", prefix);
            for (int i = 0; i < count && pos < max_len - 20; i++) {
                if (i > 0) pos += snprintf(result + pos, max_len - pos, "、");
                pos += snprintf(result + pos, max_len - pos, "【%s】", assocs[i].concept);
            }
            pos += snprintf(result + pos, max_len - pos, "。");
            break;
    }
}

// 基于联想结果生成句子（使用增强的模板系统）
char* generate_from_associations(AssociativeEngine* engine, int max_len) {
    if (engine->assoc_count == 0) {
        return strdup("我正在思考...");
    }
    
    // 生成缓存键（基于前3个联想词）
    char cache_key[256] = "";
    int key_len = 0;
    int count = (engine->assoc_count < 3) ? engine->assoc_count : 3;
    for (int i = 0; i < count; i++) {
        key_len += snprintf(cache_key + key_len, sizeof(cache_key) - key_len, 
                          "%s%s", (i > 0 ? ":" : ""), engine->associations[i].concept);
    }
    
    // 查找缓存
    for (int i = 0; i < engine->cache_count; i++) {
        if (strcmp(engine->cache_keys[i], cache_key) == 0) {
            engine->cache_hits[i]++;
            char* cached_result = strdup(engine->cache_values[i]);
            printf("[缓存命中] 键='%s', 命中次数=%d\n", cache_key, engine->cache_hits[i]);
            return cached_result;
        }
    }

    // 按激活值排序
    for (int i = 0; i < engine->assoc_count - 1; i++) {
        for (int j = i + 1; j < engine->assoc_count; j++) {
            if (engine->associations[j].activation > engine->associations[i].activation) {
                Association temp = engine->associations[i];
                engine->associations[i] = engine->associations[j];
                engine->associations[j] = temp;
            }
        }
    }

    // 分配结果缓冲区
    char* result = (char*)malloc(max_len);
    if (!result) return strdup("...");
    result[0] = '\0';

    // 统计各拓扑类型的激活贡献
    float topo_activation[10] = {0};
    int topo_count[10] = {0};

    for (int i = 0; i < engine->assoc_count; i++) {
        int t = engine->associations[i].topo_type;
        if (t >= 0 && t < 10) {
            topo_activation[t] += engine->associations[i].activation;
            topo_count[t]++;
        }
    }

    // 找出主导拓扑类型（激活贡献最大）
    int dominant_topo = 0;
    float max_activation = 0;
    for (int t = 0; t < 10; t++) {
        if (topo_activation[t] > max_activation) {
            max_activation = topo_activation[t];
            dominant_topo = t;
        }
    }

    // 根据主导拓扑选择模板
    TemplateType tmpl = topo_type_to_template((TopologyType)dominant_topo);
    float top_activation = engine->associations[0].activation;

    // 使用增强的模板生成
    generate_enhanced_response(result, max_len,
                             engine->associations, engine->assoc_count,
                             tmpl, top_activation, dominant_topo);

    // 存储到缓存
    int cache_idx = engine->cache_next;
    snprintf(engine->cache_keys[cache_idx], sizeof(engine->cache_keys[cache_idx]), "%s", cache_key);
    snprintf(engine->cache_values[cache_idx], sizeof(engine->cache_values[cache_idx]), "%s", result);
    engine->cache_hits[cache_idx] = 1;
    engine->cache_next = (engine->cache_next + 1) % 20;
    if (engine->cache_count < 20) engine->cache_count++;
    printf("[缓存存储] 键='%s'\n", cache_key);

    return result;
}

// 打印联想路径
void print_associations(AssociativeEngine* engine) {
    printf("\n联想路径:\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    
    for (int i = 0; i < engine->assoc_count && i < 20; i++) {
        Association* assoc = &engine->associations[i];
        const char* topo_name = "未知";
        
        switch (assoc->topo_type) {
            case TOPO_VOCABULARY: topo_name = "词汇"; break;
            case TOPO_SEMANTIC: topo_name = "语义"; break;
            case TOPO_CULTURE: topo_name = "文化"; break;
            case TOPO_SYNTAX: topo_name = "语法"; break;
            case TOPO_EMOTION: topo_name = "情绪"; break;
        }
        
        printf("  [%d] %s: %s (激活=%.3f, 跳数=%d)\n",
               i + 1, topo_name, assoc->concept, 
               assoc->activation, assoc->hop_count);
    }
    
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
}
