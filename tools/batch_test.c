/**
 * batch_test.c - 批量对话测试，只加载一次state
 * 用法: batch_test.exe state_file
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "multi_topology.h"
#include "common.h"
#include "feature_io.h"
#include "cross_edge_io.h"
#include "template_builder.h"

static const char* tests[] = {
    "你好",
    "什么是人工智能",
    "今天天气怎么样",
    "1+1等于几",
    "你好吗",
    "你是谁",
    "解释一下量子力学",
    "什么是机器学习",
    "如何学好编程",
    "生命的意义是什么",
    NULL
};

int main(int argc, char* argv[]) {
    const char* state_file = argc > 1 ? argv[1] : "pivotmind_state.dat";
    init_random();

    printf("=== 批量对话测试 ===\n\n");

    // 创建并加载
    MasterTopology* master = master_topology_create(12);
    if (!master) { printf("错误: 无法创建主拓扑\n"); return 1; }

    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 8000, 10);
    master_add_sub_topology(master, TOPO_SEMANTIC, "语义拓扑", 8000, 9);
    master_add_sub_topology(master, TOPO_EMOTION, "情绪拓扑", 4000, 8);
    master_add_sub_topology(master, TOPO_SYNTAX, "语法拓扑", 1000, 7);
    master_add_sub_topology(master, TOPO_CONTEXT, "上下文拓扑", 1000, 6);
    master_add_sub_topology(master, TOPO_DOMAIN, "领域拓扑", 1000, 5);
    master_add_sub_topology(master, TOPO_PRAGMA, "语用拓扑", 1000, 4);
    master_add_sub_topology(master, TOPO_CULTURE, "文化拓扑", 1000, 3);
    master_add_sub_topology(master, TOPO_CONCEPT, "概念拓扑", 8000, 9);
    master_add_sub_topology(master, TOPO_MASTER, "主拓扑", 100, 0);
    master_add_sub_topology(master, TOPO_TEMPLATE, "模板拓扑", 2000, 8);

    printf("[加载] 读取状态文件...\n");
    int loaded = master_load_state(master, state_file);
    if (loaded < 0) { printf("错误: 加载失败\n"); return 1; }
    printf("[加载] %d 节点已就绪\n\n", loaded);

    load_cross_edges(master, "cross_edges.bin");
    template_auto_build(master, 500, 100);

    // 批量测试
    for (int i = 0; tests[i]; i++) {
        printf("Q: %s\n", tests[i]);
        char* resp = master_generate_response(master, tests[i], 50);
        if (resp) {
            printf("A: %s\n\n", resp);
            free(resp);
        } else {
            printf("A: (空)\n\n");
        }
    }

    // 统计
    int total_edges = 0, total_nodes = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        total_nodes += sub->net->node_count;
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (node) total_edges += node->connection_count;
        }
    }
    printf("=== 统计: %d 节点, %d 边, %d 跨连接 ===\n",
           total_nodes, total_edges / 2, master->cross_link_count);

    master_topology_destroy(master);
    return 0;
}
