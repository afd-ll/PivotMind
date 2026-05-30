/**
 * @file build_cross_links.c
 * @brief 批量创建跨拓扑连接 — 遍历词汇拓扑节点，在语义/情绪/概念拓扑创建对应节点并建跨连接
 *
 * 用法: ./build/bin/build_cross_links [状态文件]
 *       默认: pivotmind_state.dat
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "multi_topology.h"
#include "node_hash.h"

int main(int argc, char* argv[]) {
    const char* state_path = argc > 1 ? argv[1] : "pivotmind_state.dat";
    
    printf("╔═══════════════════════════════════════════╗\n");
    printf("║    跨拓扑连接构建工具 v1.0                ║\n");
    printf("╚═══════════════════════════════════════════╝\n\n");
    
    // 创建拓扑
    MasterTopology* master = master_topology_create(10);
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 8000, 10);
    master_add_sub_topology(master, TOPO_SEMANTIC, "语义拓扑", 6000, 9);
    master_add_sub_topology(master, TOPO_EMOTION, "情绪拓扑", 2000, 8);
    master_add_sub_topology(master, TOPO_CONCEPT, "概念拓扑", 8000, 9);
    
    // 加载状态
    printf("[1/3] 加载拓扑状态...\n");
    FILE* test = fopen(state_path, "rb");
    if (!test) {
        printf("  × 找不到状态文件: %s\n", state_path);
        master_topology_destroy(master);
        return 1;
    }
    fclose(test);
    
    int loaded = master_load_state(master, state_path);
    if (loaded <= 0) {
        printf("  × 加载失败\n");
        master_topology_destroy(master);
        return 1;
    }
    printf("  ✓ 已加载 %d 节点\n", loaded);
    
    // 获取各拓扑
    SubTopology* vocab = NULL;
    SubTopology* semantic = NULL;
    SubTopology* emotion = NULL;
    SubTopology* concept = NULL;
    
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub) continue;
        switch (sub->type) {
            case TOPO_VOCABULARY: vocab = sub; break;
            case TOPO_SEMANTIC: semantic = sub; break;
            case TOPO_EMOTION: emotion = sub; break;
            case TOPO_CONCEPT: concept = sub; break;
            default: break;
        }
    }
    
    if (!vocab || !vocab->net) {
        printf("  × 词汇拓扑不可用\n");
        master_topology_destroy(master);
        return 1;
    }
    
    printf("  词汇: %d节点\n", vocab->net->node_count);
    printf("  语义: %d节点\n", semantic ? semantic->net->node_count : 0);
    printf("  情绪: %d节点\n", emotion ? emotion->net->node_count : 0);
    printf("  概念: %d节点\n", concept ? concept->net->node_count : 0);
    printf("  当前跨拓扑连接: %d\n", master->cross_link_count);
    
    // [2/3] 创建跨拓扑连接
    printf("\n[2/3] 创建跨拓扑连接...\n");
    int total_cross = 0;
    const float CROSS_WEIGHT = 0.5f;
    
    // 目标拓扑列表：语义、情绪、概念
    SubTopology* targets[] = {semantic, emotion, concept};
    const char* target_names[] = {"语义", "情绪", "概念"};
    float target_weights[] = {0.55f, 0.45f, 0.50f};
    int num_targets = 3;
    
    for (int ti = 0; ti < num_targets; ti++) {
        SubTopology* tgt = targets[ti];
        if (!tgt || !tgt->net) continue;
        
        int created = 0;
        // 遍历词汇拓扑的每个节点
        for (int vi = 0; vi < vocab->net->node_count; vi++) {
            ReasoningNode* vn = vocab->net->nodes[vi];
            if (!vn || !vn->concept) continue;
            
            // 在目标拓扑中查找同名节点
            ReasoningNode* tn = NULL;
            for (int ti2 = 0; ti2 < tgt->net->node_count; ti2++) {
                ReasoningNode* candidate = tgt->net->nodes[ti2];
                if (candidate && candidate->concept && 
                    strcmp(candidate->concept, vn->concept) == 0) {
                    tn = candidate;
                    break;
                }
            }
            
            // 如果目标拓扑中没有同名节点，创建一个
            if (!tn) {
                tn = huarong_net_add_node(tgt->net, vn->concept, NULL, 0);
                if (!tn) continue;
            }
            
            // 创建词汇→目标 跨拓扑连接
            int ret = master_add_cross_link(master,
                                            vocab->topo_id, vn->node_id,
                                            tgt->topo_id, tn->node_id,
                                            target_weights[ti],
                                            target_names[ti]);
            if (ret == 0) created++;
            
            // 双向连接也让语义/情绪/概念可以回到词汇
            if (target_weights[ti] >= 0.4f) {
                master_add_cross_link(master,
                                      tgt->topo_id, tn->node_id,
                                      vocab->topo_id, vn->node_id,
                                      target_weights[ti] * 0.8f,
                                      target_names[ti]);
                created++;
            }
        }
        printf("  %s → 词汇: %d 条双向连接\n", target_names[ti], created / 2);
        total_cross += created;
    }
    
    printf("\n[3/3] 保存状态...\n");
    // 备份旧状态
    char bak[520];
    snprintf(bak, 519, "%s.cross_bak", state_path);
    remove(bak);
    rename(state_path, bak);
    
    int saved = master_save_state(master, state_path);
    if (saved > 0) {
        printf("  ✓ 已保存 %d 节点, %d 跨拓扑连接到 %s\n", saved, master->cross_link_count, state_path);
    }
    
    printf("\n╔═══════════════════════════════════════════╗\n");
    printf("║  完成！                                   ║\n");
    printf("╠═══════════════════════════════════════════╣\n");
    printf("║  跨拓扑连接: %d                            ║\n", master->cross_link_count);
    printf("╚═══════════════════════════════════════════╝\n");
    
    master_topology_destroy(master);
    return 0;
}
