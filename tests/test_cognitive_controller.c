/**
 * @file test_cognitive_controller.c
 * @brief 认知调度中心测试（含确定性回归断言）
 *
 * 测试内容：
 * 1. 反复问"人工智能是什么"——看 retry 三级降级 + 满意度评分
 * 2. 多轮对话 intent_weights 快照——看调度是否生效
 * 3. 模糊输入压力测试——看优雅降级
 *
 * ⚠️ 本文件历史上 assert 数 = 0，`make test` 的 PASS 只等价于"进程没崩"。
 * 现补入下列**确定性**不变量断言（每条数值均由源码逐行推导，无时序/随机依赖）。
 * 依据（HEAD=8194d31）：
 *   - create_test_system() 固定 9 次 master_add_sub_topology      → sub_topo_count == 9
 *   - 种子节点固定 20/11/4/3/5 次 huarong_net_add_node             → 各拓扑 node_count
 *   - 词汇拓扑固定 20 次 huarong_net_add_connection，(from,to) 互异 → 边总数 20
 *   - dialog_process 顶部 sys->turn_count++（src/dialog_system.c:1507）→ 轮数精确可数
 *   - dialog_generate 所有出口均 strdup(...)（src/dialog_generate.c:439 兜底）
 *     故 dialog_process 返回者必为真实非空串，"(null)" 哨兵不可泄露到用户侧
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

/* ================================================================
 *  断言框架 — 失败计数 + main 末尾据此返回非 0（Makefile test: 以退出码判成败）
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
 * 依据 dialog_process 契约（src/dialog_system.c:2046 用 strdup(response)，
 * response 为 NULL 时退化为哨兵 "(null)"）与 dialog_generate 的全出口兜底
 * （src/dialog_generate.c:439）。任一不成立都是用户侧可见缺陷，不得放宽。 */
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

