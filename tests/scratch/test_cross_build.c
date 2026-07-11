/**
 * @file test_cross_build.c
 * @brief 验证跨拓扑连接重建
 *
 * 编译:
 *   gcc -std=gnu99 -O2 -Iinclude -I. -Ilibs -D_USE_MATH_DEFINES -pthread \
 *       -o build/bin/test_cross_build tools/test_cross_build.c \
 *       src/cross_edge_io.c src/multi_topology.c src/huarong_topology.c \
 *       src/string_pool.c src/node_hash.c src/utf8_tokenizer.c \
 *       src/tensor.c src/common_util.c src/thread_pool.c \
 *       src/cognitive_params.c src/memory_arena.c src/topo_snapshot.c \
 *       -lm -fopenmp 2>&1
 *
 * 用法: ./build/bin/test_cross_build [状态文件]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "multi_topology.h"
#include "cross_edge_io.h"
#include "node_hash.h"

int main(int argc, char* argv[]) {
    const char* state_path = argc > 1 ? argv[1] : "pivotmind_state.dat";

    printf("╔═══════════════════════════════════════════╗\n");
    printf("║  跨拓扑连接重建测试                      ║\n");
    printf("╚═══════════════════════════════════════════╝\n\n");

    // 创建主拓扑
    MasterTopology* master = master_topology_create(10);
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 8000, 10);
    master_add_sub_topology(master, TOPO_SEMANTIC, "语义拓扑", 6000, 9);
    master_add_sub_topology(master, TOPO_EMOTION, "情绪拓扑", 2000, 8);
    master_add_sub_topology(master, TOPO_SYNTAX, "语法拓扑", 500, 7);
    master_add_sub_topology(master, TOPO_CONTEXT, "上下文拓扑", 500, 6);
    master_add_sub_topology(master, TOPO_DOMAIN, "领域拓扑", 500, 5);
    master_add_sub_topology(master, TOPO_PRAGMA, "语用拓扑", 500, 4);
    master_add_sub_topology(master, TOPO_CULTURE, "文化拓扑", 500, 3);
    master_add_sub_topology(master, TOPO_CONCEPT, "概念拓扑", 8000, 9);

    // 加载状态
    printf("[1] 加载拓扑状态...\n");
    int loaded = master_load_state(master, state_path);
    if (loaded <= 0) {
        printf("  × 加载失败或状态为空\n");
        master_topology_destroy(master);
        return 1;
    }
    printf("  ✓ 加载 %d 个节点\n", loaded);

    // 统计各拓扑节点数
    printf("\n[2] 各拓扑节点数:\n");
    int total_nodes = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (sub && sub->net) {
            printf("  %s: %d 节点\n", sub->name, sub->net->node_count);
            total_nodes += sub->net->node_count;
        }
    }
    printf("  总节点: %d\n", total_nodes);
    printf(" 当前跨连接: %d\n", master->cross_link_count);

    // 执行跨连接重建
    printf("\n[3] 执行跨拓扑连接重建...\n");
    clock_t start = clock();
    int rebuilt = rebuild_cross_connections(master);
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    printf("\n  ✓ 创建 %d 条跨拓扑连接 (耗时 %.2f 秒)\n", rebuilt, elapsed);

    // 按拓扑对统计
    printf("\n[4] 跨连接按拓扑分布:\n");
    int type_pairs[10][10] = {{0}};
    for (int i = 0; i < master->cross_link_count; i++) {
        CrossTopologyLink* link = master->cross_links[i];
        if (link) {
            SubTopology* from = master_get_sub_topology(master, link->from_topo_id);
            SubTopology* to = master_get_sub_topology(master, link->to_topo_id);
            if (from && to && from->type < 10 && to->type < 10) {
                type_pairs[from->type][to->type]++;
            }
        }
    }
    for (int f = 0; f < 9; f++) {
        for (int t = 0; t < 9; t++) {
            if (type_pairs[f][t] > 0) {
                printf("  %s→%s: %d\n",
                       TOPOLOGY_TYPE_NAMES[f],
                       TOPOLOGY_TYPE_NAMES[t],
                       type_pairs[f][t]);
            }
        }
    }

    // 保存
    printf("\n[5] 保存跨拓扑连接...\n");
    int saved = save_cross_edges(master, "cross_edges.bin");
    if (saved > 0) {
        printf("  ✓ 已保存 %d 条连接到 cross_edges.bin\n", saved);
    }

    printf("\n╔═══════════════════════════════════════════╗\n");
    printf("║  测试完成！                               ║\n");
    printf("║  跨拓扑连接: %d                            ║\n", master->cross_link_count);
    printf("╚═══════════════════════════════════════════╝\n");

    master_topology_destroy(master);
    return 0;
}
