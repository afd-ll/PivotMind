/**
 * @file test_cognitive_full.c
 * @brief 认知调度中心完整测试 — 使用 cwd 相对路径的训练态 pivotmind_state.dat
 *
 * 测试内容（4 组对话，共 11 轮）：
 * 1. 反复问同一问题 — 看 retry 三级降级
 * 2. 多轮不同话题 — 看 intent_weights 调度
 * 3. 模糊输入 — 看优雅降级
 * 4. 高阈值 — 强制触发三级降级
 *
 * ⚠️ 依赖环境：本文件从 **当前工作目录** 读取 `pivotmind_state.dat`（训练态）。
 *    无状态文件时**显式 SKIP**（见 main 顶部），不静默通过、不伪造结果。
 *
 * ⚠️ 断言策略（进 TEST_BINS 的前提）：
 *    训练态的具体数值（各拓扑节点/边数）随训练过程变化，**严禁**对其做数值断言
 *    （必然 flaky）。本文件只断言**结构性不变量** —— 与训练数值无关的契约：
 *      A. 加载后子拓扑数与种子一致（9）——依据：master_load_state 只遍历既有
 *         `sub_topo_count` 填充节点，从不调用 master_add_sub_topology
 *         （src/multi_topology.c:4826 起全程无新增子拓扑）。
 *      B. 各子拓扑 node_count 不超容量 max_nodes —— 依据：net->nodes[] 为
 *         ReasoningNode*[max_nodes]，node_count 恒 <= max_nodes（结构性恒真）。
 *      C. 状态确实被物化：total_nodes > 0（loaded > 0 的加载不得是空操作）。
 *      D. 认知调度器存在（dialog_system_create 无条件创建，src/dialog_system.c:1201）。
 *      E. 4 组对话**每一轮**回复非 NULL / 非 "(null)" 哨兵 / 非空串
 *         —— 依据 dialog_generate 全出口兜底 strdup（src/dialog_generate.c:439）
 *         与 dialog_process 契约；"(null)" 不得泄露到用户侧。
 *    任一不成立 → g_failures++，main 末尾 return 1（Makefile test: 以退出码判成败）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "dialog_system.h"
#include "multi_topology.h"
#include "memory_system.h"
#include "active_learner.h"
#include "cognitive_controller.h"

static const char* STATE_FILE = "pivotmind_state.dat";

/* ================================================================
 *  断言框架 — 失败计数 + main 末尾据此返回非 0
 * ================================================================ */
static int g_failures = 0;

#define CHECK_EQ_INT(actual, expected, msg)                                     \
    do {                                                                        \
        long _a = (long)(actual), _e = (long)(expected);                        \
        if (_a != _e) {                                                         \
            printf("  x FAIL: %s (实际 %ld, 期望 %ld)\n", (msg), _a, _e);       \
            g_failures++;                                                       \
        } else {                                                                \
            printf("  + %s = %ld\n", (msg), _a);                                \
        }                                                                       \
    } while (0)

#define CHECK_TRUE(cond, msg)                                                   \
    do {                                                                        \
        if (!(cond)) {                                                          \
            printf("  x FAIL: %s\n", (msg));                                    \
            g_failures++;                                                       \
        } else {                                                                \
            printf("  + %s\n", (msg));                                          \
        }                                                                       \
    } while (0)

/* 校验单轮回复：非 NULL / 非 "(null)" 哨兵 / 非空串。
 * 依据 dialog_process 契约与 dialog_generate 全出口 strdup 兜底
 * （src/dialog_generate.c:439；src/dialog_system.c 以 strdup(response) 返回，
 *  response==NULL 时退化为哨兵 "(null)"）。任一不成立都是用户侧可见缺陷，不得放宽。 */
static void check_response(const char* input, const char* response) {
    if (!response) {
        printf("  x FAIL: [%s] dialog_process 返回 NULL\n", input);
        g_failures++;
        return;
    }
    if (strcmp(response, "(null)") == 0) {
        printf("  x FAIL: [%s] 内部回复为 NULL（哨兵 \"(null)\" 泄露到用户侧）\n", input);
        g_failures++;
        return;
    }
    if (strlen(response) == 0) {
        printf("  x FAIL: [%s] 回复为空串\n", input);
        g_failures++;
        return;
    }
    printf("  + [%s] 真实回复 %zu 字节\n", input, strlen(response));
}

/* 状态文件就绪判定：存在且尺寸 > 0。
 * 依据：master_load_state 对 file_size <= 0 直接返回 -1
 * （src/multi_topology.c:4826 起 `if (file_size <= 0) return -1`），
 * 故尺寸为 0 时加载必失败，等同“无可用训练态”。 */