// 简化版的 digital_life_create，只创建核心组件
static DialogSystem* create_test_system(void) {
    // 1. 创建记忆系统
    MemorySystem* memory = memory_system_create(500, 2000, 5000);
    if (!memory) { printf("FAIL: memory\n"); return NULL; }

    // 2. 创建多拓扑网络
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

    // 3. 添加种子知识
    SubTopology* vocab = master_get_sub_topology_by_type(topology, TOPO_VOCABULARY);
    SubTopology* semantic = master_get_sub_topology_by_type(topology, TOPO_SEMANTIC);
    SubTopology* emotion = master_get_sub_topology_by_type(topology, TOPO_EMOTION);
    SubTopology* culture = master_get_sub_topology_by_type(topology, TOPO_CULTURE);
    SubTopology* concept_t = master_get_sub_topology_by_type(topology, TOPO_CONCEPT);

    if (vocab && semantic) {
        huarong_net_add_node(vocab->net, "我", NULL, 0);
        huarong_net_add_node(vocab->net, "你", NULL, 0);
        huarong_net_add_node(vocab->net, "是", NULL, 0);
        huarong_net_add_node(vocab->net, "什么", NULL, 0);
        huarong_net_add_node(vocab->net, "学习", NULL, 0);
        huarong_net_add_node(vocab->net, "知道", NULL, 0);
        huarong_net_add_node(vocab->net, "帮助", NULL, 0);
        huarong_net_add_node(vocab->net, "人工智能", NULL, 0);
        huarong_net_add_node(vocab->net, "AI", NULL, 0);
        huarong_net_add_node(vocab->net, "机器", NULL, 0);
        huarong_net_add_node(vocab->net, "智能", NULL, 0);
        huarong_net_add_node(vocab->net, "算法", NULL, 0);
        huarong_net_add_node(vocab->net, "数据", NULL, 0);
        huarong_net_add_node(vocab->net, "计算机", NULL, 0);
        huarong_net_add_node(vocab->net, "代码", NULL, 0);
        huarong_net_add_node(vocab->net, "软件", NULL, 0);
        huarong_net_add_node(vocab->net, "硬件", NULL, 0);
        huarong_net_add_node(vocab->net, "电路", NULL, 0);
        huarong_net_add_node(vocab->net, "芯片", NULL, 0);
        huarong_net_add_node(vocab->net, "STM32", NULL, 0);

        huarong_net_add_node(semantic->net, "自我", NULL, 0);
        huarong_net_add_node(semantic->net, "他人", NULL, 0);
        huarong_net_add_node(semantic->net, "存在", NULL, 0);
        huarong_net_add_node(semantic->net, "知识", NULL, 0);
        huarong_net_add_node(semantic->net, "理解", NULL, 0);
        huarong_net_add_node(semantic->net, "协助", NULL, 0);
        huarong_net_add_node(semantic->net, "计算", NULL, 0);
        huarong_net_add_node(semantic->net, "推理", NULL, 0);
        huarong_net_add_node(semantic->net, "自动化", NULL, 0);
        huarong_net_add_node(semantic->net, "感知", NULL, 0);
        huarong_net_add_node(semantic->net, "决策", NULL, 0);

        // 建立连接（模拟训练后的边）
        huarong_net_add_connection(vocab->net, 0, 0, 0.9f);  // 我->自我
        huarong_net_add_connection(vocab->net, 1, 1, 0.9f);  // 你->他人
        huarong_net_add_connection(vocab->net, 7, 2, 0.8f);  // 人工智能->知识
        huarong_net_add_connection(vocab->net, 7, 3, 0.7f);  // 人工智能->理解
        huarong_net_add_connection(vocab->net, 7, 6, 0.6f);  // 人工智能->计算
        huarong_net_add_connection(vocab->net, 7, 7, 0.5f);  // 人工智能->推理
        huarong_net_add_connection(vocab->net, 7, 8, 0.6f);  // 人工智能->自动化
        huarong_net_add_connection(vocab->net, 7, 9, 0.4f);  // 人工智能->感知
        huarong_net_add_connection(vocab->net, 8, 6, 0.7f);  // AI->计算
        huarong_net_add_connection(vocab->net, 9, 6, 0.8f);  // 机器->计算
        huarong_net_add_connection(vocab->net, 10, 3, 0.6f); // 智能->理解
        huarong_net_add_connection(vocab->net, 11, 6, 0.7f); // 算法->计算
        huarong_net_add_connection(vocab->net, 12, 6, 0.7f); // 数据->计算
        huarong_net_add_connection(vocab->net, 13, 6, 0.6f); // 计算机->计算
        huarong_net_add_connection(vocab->net, 14, 7, 0.5f); // 代码->推理
        huarong_net_add_connection(vocab->net, 15, 8, 0.5f); // 软件->自动化
        huarong_net_add_connection(vocab->net, 16, 8, 0.5f); // 硬件->自动化
        huarong_net_add_connection(vocab->net, 17, 9, 0.4f); // 电路->感知
        huarong_net_add_connection(vocab->net, 18, 9, 0.4f); // 芯片->感知
        huarong_net_add_connection(vocab->net, 19, 16, 0.5f); // STM32->硬件

        // 跨拓扑连接
        master_add_cross_link(topology, 0, 7, 1, 0, 0.8f, "属于");   // 人工智能->自我
        master_add_cross_link(topology, 0, 7, 1, 2, 0.8f, "属于");   // 人工智能->知识
        master_add_cross_link(topology, 0, 7, 1, 3, 0.7f, "属于");   // 人工智能->理解
        master_add_cross_link(topology, 0, 7, 1, 4, 0.6f, "属于");   // 人工智能->协助
        master_add_cross_link(topology, 0, 7, 1, 6, 0.7f, "属于");   // 人工智能->计算
        master_add_cross_link(topology, 0, 7, 1, 7, 0.8f, "属于");   // 人工智能->推理
        master_add_cross_link(topology, 0, 10, 1, 3, 0.5f, "相关");  // 智能->理解
    }

    if (vocab && emotion) {
        huarong_net_add_node(emotion->net, "开心", NULL, 0);
        huarong_net_add_node(emotion->net, "好奇", NULL, 0);
        huarong_net_add_node(emotion->net, "满足", NULL, 0);
        huarong_net_add_node(emotion->net, "困惑", NULL, 0);
    }

    if (vocab && culture) {
        huarong_net_add_node(culture->net, "科技", NULL, 0);
        huarong_net_add_node(culture->net, "创新", NULL, 0);
        huarong_net_add_node(culture->net, "未来", NULL, 0);
        master_add_cross_link(topology, 0, 7, 7, 0, 0.6f, "关联"); // 人工智能->科技
        master_add_cross_link(topology, 0, 7, 7, 1, 0.5f, "关联"); // 人工智能->创新
    }

    if (vocab && concept_t) {
        huarong_net_add_node(concept_t->net, "AI系统", NULL, 0);
        huarong_net_add_node(concept_t->net, "机器学习", NULL, 0);
        huarong_net_add_node(concept_t->net, "神经网络", NULL, 0);
        huarong_net_add_node(concept_t->net, "深度学习", NULL, 0);
        huarong_net_add_node(concept_t->net, "嵌入式", NULL, 0);
        master_add_cross_link(topology, 0, 7, 8, 0, 0.8f, "抽象"); // 人工智能->AI系统
        master_add_cross_link(topology, 0, 7, 8, 1, 0.7f, "抽象"); // 人工智能->机器学习
        master_add_cross_link(topology, 0, 7, 8, 2, 0.6f, "抽象"); // 人工智能->神经网络
        master_add_cross_link(topology, 0, 7, 8, 4, 0.3f, "弱关联"); // 人工智能->嵌入式
    }

    // 4. 创建因果图
    CausalGraph* causal_graph = causal_graph_create(1000, 5000);
    if (!causal_graph) { master_topology_destroy(topology); memory_system_destroy(memory); return NULL; }

    // 5. 创建主动学习器
    ActiveLearner* learner = active_learner_create(topology, memory);
    if (!learner) { causal_graph_destroy(causal_graph); master_topology_destroy(topology); memory_system_destroy(memory); return NULL; }

    // 6. 创建对话系统
    DialogSystem* dialog = dialog_system_create(topology, memory, causal_graph, learner);
    if (!dialog) { active_learner_destroy(learner); causal_graph_destroy(causal_graph); master_topology_destroy(topology); memory_system_destroy(memory); return NULL; }

    return dialog;
}

