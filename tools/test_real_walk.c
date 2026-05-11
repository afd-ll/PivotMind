/**
 * 真实数据走边测试
 * 加载训练好的模型，用 topology_walk_greedy 测具体路径
 */
#include "multi_topology.h"
#include "node_hash.h"
#include "huarong_topology.h"
#include "utf8_tokenizer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 全局状态文件路径
#define STATE_FILE "pivotmind_state.dat"

static void test_real_walk(MasterTopology* master, const char* start_concept, int topo_type) {
    SubTopology* sub = master_get_sub_topology_by_type(master, topo_type);
    if (!sub || !sub->net || !sub->node_hash) {
        printf("  [SKIP] 子拓扑不存在\n");
        return;
    }

    ReasoningNode* start = node_hash_find(sub->node_hash, start_concept);
    if (!start) {
        printf("  [SKIP] 找不到概念「%s」\n", start_concept);
        return;
    }

    if (start->connection_count == 0) {
        printf("  [信息] 「%s」没有出边\n", start_concept);
    } else {
        printf("  [信息] 「%s」有 %d 条出边\n", start_concept, start->connection_count);
        // 打印前5条最强的边
        printf("  最强出边:\n");
        int printed = 0;
        for (int i = 0; i < start->connection_count && printed < 5; i++) {
            ReasoningNode* target = start->connections[i];
            if (!target || !target->concept) continue;
            float score = 0.28f * start->connection_weights[i]
                        + 0.22f * (start->connection_confidences ? start->connection_confidences[i] : start->connection_weights[i])
                        + 0.11f * (start->connection_motivational_bias ? start->connection_motivational_bias[i] : 0.0f)
                        + 0.28f * target->activation
                        + 0.11f * target->confidence;
            float vmod = 1.0f + 0.6f * target->valence;
            score *= vmod;
            printf("    → %s (权重=%.3f conf=%.3f 目标激活=%.3f 综合=%.4f)\n",
                   target->concept,
                   start->connection_weights[i],
                   start->connection_confidences ? start->connection_confidences[i] : 0,
                   target->activation,
                   score);
            printed++;
        }
    }

    // 走边测试
    int path[32];
    float scores[32];
    int len = topology_walk_greedy(sub, start->node_id, path, scores, 20, NULL);

    printf("  [走边] 从「%s」出发:", start_concept);
    for (int i = 0; i < len; i++) {
        int nid = path[i];
        if (nid >= 0 && nid < sub->net->node_count && sub->net->nodes[nid]) {
            printf("%s", sub->net->nodes[nid]->concept);
            if (i < len - 1) printf("→");
        }
    }
    printf(" (长度=%d)\n", len);
}

int main() {
    printf("=== 真实数据走边测试 ===\n\n");

    // 加载状态
    MasterTopology* master = master_topology_create(10);
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 10000, 10);
    master_add_sub_topology(master, TOPO_SEMANTIC, "语义拓扑", 10000, 8);
    master_add_sub_topology(master, TOPO_EMOTION, "情绪拓扑", 1000, 6);
    master_add_sub_topology(master, TOPO_SYNTAX, "语法拓扑", 1000, 7);
    master_add_sub_topology(master, TOPO_CONTEXT, "上下文拓扑", 1000, 5);
    master_add_sub_topology(master, TOPO_CULTURE, "文化拓扑", 1000, 4);

    int loaded = master_load_state(master, STATE_FILE);
    if (!loaded) {
        printf("⚠️  状态文件加载失败，请先训练或确认路径\n");
        master_topology_destroy(master);
        return 1;
    }
    printf("状态加载成功\n\n");

    // 获取各子拓扑统计
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (sub && sub->net) {
            int edge_count = 0;
            for (int n = 0; n < sub->net->node_count; n++) {
                if (sub->net->nodes[n])
                    edge_count += sub->net->nodes[n]->connection_count;
            }
            printf("  %s: %d 节点, %d 边\n",
                   sub->name ? sub->name : "?",
                   sub->net->node_count, edge_count);
        }
    }
    printf("\n");

    // ===== 测试核心序列 =====
    printf("--- 测试: 人工智能是什么 ---\n");
    test_real_walk(master, "人", TOPO_VOCABULARY);
    test_real_walk(master, "工", TOPO_VOCABULARY);
    test_real_walk(master, "智", TOPO_VOCABULARY);
    test_real_walk(master, "能", TOPO_VOCABULARY);
    test_real_walk(master, "是", TOPO_VOCABULARY);
    test_real_walk(master, "什", TOPO_VOCABULARY);
    test_real_walk(master, "么", TOPO_VOCABULARY);

    printf("\n--- 测试: 学习 ---\n");
    test_real_walk(master, "学", TOPO_VOCABULARY);
    test_real_walk(master, "习", TOPO_VOCABULARY);

    printf("\n--- 测试: 机器 ---\n");
    test_real_walk(master, "机", TOPO_VOCABULARY);
    test_real_walk(master, "器", TOPO_VOCABULARY);

    printf("\n--- 语义拓扑测试 ---\n");
    test_real_walk(master, "人工智能", TOPO_SEMANTIC);
    test_real_walk(master, "学习", TOPO_SEMANTIC);

    printf("\n--- 多概念起点测试（语义拓扑）---\n");
    // 模拟联想引擎的起始方式：从最高激活节点开始
    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    if (vocab && vocab->net) {
        // 找激活最高的5个节点
        printf("词汇拓扑中激活最高的节点:\n");
        int top_n = 0;
        for (int n = 0; n < vocab->net->node_count && top_n < 10; n++) {
            ReasoningNode* node = vocab->net->nodes[n];
            if (node && node->activation > 0.1f) {
                printf("  [act=%.3f] %s\n", node->activation, node->concept);
                top_n++;
            }
        }
        if (top_n == 0) printf("  (无高激活节点)\n");
    }

    printf("\n--- 激活所有测试概念后再走边 ---\n");
    // 模拟推理后的激活状态：激活"人""工""智""能""是""什""么"
    const char* test_concepts[] = {"人","工","智","能","是","什","么","学","习","机","器"};
    if (vocab && vocab->node_hash) {
        for (int i = 0; i < 11; i++) {
            ReasoningNode* node = node_hash_find(vocab->node_hash, test_concepts[i]);
            if (node) {
                node->activation = 0.9f - i * 0.05f;
            }
        }
    }

    test_real_walk(master, "人", TOPO_VOCABULARY);
    test_real_walk(master, "智", TOPO_VOCABULARY);

    master_topology_destroy(master);
    return 0;
}
