/**
 * @file enhanced_generator.c
 * @brief 增强的文本生成器 - 基于多拓扑网络激活传播生成内容
 * 
 * 不再依赖硬编码的训练数据，而是利用拓扑网络中已激活的
 * 节点和关系来动态生成回复。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "multi_topology.h"
#include "utf8_tokenizer.h"
#include "huarong_topology.h"

// 激活传播参数
#define ACTIVATION_THRESHOLD 0.3f     // 低于此值的激活视为无意义
#define MAX_RESPONSE_CONCEPTS 8       // 回复中最多使用的概念数
#define ACTIVATION_DECAY 0.5f         // 每跳衰减系数
#define ACTIVATION_SPREAD_HOPS 2      // 激活传播的最大跳数

/**
 * 收集拓扑网络中高于阈值的激活节点
 */
static int collect_activated_concepts(MasterTopology* master,
                                       const char** output_concepts,
                                       float* output_scores,
                                       int max_count) {
    if (!master || !output_concepts || !output_scores) return 0;
    int collected = 0;
    for (int t = 0; t < master->sub_topo_count && collected < max_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        for (int n = 0; n < sub->net->node_count && collected < max_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (!node || !node->concept) continue;
            if (node->activation >= ACTIVATION_THRESHOLD) {
                output_concepts[collected] = node->concept;
                output_scores[collected] = node->activation;
                collected++;
            }
        }
    }
    // 按激活值降序排序
    for (int i = 0; i < collected - 1; i++) {
        for (int j = i + 1; j < collected; j++) {
            if (output_scores[i] < output_scores[j]) {
                float ts = output_scores[i];
                output_scores[i] = output_scores[j];
                output_scores[j] = ts;
                const char* tc = output_concepts[i];
                output_concepts[i] = output_concepts[j];
                output_concepts[j] = tc;
            }
        }
    }
    return collected;
}

/**
 * 从拓扑网络中传播激活
 * 从输入token匹配的节点出发，沿连接多跳传播
 */
static void propagate_activation(MasterTopology* master,
                                  const char** tokens, int token_count) {
    if (!master || !tokens || token_count <= 0) return;

    // 衰减所有节点的现有激活（模拟短期记忆衰减）
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (node) node->activation *= 0.3f;
        }
    }

    // 在词汇拓扑中匹配并激活输入token
    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    if (!vocab || !vocab->net) return;

    for (int i = 0; i < token_count; i++) {
        for (int j = 0; j < vocab->net->node_count; j++) {
            ReasoningNode* node = vocab->net->nodes[j];
            if (!node || !node->concept) continue;
            // 模糊匹配
            if (strstr(tokens[i], node->concept) ||
                strstr(node->concept, tokens[i])) {
                master_activate_node(master, vocab->topo_id, j, 0.9f);
                // 激活后向邻居传播
                for (int c = 0; c < node->connection_count; c++) {
                    ReasoningNode* neighbor = node->connections[c];
                    if (neighbor && neighbor->activation < 0.5f)
                        neighbor->activation = 0.5f * node->connection_weights[c];
                }
                break;
            }
        }
    }

    // 跨拓扑多跳传播
    for (int hop = 0; hop < ACTIVATION_SPREAD_HOPS; hop++) {
        for (int t = 0; t < master->sub_topo_count; t++) {
            SubTopology* sub = master->sub_topologies[t];
            if (!sub || !sub->net) continue;
            for (int n = 0; n < sub->net->node_count; n++) {
                ReasoningNode* node = sub->net->nodes[n];
                if (!node || node->activation < ACTIVATION_THRESHOLD) continue;
                float spread = node->activation * ACTIVATION_DECAY;
                for (int c = 0; c < node->connection_count; c++) {
                    ReasoningNode* neighbor = node->connections[c];
                    if (neighbor && neighbor->activation < spread)
                        neighbor->activation = spread;
                }
            }
        }
    }
}

/**
 * 基于拓扑激活构建回复文本
 */
static void build_activation_response(const char** concepts, float* scores,
                                       int count, char* output, int max_len) {
    if (!concepts || !output || count <= 0) {
        snprintf(output, max_len, "嗯，我在思考...");
        return;
    }

    int pos = 0;
    pos += snprintf(output + pos, max_len - pos, "关于");

    int listed = 0;
    for (int i = 0; i < count && i < 4 && pos < max_len - 50; i++) {
        const char* sep = (listed == 0) ? "" :
                          (i < count - 1 && i < 3) ? "、" : "和";
        if (strlen(concepts[i]) < 20) {
            pos += snprintf(output + pos, max_len - pos, "%s%s", sep, concepts[i]);
            listed++;
        }
    }

    if (listed > 0) {
        pos += snprintf(output + pos, max_len - pos, "，我想到了一些关联：");
    } else {
        pos += snprintf(output + pos, max_len - pos, "这个话题，我正在学习。");
        return;
    }

    // 根据最高激活值决定回复语气
    float max_score = 0.0f;
    for (int i = 0; i < count; i++) {
        if (scores[i] > max_score) max_score = scores[i];
    }

    if (max_score > 0.7f) {
        pos += snprintf(output + pos, max_len - pos,
                        " 其中「%s」让我最感兴趣。", concepts[0]);
    } else if (count >= 2) {
        pos += snprintf(output + pos, max_len - pos,
                        " 「%s」和「%s」之间似乎有某种联系。",
                        concepts[0], concepts[1]);
    }
}

/**
 * 增强的生成函数 - 基于多拓扑网络激活传播
 * 不再依赖硬编码训练数据，而是通过拓扑激活动态生成
 */
char* enhanced_generate(MasterTopology* master, const char* input_text, int max_output_len) {
    if (!master || !input_text) {
        return strdup("我正在学习中...");
    }

    printf("\n===== 增强生成 =====\n");
    printf("输入: %s\n", input_text);

    // 1. UTF-8分词
    char* tokens[100];
    int token_count = utf8_tokenize(input_text, tokens, 100);
    printf("UTF-8分词: %d 个token\n", token_count);
    for (int i = 0; i < token_count && i < 10; i++) {
        printf("  [%d] %s\n", i, tokens[i]);
    }

    // 2. 拓扑网络激活传播
    propagate_activation(master, (const char**)tokens, token_count);

    // 3. 收集激活的概念
    const char* activated_concepts[MAX_RESPONSE_CONCEPTS];
    float activated_scores[MAX_RESPONSE_CONCEPTS];
    int activated_count = collect_activated_concepts(
        master, activated_concepts, activated_scores, MAX_RESPONSE_CONCEPTS);

    printf("激活的概念: %d 个\n", activated_count);
    for (int i = 0; i < activated_count && i < 8; i++) {
        printf("  [%.2f] %s\n", activated_scores[i], activated_concepts[i]);
    }

    // 4. 构建回复
    char* response = (char*)malloc(max_output_len);
    if (!response) {
        for (int i = 0; i < token_count; i++) free(tokens[i]);
        return strdup("生成失败");
    }

    build_activation_response(activated_concepts, activated_scores,
                               activated_count, response, max_output_len);

    // 释放tokens
    for (int i = 0; i < token_count; i++) {
        free(tokens[i]);
    }

    printf("\n生成结果:\n%s\n", response);
    printf("======================\n");

    return response;
}
