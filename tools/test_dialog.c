/**
 * @file test_dialog.c
 * @brief 对话测试工具 — 加载训练好的拓扑状态 + 特征向量 + 跨连接
 * 
 * 用法: ./build/bin/test_dialog [state_file] [input_text]
 *       默认: pivotmind_state.dat "你好"
 * 
 * 编译: gcc -std=gnu99 -O2 -Iinclude -I. -Ilibs -I/usr/include
 *        -o build/bin/test_dialog tools/test_dialog.c src/*.c -lm -pthread -fopenmp
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "multi_topology.h"
#include "common.h"
#include "feature_io.h"
#include "cross_edge_io.h"

int main(int argc, char* argv[]) {
    const char* state_file = argc > 1 ? argv[1] : "pivotmind_state.dat";
    const char* input_text = argc > 2 ? argv[2] : "你好";
    int max_output = 50;

    printf("╔══════════════════════════════════════════╗\n");
    printf("║      玄枢 对话测试工具 v1.0              ║\n");
    printf("╚══════════════════════════════════════════╝\n\n");

    // 1. 创建主拓扑
    printf("[1/5] 创建多拓扑认知网络...\n");
    MasterTopology* master = master_topology_create(9);
    if (!master) {
        printf("错误: 无法创建主拓扑\n");
        return 1;
    }

    // 添加子拓扑（与 digital_life 一致）
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 6000, 10);
    master_add_sub_topology(master, TOPO_SEMANTIC, "语义拓扑", 2000, 9);
    master_add_sub_topology(master, TOPO_EMOTION, "情绪拓扑", 500, 8);
    master_add_sub_topology(master, TOPO_SYNTAX, "语法拓扑", 500, 7);
    master_add_sub_topology(master, TOPO_CONTEXT, "上下文拓扑", 500, 6);
    master_add_sub_topology(master, TOPO_DOMAIN, "领域拓扑", 500, 5);
    master_add_sub_topology(master, TOPO_PRAGMA, "语用拓扑", 500, 4);
    master_add_sub_topology(master, TOPO_CULTURE, "文化拓扑", 500, 3);
    master_add_sub_topology(master, TOPO_CONCEPT, "概念拓扑", 6000, 9);

    printf("     ✓ 认知网络就绪 (9 个拓扑)\n");

    // 2. 加载拓扑状态
    printf("[2/5] 加载拓扑状态...\n");
    if (access(state_file, F_OK) != 0) {
        printf("错误: 状态文件 %s 不存在\n", state_file);
        master_topology_destroy(master);
        return 1;
    }
    int loaded = master_load_state(master, state_file);
    if (loaded < 0) {
        printf("错误: 无法加载状态\n");
        master_topology_destroy(master);
        return 1;
    }
    printf("     ✓ 已加载拓扑状态 (%d 节点)\n", loaded);

    // 3. 加载/初始化特征向量
    printf("[3/5] 加载特征向量...\n");
    int feat_loaded = load_features(master, "features.bin");
    int features_were_reinit = 0;
    if (feat_loaded > 0) {
        printf("     ✓ 已加载特征向量 (%d 节点)\n", feat_loaded);
    } else {
        int initted = init_random_features(master);
        printf("     ✓ 已初始化特征向量 (%d 节点)\n", initted);
        features_were_reinit = 1;
    }

    // 4. 加载/重建跨拓扑连接
    printf("[4/5] 加载跨拓扑连接...\n");
    int cross_loaded = load_cross_edges(master, "cross_edges.bin");
    if (cross_loaded > 0 && !features_were_reinit) {
        printf("     ✓ 已加载跨拓扑连接 (%d 条)\n", cross_loaded);
    } else {
        int rebuilt = rebuild_cross_connections(master);
        printf("     ✓ 已重建跨拓扑连接 (%d 条)\n", rebuilt);
        // 保存跨连接，下次可直接加载
        save_cross_edges(master, "cross_edges.bin");
    }

    // 保存特征向量
    save_features(master, "features.bin");

    // 5. 生成回复
    printf("[5/5] 生成回复...\n\n");
    printf("输入: \"%s\"\n\n", input_text);

    char* response = master_generate_response(master, input_text, max_output);

    if (response) {
        printf(">>> 输出: \"%s\"\n", response);
        printf("     (长度: %zu 字符)\n\n", strlen(response));
        free(response);
    } else {
        printf(">>> 输出: (空)\n\n");
    }

    // 打印拓扑统计
    {
        int total_nodes = 0, total_links = 0;
        float avg_act = 0;
        master_get_system_status(master, &total_nodes, &total_links, &avg_act);
        printf("统计: %d 节点, %d 内部边, %d 跨连接\n",
               total_nodes, total_links, master->cross_link_count);
    }

    master_topology_destroy(master);
    printf("\n✓ 测试完成\n");
    return 0;
}