static void destroy_test_system(DialogSystem* dialog) {
    if (!dialog) return;
    ActiveLearner* learner = dialog->learner;
    CausalGraph* graph = dialog->causal_graph;
    MasterTopology* topology = dialog->master;
    MemorySystem* memory = dialog->memory;
    dialog_system_destroy(dialog);
    if (learner) active_learner_destroy(learner);
    if (graph) causal_graph_destroy(graph);
    if (topology) master_topology_destroy(topology);
    if (memory) memory_system_destroy(memory);
}

static void print_separator(void) {
    printf("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
}

int main(void) {
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║       CognitiveController 测试套件(含断言)           ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");
    fflush(stdout);

    DialogSystem* dialog = create_test_system();
    if (!dialog) {
        printf("FATAL: 创建系统失败\n");
        return 1;
    }
    printf("\n系统就绪! 子拓扑数: %d\n", dialog->master->sub_topo_count);
    printf("认知调度器: %s\n", dialog->controller ? "已激活" : "未激活");
    fflush(stdout);

    // ==================== 构建不变量断言 ====================
    // 依据：create_test_system() 中硬编码的种子注入次数（无随机/时序依赖）
    print_separator();
    printf("[不变量] 系统构建断言\n");
    CHECK_EQ_INT(dialog->master->sub_topo_count, 9,
                 "子拓扑数 (9 次 master_add_sub_topology)");
    CHECK_TRUE(dialog->controller != NULL, "认知调度器已激活 (被测主体存在)");
    {
        SubTopology* v  = master_get_sub_topology_by_type(dialog->master, TOPO_VOCABULARY);
        SubTopology* s  = master_get_sub_topology_by_type(dialog->master, TOPO_SEMANTIC);
        SubTopology* em = master_get_sub_topology_by_type(dialog->master, TOPO_EMOTION);
        SubTopology* cu = master_get_sub_topology_by_type(dialog->master, TOPO_CULTURE);
        SubTopology* ct = master_get_sub_topology_by_type(dialog->master, TOPO_CONCEPT);
        CHECK_TRUE(v && s && em && cu && ct, "5 个受种子子拓扑均已按类型取回");
        if (v && s && em && cu && ct) {
            CHECK_EQ_INT(v->net->node_count,  20, "词汇拓扑种子节点数");
            CHECK_EQ_INT(s->net->node_count,  11, "语义拓扑种子节点数");
            CHECK_EQ_INT(em->net->node_count,  4, "情绪拓扑种子节点数");
            CHECK_EQ_INT(cu->net->node_count,  3, "文化拓扑种子节点数");
            CHECK_EQ_INT(ct->net->node_count,  5, "概念拓扑种子节点数");
            int v_edges = 0;
            for (int n = 0; n < v->net->node_count; n++)
                if (v->net->nodes[n]) v_edges += v->net->nodes[n]->edge_count;
            CHECK_EQ_INT(v_edges, 20,
                         "词汇拓扑种子边总数 (20 次 add_connection, (from,to) 互异)");
        }
    }
    print_separator();

    // ==================== 测试1: 反复问同一问题 ====================
    print_separator();
    printf("测试1: 反复问「人工智能是什么」— 看 retry 日志和满意度\n\n");

    const char* questions[] = {
        "人工智能是什么",
        "人工智能是什么",
        "人工智能是什么",
    };
    int q_count = sizeof(questions) / sizeof(questions[0]);

    for (int i = 0; i < q_count; i++) {
        printf("\n>>> [第%d轮] 用户: %s\n", i + 1, questions[i]);
        fflush(stdout);

        // 在问之前先看看意图权重
        if (dialog->controller) {
            printf("[快照] prev_intent_weights: ");
            for (int t = 0; t < MAX_SUBTOPOS; t++) {
                if (dialog->controller->prev_intent_weights[t] > 0.01f)
                    printf("topo%d=%.4f ", t, dialog->controller->prev_intent_weights[t]);
            }
            printf("\n");
            fflush(stdout);
        }

        DialogReasoning* reasoning = NULL;
        char* response = dialog_process(dialog, questions[i], &reasoning);
        check_response(questions[i], response);
        if (response) {
            printf("\n<<< AI: %s\n", response);
            free(response);
        }
        if (reasoning) {
            dialog_reasoning_destroy(reasoning);
        }
        fflush(stdout);
    }

    // ==================== 测试2: 多轮对话看调度演化 ====================
    print_separator();
    printf("测试2: 多轮不同话题 — 看 intent_weights 调度\n\n");

    const char* topics[] = {
        "学习是什么",
        "STM32是什么",
        "你好",
        "芯片是什么",
        "算法是什么",
    };
    int t_count = sizeof(topics) / sizeof(topics[0]);

    for (int i = 0; i < t_count; i++) {
        printf("\n>>> [第%d轮] 用户: %s\n", i + 1, topics[i]);
        fflush(stdout);

        if (dialog->controller) {
            printf("[快照] prev_intent_weights: ");
            int count = 0;
            for (int t = 0; t < MAX_SUBTOPOS; t++) {
                if (dialog->controller->prev_intent_weights[t] > 0.01f) {
                    if (count++ > 0) printf(", ");
                    printf("topo%d[%s]=%.4f", t,
                           cognitive_controller_topo_name(t),
                           dialog->controller->prev_intent_weights[t]);
                }
            }
            printf("\n");
            fflush(stdout);
        }

        DialogReasoning* reasoning = NULL;
        char* response = dialog_process(dialog, topics[i], &reasoning);
        check_response(topics[i], response);
        if (response) {
            printf("<<< AI: %s\n", response);
            free(response);
        }
        if (reasoning) dialog_reasoning_destroy(reasoning);
        fflush(stdout);
    }

    // ==================== 测试3: 模糊输入/弱覆盖压力测试 ====================
    print_separator();
    printf("测试3: 模糊输入压力测试 — 看优雅降级\n\n");

    const char* obscure_inputs[] = {
        "量子纠缠的超距作用",
        "薛定谔的猫是怎么回事",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    };
    int o_count = sizeof(obscure_inputs) / sizeof(obscure_inputs[0]);

    for (int i = 0; i < o_count; i++) {
        printf("\n>>> [模糊%d] 用户: %s\n", i + 1, obscure_inputs[i]);
        fflush(stdout);

        DialogReasoning* reasoning = NULL;
        char* response = dialog_process(dialog, obscure_inputs[i], &reasoning);
        // 优雅降级不等于允许泄露 "(null)"：仍须给出真实非空回复
        check_response(obscure_inputs[i], response);
        if (response) {
            printf("<<< AI: %s\n", response);
            free(response);
        }
        if (reasoning) dialog_reasoning_destroy(reasoning);
        fflush(stdout);
    }

    // ==================== 测试4: 调低阈值模拟频繁修正 ====================
    print_separator();
    printf("测试4: 低满意度阈值 — 强制触发三级降级\n\n");

    // 保存原始阈值
    float orig_threshold = dialog->controller->satisfaction_threshold;
    dialog->controller->satisfaction_threshold = 0.95f;  // 极高阈值，几乎必触发retry

    printf("(满意度阈值设为 0.95，几乎必然触发 retry)\n");
    fflush(stdout);

    const char* trigger_input = "人工智能是什么";
    printf("\n>>> 用户: %s\n", trigger_input);
    fflush(stdout);

    DialogReasoning* reasoning = NULL;
    char* response = dialog_process(dialog, trigger_input, &reasoning);
    check_response(trigger_input, response);
    if (response) {
        printf("<<< AI: %s\n", response);
        free(response);
    }
    if (reasoning) dialog_reasoning_destroy(reasoning);
    fflush(stdout);

    // 恢复阈值
    dialog->controller->satisfaction_threshold = orig_threshold;

    // ==================== 总结 + 断言汇总 ====================
    print_separator();
    printf("测试完成\n");
    printf("  - 测试1: 反复同一问题 → 看 retry 日志\n");
    printf("  - 测试2: 多轮不同话题 → 看 intent_weights 变化\n");
    printf("  - 测试3: 模糊输入 → 看优雅降级\n");
    printf("  - 测试4: 高阈值强制 retry → 看三级降级链\n\n");

    // 每轮 dialog_process 顶部 turn_count++（src/dialog_system.c:1507），
    // 且 create 时置 0、无其它自增路径 → 3 + 5 + 3 + 1 = 12 精确可断言
    CHECK_EQ_INT(dialog->turn_count, 12,
                 "累计对话轮数 (3+5+3+1 次 dialog_process)");

    printf("\n[断言汇总] 失败项: %d\n", g_failures);

    // 清理
    destroy_test_system(dialog);
    printf("系统已清理.\n");

    if (g_failures > 0) {
        printf("测试失败: %d 项断言未通过\n", g_failures);
        return 1;
    }
    printf("测试通过: 全部断言成立\n");
    return 0;
}
