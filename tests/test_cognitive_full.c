/**
 * @file test_cognitive_full.c
 * @brief 认知调度中心完整测试 — 使用训练好的状态
 *
 * 测试内容：
 * 1. 反复问同一问题 — 看 retry 三级降级
 * 2. 多轮不同话题 — 看 intent_weights 调度
 * 3. 模糊输入 — 看优雅降级
 * 4. 高阈值 — 强制触发三级降级
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dialog_system.h"
#include "multi_topology.h"
#include "memory_system.h"
#include "active_learner.h"
#include "cognitive_controller.h"

static const char* STATE_FILE = "pivotmind_state.dat";

static DialogSystem* create_loaded_system(void) {
    MemorySystem* memory = memory_system_create(500, 2000, 5000);
    if (!memory) return NULL;

    MasterTopology* topology = master_topology_create(9);
    if (!topology) { memory_system_destroy(memory); return NULL; }

    master_add_sub_topology(topology, TOPO_VOCABULARY, "词汇拓扑", 6000, 10);
    master_add_sub_topology(topology, TOPO_SEMANTIC, "语义拓扑", 2000, 9);
    master_add_sub_topology(topology, TOPO_EMOTION, "情绪拓扑", 500, 8);
    master_add_sub_topology(topology, TOPO_SYNTAX, "语法拓扑", 500, 7);
    master_add_sub_topology(topology, TOPO_CONTEXT, "上下文拓扑", 500, 6);
    master_add_sub_topology(topology, TOPO_DOMAIN, "领域拓扑", 500, 5);
    master_add_sub_topology(topology, TOPO_PRAGMA, "语用拓扑", 500, 4);
    master_add_sub_topology(topology, TOPO_CULTURE, "文化拓扑", 500, 3);
    master_add_sub_topology(topology, TOPO_CONCEPT, "概念拓扑", 6000, 9);

    int loaded = master_load_state(topology, STATE_FILE);
    if (loaded <= 0) {
        printf("  × 状态加载失败 (%d)\n", loaded);
        master_topology_destroy(topology);
        memory_system_destroy(memory);
        return NULL;
    }
    printf("  ✓ 已加载 %d 个节点\n", loaded);

    // 统计各拓扑
    int total_nodes = 0, total_edges = 0;
    for (int t = 0; t < topology->sub_topo_count; t++) {
        SubTopology* sub = topology->sub_topologies[t];
        if (!sub || !sub->net) continue;
        total_nodes += sub->net->node_count;
        int e = 0;
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (node) e += node->connection_count;
        }
        total_edges += e / 2;
        printf("    %s: %d 节点, ~%d 边\n",
               sub->name, sub->net->node_count, e / 2);
    }
    printf("  ─── 总计: %d 节点, ~%d 边\n", total_nodes, total_edges);

    CausalGraph* causal_graph = causal_graph_create(1000, 5000);
    if (!causal_graph) { master_topology_destroy(topology); memory_system_destroy(memory); return NULL; }

    ActiveLearner* learner = active_learner_create(topology, memory);
    if (!learner) { causal_graph_destroy(causal_graph); master_topology_destroy(topology); memory_system_destroy(memory); return NULL; }

    DialogSystem* dialog = dialog_system_create(topology, memory, causal_graph, learner);
    if (!dialog) { active_learner_destroy(learner); causal_graph_destroy(causal_graph); master_topology_destroy(topology); memory_system_destroy(memory); return NULL; }

    return dialog;
}

int main(void) {
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║  CognitiveController 完整测试 — 训练数据版本       ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");
    fflush(stdout);

    DialogSystem* dialog = create_loaded_system();
    if (!dialog) {
        printf("FATAL: 创建系统失败\n");
        return 1;
    }
    printf("\n系统就绪! 认知调度器: %s\n\n", dialog->controller ? "已激活" : "未激活");
    fflush(stdout);

    // ==================== 测试1: 反复问同一问题 ====================
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("测试1: 反复问「人工智能是什么」× 3\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");
    fflush(stdout);

    for (int i = 0; i < 3; i++) {
        printf(">>> [第%d轮]\n", i + 1);
        if (dialog->controller) {
            printf("    调度快照: ");
            int c = 0;
            for (int t = 0; t < MAX_SUBTOPOS; t++) {
                if (dialog->controller->prev_intent_weights[t] > 0.01f) {
                    if (c++ > 0) printf(", ");
                    printf("%s=%.3f", cognitive_controller_topo_name(t),
                           dialog->controller->prev_intent_weights[t]);
                }
            }
            printf("\n");
        }
        fflush(stdout);

        DialogReasoning* reasoning = NULL;
        char* response = dialog_process(dialog, "人工智能是什么", &reasoning);
        if (response) {
            printf("<<< %s\n\n", response);
            free(response);
        }
        if (reasoning) dialog_reasoning_destroy(reasoning);
        fflush(stdout);
    }

    // ==================== 测试2: 多轮不同话题 ====================
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("测试2: 多轮不同话题 — 看调度变化\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");
    fflush(stdout);

    const char* topics[] = {"你好", "学习", "计算机", "芯片"};
    for (int i = 0; i < 4; i++) {
        printf(">>> [话题%d] %s\n", i + 1, topics[i]);
        DialogReasoning* reasoning = NULL;
        char* response = dialog_process(dialog, topics[i], &reasoning);
        if (response) {
            printf("<<< %s\n\n", response);
            free(response);
        }
        if (reasoning) dialog_reasoning_destroy(reasoning);
        fflush(stdout);
    }

    // ==================== 测试3: 模糊输入 ====================
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("测试3: 模糊输入压力\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");
    fflush(stdout);

    const char* obscure[] = {"量子纠缠", "薛定谔的猫", "ABCDEFG"};
    for (int i = 0; i < 3; i++) {
        printf(">>> [模糊%d] %s\n", i + 1, obscure[i]);
        DialogReasoning* reasoning = NULL;
        char* response = dialog_process(dialog, obscure[i], &reasoning);
        if (response) {
            printf("<<< %s\n\n", response);
            free(response);
        }
        if (reasoning) dialog_reasoning_destroy(reasoning);
        fflush(stdout);
    }

    // ==================== 测试4: 强制三级降级 ====================
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("测试4: 高阈值强制 retry — 看三级降级链\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");
    fflush(stdout);

    float orig_threshold = dialog->controller->satisfaction_threshold;
    dialog->controller->satisfaction_threshold = 0.95f;
    printf("    (阈值 0.95, 当前满意度约 0.5-0.8)\n");
    fflush(stdout);

    DialogReasoning* reasoning = NULL;
    char* response = dialog_process(dialog, "人工智能是什么", &reasoning);
    if (response) {
        printf("<<< %s\n", response);
        free(response);
    }
    if (reasoning) dialog_reasoning_destroy(reasoning);
    fflush(stdout);

    dialog->controller->satisfaction_threshold = orig_threshold;

    // ==================== 总结 ====================
    printf("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("测试完成 — 无崩溃\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    fflush(stdout);

    ActiveLearner* learner = dialog->learner;
    CausalGraph* graph = dialog->causal_graph;
    MasterTopology* topology = dialog->master;
    MemorySystem* memory = dialog->memory;
    dialog_system_destroy(dialog);
    if (learner) active_learner_destroy(learner);
    if (graph) causal_graph_destroy(graph);
    if (topology) master_topology_destroy(topology);
    if (memory) memory_system_destroy(memory);
    printf("已清理.\n");
    return 0;
}
