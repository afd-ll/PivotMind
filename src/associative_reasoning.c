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

// 导入拓扑类型映射（仅保留用于类型检测）
static TemplateType topo_type_to_template(TopologyType type) {
    switch (type) {
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

// 基于联想结果生成句子（纯生成式，无模板）
char* generate_from_associations(AssociativeEngine* engine, int max_len) {
    if (engine->assoc_count == 0) {
        return strdup("...");
    }

    // 按激活值排序（找最佳起点）
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
    char* result = (char*)calloc(max_len, 1);
    if (!result) return strdup("...");

    // ===== 走边生成路径 =====
    // 从最高激活节点出发，沿边贪心走到下一个最优节点
    // 六个维度评分：边(权重+置信+动机) + 节点(激活+置信+效价)
    int pos = 0;
    unsigned char* global_visited_bitmap = NULL;  // 跨起点共享
    int max_path = 20;  // 最多走20步

    for (int start_i = 0; start_i < engine->assoc_count && start_i < 5 && pos < max_len - 10; start_i++) {
        Association* assoc = &engine->associations[start_i];

        // 跳过激活值太低的起点
        if (assoc->activation < 0.1f) continue;

        // 在对应子拓扑中查找此概念对应的节点
        SubTopology* sub = master_get_sub_topology_by_type(
            engine->topology, assoc->topo_type);
        if (!sub || !sub->net || !sub->node_hash) continue;

        ReasoningNode* start_node = node_hash_find(sub->node_hash, assoc->concept);
        if (!start_node) continue;

        int start_id = start_node->node_id;
        if (start_id < 0 || start_id >= sub->net->node_count) continue;

        // 如果已被之前的起点走过，跳过
        if (global_visited_bitmap) {
            int bm_size = (sub->net->node_count + 7) / 8;
            if (start_id < bm_size * 8) {
                if (global_visited_bitmap[start_id / 8] & (unsigned char)(1 << (start_id % 8)))
                    continue;
            }
        }

        // 贪心走边
        int path_nodes[32];
        float path_scores[32];
        int path_len = topology_walk_greedy(
            sub, start_id, path_nodes, path_scores,
            max_path < max_len ? max_path : max_len,
            global_visited_bitmap);

        if (path_len <= 1) continue;  // 只有起点，无有效路径

        // 收集全局 visited 位图（跨起点重用）
        if (!global_visited_bitmap && path_len > 1) {
            int bm_size = (sub->net->node_count + 7) / 8;
            global_visited_bitmap = (unsigned char*)calloc(bm_size, 1);
            if (global_visited_bitmap) {
                // 重走一遍路径，把走过的节点都标记上
                for (int s = 0; s < start_i; s++) {
                    Association* prev = &engine->associations[s];
                    SubTopology* prev_sub = master_get_sub_topology_by_type(
                        engine->topology, prev->topo_type);
                    if (prev_sub && prev_sub->node_hash) {
                        ReasoningNode* pn = node_hash_find(prev_sub->node_hash, prev->concept);
                        if (pn && pn->node_id >= 0 && pn->node_id < prev_sub->net->node_count) {
                            global_visited_bitmap[pn->node_id / 8] |=
                                (unsigned char)(1 << (pn->node_id % 8));
                        }
                    }
                }
            }
        }

        // 将路径转换输出
        // 路径[0]是起点（已在输出中或不需要），从路径[1]开始是走边结果
        for (int p = 1; p < path_len && pos < max_len - 10; p++) {
            int nid = path_nodes[p];
            if (nid < 0 || nid >= sub->net->node_count) continue;
            ReasoningNode* node = sub->net->nodes[nid];
            if (!node || !node->concept) continue;

            pos += snprintf(result + pos, max_len - pos, "%s", node->concept);
        }

        // 如果已经生成了足够长的输出，不再尝试更多起点
        if (pos >= 5) break;
    }

    if (global_visited_bitmap) free(global_visited_bitmap);

    // 兜底：如果走边没走出任何结果，输出最高激活的概念
    if (pos == 0 && engine->assoc_count > 0) {
        snprintf(result, max_len, "%s", engine->associations[0].concept);
    }

    return result;
}

void print_associations(AssociativeEngine* engine) {
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