static int state_file_available(void) {
    struct stat st;
    if (stat(STATE_FILE, &st) != 0) return 0;
    if (st.st_size <= 0) return 0;
    return 1;
}

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

    // 不变量 A：加载前，种子子拓扑数固定为 9（9 次 master_add_sub_topology）
    CHECK_EQ_INT(topology->sub_topo_count, 9,
                 "[不变量A] 种子子拓扑数 (9 次 master_add_sub_topology)");

    int loaded = master_load_state(topology, STATE_FILE);
    if (loaded <= 0) {
        printf("  × 状态加载失败 (%d)\n", loaded);
        master_topology_destroy(topology);
        memory_system_destroy(memory);
        return NULL;
    }
    printf("  ✓ 已加载 %d 个节点\n", loaded);

    // 不变量 A（续）：master_load_state 从不新增子拓扑，加载后仍应为 9
    // 依据：src/multi_topology.c 中 master_load_state 全程只读 sub_topo_count
    CHECK_EQ_INT(topology->sub_topo_count, 9,
                 "[不变量A] 加载后子拓扑数不变 (load 不新增子拓扑)");

    // 不变量 B + C：逐拓扑容量不变量 + 状态已物化
    int total_nodes = 0, total_edges = 0;
    for (int t = 0; t < topology->sub_topo_count; t++) {
        SubTopology* sub = topology->sub_topologies[t];
        CHECK_TRUE(sub != NULL, "[不变量B] 子拓扑指针非空");
        if (!sub) continue;
        CHECK_TRUE(sub->net != NULL, "[不变量B] 子拓扑底层网络非空");
        if (!sub->net) continue;

        // node_count 恒 <= max_nodes（nodes[] 数组容量上界，结构性恒真）
        if (sub->net->node_count < 0 ||
            (size_t)sub->net->node_count > sub->net->max_nodes) {
            printf("  x FAIL: [不变量B] %s node_count=%d 越界 (max_nodes=%zu)\n",
                   sub->name, sub->net->node_count, sub->net->max_nodes);
            g_failures++;
        } else {
            printf("  + %s: node_count=%d <= max_nodes=%zu\n",
                   sub->name, sub->net->node_count, sub->net->max_nodes);
        }

        total_nodes += sub->net->node_count;
        int e = 0;
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (node) e += node->edge_count;   // 原 connection_count → 正确字段 edge_count
        }
        total_edges += e / 2;
        printf("    %s: %d 节点, ~%d 边\n",
               sub->name, sub->net->node_count, e / 2);
    }
    printf("  ─── 总计: %d 节点, ~%d 边\n", total_nodes, total_edges);

    // 不变量 C：加载成功（loaded>0）必须真的把节点物化进子拓扑，不得是空操作
    CHECK_TRUE(total_nodes > 0, "[不变量C] 加载后子拓扑内存在节点 (loaded>0 非空操作)");

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

    // ==================== 环境前置：状态文件就绪检查 ====================
    // 无 pivotmind_state.dat（不存在或尺寸 0）→ 显式 SKIP，退出码 0。
    // 不静默通过：打印明确一行说明，绝不伪造“通过”。
    if (!state_file_available()) {
        printf("SKIP: pivotmind_state.dat not found (run test_cognitive_controller first or point cwd at a real state dir)\n");
        fflush(stdout);
        return 0;
    }

    DialogSystem* dialog = create_loaded_system();
    if (!dialog) {
        printf("FATAL: 创建系统失败\n");
        return 1;
    }

    // 不变量 D：认知调度器存在（dialog_system_create 无条件创建）
    // 依据：src/dialog_system.c:1201 sys->controller = cognitive_controller_create(...)
    CHECK_TRUE(dialog->controller != NULL,
               "[不变量D] 认知调度器已激活 (cognitive_controller_create)");
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
        check_response("人工智能是什么", response);   // 不变量 E
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
        check_response(topics[i], response);          // 不变量 E
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
        // 优雅降级不等于允许泄露 "(null)"：仍须给出真实非空回复
        check_response(obscure[i], response);         // 不变量 E
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
    check_response("人工智能是什么", response);        // 不变量 E
    if (response) {
        printf("<<< %s\n", response);
        free(response);
    }
    if (reasoning) dialog_reasoning_destroy(reasoning);
    fflush(stdout);

    dialog->controller->satisfaction_threshold = orig_threshold;

    // ==================== 总结 ====================
    printf("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    if (g_failures == 0) {
        printf("测试完成 — 全部不变量成立（无崩溃、11 轮回复均为真实非空串）\n");
    } else {
        printf("测试结束 — 断言失败 %d 条（见上方 x FAIL）\n", g_failures);
    }
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
    return g_failures == 0 ? 0 : 1;
}
