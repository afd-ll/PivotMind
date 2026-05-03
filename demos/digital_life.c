/**
 * @file digital_life.c
 * @brief PivotMind 溯智系统 - 认知框架运行时
 * 
 * 集成:
 * 1. 对话服务 (Dialog)
 * 2. 主动学习 (Active Learning)
 * 3. 记忆系统 (Memory)
 * 4. 置信度演化 (Confidence Evolution)
 * 5. 遗忘机制 (Forgetting)
 * 
 * 特点: 持续学习，可长期运行
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include "dialog_system.h"
#include "active_learner.h"
#include "multi_topology.h"
#include "memory_system.h"

#ifdef _WIN32
#ifdef _WIN32
#include <windows.h>
#endif
#endif

// ==================== 溯智系统核心 ====================

typedef struct {
    // 核心组件
    MasterTopology* topology;      // 拓扑认知网络
    MemorySystem* memory;          // 三级记忆系统
    CausalGraph* causal_graph;     // 因果图
    DialogSystem* dialog;          // 对话服务
    ActiveLearner* learner;        // 主动学习器
    
    // 运行控制
    int is_running;
    pthread_t main_thread;
    
    // 配置
    int dialog_port;              // 对话服务端口
    int learning_interval;        // 学习间隔
    int auto_save_interval;       // 自动保存间隔
    
    // 统计
    time_t start_time;
    long total_dialogs;
    long total_learning_cycles;
    
    // 运行模式: 0=对话模式, 1=学习模式
    int mode;
    
    // 信号处理
    volatile int shutdown_requested;
} DigitalLifeSystem;

// 全局系统指针（用于信号处理）
static DigitalLifeSystem* g_system = NULL;

// 统一的学习函数 - 消除重复代码
static void trigger_learning_cycle(ActiveLearner* learner) {
    if (!learner) return;
    learn_from_memory(learner);
    discover_new_relations(learner);
    cleanup_forgotten_knowledge(learner);
}

// 信号处理
void signal_handler(int signum) {
    (void)signum;  // 未使用的信号码
    if (g_system) {
        printf("\n[系统] 收到退出信号，正在关闭...\n");
        g_system->shutdown_requested = 1;
    }
}

// 创建数字生命系统
DigitalLifeSystem* digital_life_create() {
    DigitalLifeSystem* sys = (DigitalLifeSystem*)calloc(1, sizeof(DigitalLifeSystem));
    if (!sys) return NULL;
    
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
    
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║            PivotMind 溯智系统 - 初始化                   ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");
    
    // 1. 创建记忆系统
    printf("[1/4] 创建记忆系统...\n");
    sys->memory = memory_system_create(500, 2000, 5000);
    if (!sys->memory) {
        printf("错误: 无法创建记忆系统\n");
        free(sys);
        return NULL;
    }
    printf("     ✓ 记忆系统就绪 (容量: 上下文100 + 短期500 + 永久2000)\n");
    
    // 2. 创建多拓扑网络
    printf("[2/4] 创建多拓扑认知网络...\n");
    sys->topology = master_topology_create(9);
    if (!sys->topology) {
        printf("错误: 无法创建拓扑网络\n");
        memory_system_destroy(sys->memory);
        free(sys);
        return NULL;
    }
    
    // 添加子拓扑（扩容以支持书籍数量）
    master_add_sub_topology(sys->topology, TOPO_VOCABULARY, "词汇拓扑", 6000, 10);
    master_add_sub_topology(sys->topology, TOPO_SEMANTIC, "语义拓扑", 2000, 9);
    master_add_sub_topology(sys->topology, TOPO_EMOTION, "情绪拓扑", 500, 8);
    master_add_sub_topology(sys->topology, TOPO_SYNTAX, "语法拓扑", 500, 7);
    master_add_sub_topology(sys->topology, TOPO_CONTEXT, "上下文拓扑", 500, 6);
    master_add_sub_topology(sys->topology, TOPO_DOMAIN, "领域拓扑", 500, 5);
    master_add_sub_topology(sys->topology, TOPO_PRAGMA, "语用拓扑", 500, 4);
    master_add_sub_topology(sys->topology, TOPO_CULTURE, "文化拓扑", 500, 3);
    master_add_sub_topology(sys->topology, TOPO_CONCEPT, "概念拓扑", 6000, 9);
    
    // 添加初始知识（可以通过训练扩展）
    printf("     添加初始知识...\n");
    SubTopology* vocab = master_get_sub_topology_by_type(sys->topology, TOPO_VOCABULARY);
    SubTopology* semantic = master_get_sub_topology_by_type(sys->topology, TOPO_SEMANTIC);
    SubTopology* emotion = master_get_sub_topology_by_type(sys->topology, TOPO_EMOTION);
    SubTopology* culture = master_get_sub_topology_by_type(sys->topology, TOPO_CULTURE);
    (void)emotion; (void)culture;  // 暂未使用
    
    if (vocab && semantic) {
        // 基础词汇
        huarong_net_add_node(vocab->net, "我", NULL, 0);
        huarong_net_add_node(vocab->net, "你", NULL, 0);
        huarong_net_add_node(vocab->net, "是", NULL, 0);
        huarong_net_add_node(vocab->net, "什么", NULL, 0);
        huarong_net_add_node(vocab->net, "学习", NULL, 0);
        huarong_net_add_node(vocab->net, "知道", NULL, 0);
        huarong_net_add_node(vocab->net, "帮助", NULL, 0);
        
        // 语义
        huarong_net_add_node(semantic->net, "自我", NULL, 0);
        huarong_net_add_node(semantic->net, "他人", NULL, 0);
        huarong_net_add_node(semantic->net, "存在", NULL, 0);
        huarong_net_add_node(semantic->net, "知识", NULL, 0);
        huarong_net_add_node(semantic->net, "理解", NULL, 0);
        huarong_net_add_node(semantic->net, "协助", NULL, 0);
        
        // 连接
        huarong_net_add_connection(vocab->net, 0, 0, 0.9f);  // 我->自我
        huarong_net_add_connection(vocab->net, 1, 1, 0.9f);  // 你->他人
        huarong_net_add_connection(vocab->net, 4, 3, 0.8f);  // 学习->知识
        huarong_net_add_connection(vocab->net, 5, 4, 0.8f);  // 知道->理解
        huarong_net_add_connection(vocab->net, 6, 5, 0.8f);  // 帮助->协助
    }
    
    if (vocab && emotion) {
        huarong_net_add_node(emotion->net, "开心", NULL, 0);
        huarong_net_add_node(emotion->net, "好奇", NULL, 0);
        huarong_net_add_node(emotion->net, "满足", NULL, 0);
    }
    
    printf("     ✓ 认知网络就绪 (%d 个拓扑)\n", sys->topology->sub_topo_count);

    // 3. 创建因果图
    printf("[3/5] 创建因果推理系统...\n");
    sys->causal_graph = causal_graph_create(1000, 5000);
    if (!sys->causal_graph) {
        printf("错误: 无法创建因果图\n");
        master_topology_destroy(sys->topology);
        memory_system_destroy(sys->memory);
        free(sys);
        return NULL;
    }
    printf("     ✓ 因果图就绪\n");

    // 4. 创建主动学习器（需要先创建，以便传递给对话系统）
    printf("[4/5] 创建主动学习器...\n");
    sys->learner = active_learner_create(sys->topology, sys->memory);
    if (!sys->learner) {
        printf("错误: 无法创建学习器\n");
        causal_graph_destroy(sys->causal_graph);
        master_topology_destroy(sys->topology);
        memory_system_destroy(sys->memory);
        free(sys);
        return NULL;
    }
    active_learner_set_interval(sys->learner, 300);  // 5分钟
    printf("     ✓ 学习器就绪 (间隔: 300秒)\n");

    // 5. 创建对话系统
    printf("[5/5] 创建对话系统...\n");
    sys->dialog = dialog_system_create(sys->topology, sys->memory, sys->causal_graph, sys->learner);
    if (!sys->dialog) {
        printf("错误: 无法创建对话系统\n");
        active_learner_destroy(sys->learner);
        causal_graph_destroy(sys->causal_graph);
        master_topology_destroy(sys->topology);
        memory_system_destroy(sys->memory);
        free(sys);
        return NULL;
    }
    printf("     ✓ 对话系统就绪\n");
    
    // 尝试加载之前保存的拓扑状态
    const char* state_file = "pivotmind_state.dat";
    if (access(state_file, F_OK) == 0) {
        int loaded = master_load_state(sys->topology, state_file);
        if (loaded >= 0) {
            printf("     ✓ 已加载拓扑状态 (%d 节点)\n", loaded);
        }
    }
    
    // 加载记忆种子
    const char* mem_file = "memory_seed.dat";
    memory_load_seed(sys->memory, mem_file);
    
    // 初始化配置
    sys->is_running = 0;
    sys->shutdown_requested = 0;
    sys->start_time = time(NULL);
    sys->total_dialogs = 0;
    sys->total_learning_cycles = 0;
    sys->mode = 0;  // 默认对话模式
    sys->learning_interval = 300;
    sys->auto_save_interval = 3600;  // 1小时
    
    g_system = sys;
    
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║                 溯智系统初始化完成!                       ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");
    
    return sys;
}

// 销毁溯智系统
void digital_life_destroy(DigitalLifeSystem* sys) {
    if (!sys) return;
    
    printf("\n[系统] 正在关闭溯智系统...\n");
    
    // 保存拓扑状态
    if (sys->topology) {
        const char* state_file = "pivotmind_state.dat";
        int saved = master_save_state(sys->topology, state_file);
        if (saved >= 0) {
            printf("  ✓ 已保存拓扑状态 (%d 节点)\n", saved);
        }
    }
    
    // 停止所有进程
    if (sys->learner) {
        active_learner_stop(sys->learner);
        active_learner_destroy(sys->learner);
    }
    
    if (sys->dialog) {
        dialog_system_destroy(sys->dialog);
    }
    
    if (sys->topology) {
        master_topology_destroy(sys->topology);
    }
    
    if (sys->memory) {
        memory_system_destroy(sys->memory);
    }

    if (sys->causal_graph) {
        causal_graph_destroy(sys->causal_graph);
    }

    printf("[系统] 溯智系统已关闭\n");
    printf("  总运行时间: %lld 秒\n", (long long)(time(NULL) - sys->start_time));
    printf("  总对话轮数: %lld\n", (long long)sys->total_dialogs);
    printf("  学习周期数: %lld\n", (long long)sys->total_learning_cycles);
    
    free(sys);
    g_system = NULL;
}

// 启动溯智系统
void digital_life_start(DigitalLifeSystem* sys) {
    if (!sys || sys->is_running) return;
    
    printf("\n[系统] 启动溯智系统...\n");
    
    // 启动主动学习器（暂关，排查 heap corruption）
    // active_learner_start(sys->learner);
    (void)sys;
    
    sys->is_running = 1;
    
    printf("[系统] 溯智系统运行中\n");
    printf("  输入 'help' 查看命令\n");
    printf("  输入 'quit' 退出\n\n");
}

// 停止数字生命
void digital_life_stop(DigitalLifeSystem* sys) {
    if (!sys->is_running) return;
    
    sys->is_running = 0;
    sys->shutdown_requested = 1;
    
    active_learner_stop(sys->learner);
    
    printf("[系统] 溯智系统已停止\n");
}

// 打印帮助
void print_help() {
    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("                    可用命令                                   \n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  help          - 显示帮助\n");
    printf("  quit/exit     - 退出系统\n");
    printf("  mode          - 切换对话/学习模式\n");
    printf("  learn on      - 开启学习模式\n");
    printf("  learn off     - 关闭学习模式\n");
    printf("  learn         - 手动触发一次学习\n");
    printf("  stats         - 显示统计信息\n");
    printf("  memory        - 查看记忆状态\n");
    printf("  network       - 查看认知网络状态\n");
    printf("  clear         - 清除屏幕\n");
    printf("═══════════════════════════════════════════════════════════════\n");
}

// 打印统计信息
void print_stats(DigitalLifeSystem* sys) {
    printf("\n=== 溯智系统统计 ===\n");
    printf("运行时间: %lld 秒 (%lld 分钟)\n", 
           (long long)(time(NULL) - sys->start_time),
           (long long)((time(NULL) - sys->start_time) / 60));
    printf("对话轮数: %lld\n", (long long)sys->total_dialogs);
    printf("学习周期: %lld\n", (long long)sys->total_learning_cycles);
    
    if (sys->learner) {
        printf("累计学习概念: %d\n", sys->learner->total_concepts_learned);
        printf("累计建立关系: %d\n", sys->learner->total_relations_learned);
        printf("累计遗忘: %d\n", sys->learner->total_forgotten);
    }
    
    if (sys->topology) {
        int total_nodes = 0;
        for (int t = 0; t < sys->topology->sub_topo_count; t++) {
            if (sys->topology->sub_topologies[t] && sys->topology->sub_topologies[t]->net) {
                total_nodes += sys->topology->sub_topologies[t]->net->node_count;
            }
        }
        printf("拓扑节点: %d\n", total_nodes);
    }
}

// 对话处理
void handle_dialog(DigitalLifeSystem* sys, char* input) {
    // 处理特殊命令
    if (strcmp(input, "help") == 0) {
        print_help();
        return;
    }
    if (strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0) {
        sys->shutdown_requested = 1;
        return;
    }
    if (strcmp(input, "stats") == 0) {
        print_stats(sys);
        return;
    }
    if (strcmp(input, "learn") == 0) {
        printf("\n[手动学习] 触发学习...\n");
        trigger_learning_cycle(sys->learner);
        sys->total_learning_cycles++;
        return;
    }
    if (strcmp(input, "clear") == 0) {
#ifdef _WIN32
        system("cls");
#else
        system("clear");
#endif
        return;
    }
    
    // 正常对话
    DialogReasoning* reasoning = NULL;
    char* response = dialog_process(sys->dialog, input, &reasoning);
    if (response) {
        printf("\n>>> AI: %s\n", response);
        
        // 提示用户反馈
        printf("\n  [可选] 评价这个回答:\n");
        printf("    - 输入 'correct' 或 '对' 标记正确\n");
        printf("    - 输入 'wrong' 或 '错' 标记错误\n");
        printf("    - 输入 '更好的回答:xxx' 提供更好答案\n");
        
        // 获取用户反馈
        char feedback[256] = {0};
        printf("\n> 你的评价 (直接回车跳过): ");
        if (fgets(feedback, sizeof(feedback), stdin)) {
            feedback[strcspn(feedback, "\n")] = 0;
            
            if (strcmp(feedback, "correct") == 0 || strcmp(feedback, "对") == 0 || 
                strcmp(feedback, "对 的") == 0 || strcmp(feedback, "对的") == 0) {
                // 用户确认正确 - 增加置信度
                if (reasoning && sys->topology) {
                    for (int i = 0; i < reasoning->assoc_count; i++) {
                        DialogAssociation* assoc = &reasoning->associations[i];
                        if (assoc->node_id >= 0 && assoc->topo_type >= 0) {
                            master_set_node_confidence(sys->topology, assoc->topo_type, 
                                                     assoc->node_id, 
                                                     0.95f);
                            if (assoc->from_node_id >= 0) {
                                master_set_edge_confidence(sys->topology, assoc->topo_type,
                                                          assoc->from_node_id, assoc->node_id,
                                                          0.95f);
                            }
                        }
                    }
                    printf("\n✓ 已增强相关知识的置信度\n");
                }
            } else if (strcmp(feedback, "wrong") == 0 || strcmp(feedback, "错") == 0 ||
                       strcmp(feedback, "不对") == 0 || strcmp(feedback, "不是") == 0) {
                // 用户纠正 - 降低置信度
                if (reasoning && sys->topology) {
                    for (int i = 0; i < reasoning->assoc_count; i++) {
                        DialogAssociation* assoc = &reasoning->associations[i];
                        if (assoc->node_id >= 0 && assoc->topo_type >= 0) {
                            float new_conf = 0.2f;
                            master_set_node_confidence(sys->topology, assoc->topo_type, 
                                                     assoc->node_id, new_conf);
                            if (assoc->from_node_id >= 0) {
                                master_set_edge_confidence(sys->topology, assoc->topo_type,
                                                          assoc->from_node_id, assoc->node_id,
                                                          new_conf);
                            }
                        }
                    }
                    printf("\n✗ 已降低相关知识的置信度\n");
                }
            } else if (strncmp(feedback, "更好的回答:", 11) == 0 || 
                       strncmp(feedback, "更好的:", 8) == 0) {
                // 用户提供更好的答案 - 学习新知识
                const char* better_answer = feedback + (strncmp(feedback, "更好的回答:", 11) == 0 ? 11 : 8);
                while (*better_answer == ' ') better_answer++;
                printf("\n★ 已记录更好的回答: %s\n", better_answer);
                // TODO: 将更好的回答存入记忆
            }
        }
        
        if (reasoning) {
            dialog_reasoning_destroy(reasoning);
        }
        
        sys->total_dialogs++;
        
        free(response);
    }
}

// 主函数
int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
    
    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 创建系统
    DigitalLifeSystem* sys = digital_life_create();
    if (!sys) {
        printf("错误: 无法创建溯智系统\n");
        return 1;
    }
    
    // 启动
    digital_life_start(sys);
    
    // 主循环
    char input[2048];
    
    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("           欢迎使用 PivotMind 溯智系统                      \n");
    printf("     持续运行，自主学习和成长                          \n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("  当前模式: 对话模式 (输入 'help' 查看命令)\n\n");
    
    while (!sys->shutdown_requested) {
        // 显示模式提示
        if (sys->mode == 0) {
            printf("\n[对话] 你: ");
        } else {
            printf("\n[学习] 你: ");
        }
        
        if (fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }
        
        // 去掉换行
        input[strcspn(input, "\n")] = 0;
        
        // 跳过空输入
        if (strlen(input) == 0) continue;
        
        // 处理命令
        if (strcmp(input, "help") == 0) {
            print_help();
            continue;
        } else if (strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0) {
            break;
        } else if (strcmp(input, "stats") == 0) {
            print_stats(sys);
            continue;
        } else if (strcmp(input, "mode") == 0 || strcmp(input, "模式") == 0) {
            // 切换模式
            if (sys->mode == 0) {
                sys->mode = 1;
                printf("  [系统] 切换到学习模式\n");
            } else {
                sys->mode = 0;
                printf("  [系统] 切换到对话模式\n");
            }
            continue;
        } else if (strncmp(input, "learn", 5) == 0) {
            // learn on / learn off
            if (strcmp(input, "learn on") == 0 || strcmp(input, "learn on") == 0) {
                sys->mode = 1;
                printf("  [系统] 已开启学习模式 (后台并行运行)\n");
            } else if (strcmp(input, "learn off") == 0) {
                sys->mode = 0;
                printf("  [系统] 已关闭学习模式\n");
            } else {
                printf("  用法: learn on / learn off\n");
            }
            continue;
        } else if (sys->mode == 1) {
            // 学习模式下，尝试学习输入的内容
            printf("  [学习] 正在学习: %s\n", input);
            learn_from_dialog(sys->learner, input, "", "");
            continue;
        }
        
        // 对话模式下的反馈处理
        if (strncmp(input, "correct", 7) == 0 || strncmp(input, "对", 2) == 0 ||
            strncmp(input, "wrong", 5) == 0 || strncmp(input, "错", 2) == 0 ||
            strncmp(input, "更好的回答:", 12) == 0) {
            printf("  [系统] 收到反馈，正在学习...\n");
            continue;
        }
        
        // 处理对话
        handle_dialog(sys, input);
    }
    
    // 清理
    digital_life_destroy(sys);
    
    printf("\n[系统] 再见！期待下次相遇。\n");
    
    return 0;
}