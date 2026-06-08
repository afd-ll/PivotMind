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
#include "background_clock.h"
#include "multi_topology.h"
#include "memory_system.h"
#include "feature_io.h"
#include "cross_edge_io.h"
#include "feature_pretrain.h"
#include "path_encoding.h"
#include "template_builder.h"

#ifdef _WIN32
#include <windows.h>
#endif

// ==================== 溯智系统核心 ====================

typedef struct {
    // 核心组件
    MasterTopology* topology;      // 拓扑认知网络
    MemorySystem* memory;          // 三级记忆系统
    CausalGraph* causal_graph;     // 因果图
    DialogSystem* dialog;          // 对话服务
    ActiveLearner* learner;        // 主动学习器
    BackgroundClock* bg_clock;     // 后台时钟（持续运转）
    
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

// ==================== 模板系统集成 ====================

/* 使用 template_auto_build() 一站式管线 (定义于 template_builder.c) */

// 统一的学习函数 - 消除重复代码
static void trigger_learning_cycle(ActiveLearner* learner) {
    if (!learner) return;
    learn_from_memory(learner);
    discover_new_relations(learner);
    cleanup_forgotten_knowledge(learner);
}

// 信号处理 (使用 async-signal-safe 函数)
void signal_handler(int signum) {
    (void)signum;
    if (g_system) {
        write(STDOUT_FILENO, "\n[系统] 收到退出信号，正在关闭...\n", 40);
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
    printf("[1/6] 创建记忆系统...\n");
    sys->memory = memory_system_create(500, 2000, 5000);
    if (!sys->memory) {
        printf("错误: 无法创建记忆系统\n");
        free(sys);
        return NULL;
    }
    printf("     ✓ 记忆系统就绪 (容量: 上下文100 + 短期500 + 永久2000)\n");
    
    // 2. 创建多拓扑网络
    printf("[2/6] 创建多拓扑认知网络...\n");
    sys->topology = master_topology_create(16);
    if (!sys->topology) {
        printf("错误: 无法创建拓扑网络\n");
        memory_system_destroy(sys->memory);
        free(sys);
        return NULL;
    }
    
    // 添加子拓扑（扩容以支持书籍数量）
    master_add_sub_topology(sys->topology, TOPO_VOCABULARY, "词汇拓扑", 30000, 10);
    master_add_sub_topology(sys->topology, TOPO_SEMANTIC, "语义拓扑", 12000, 9);
    master_add_sub_topology(sys->topology, TOPO_EMOTION, "情绪拓扑", 4000, 8);
    master_add_sub_topology(sys->topology, TOPO_SYNTAX, "语法拓扑", 1000, 7);
    master_add_sub_topology(sys->topology, TOPO_CONTEXT, "上下文拓扑", 1000, 6);
    master_add_sub_topology(sys->topology, TOPO_DOMAIN, "领域拓扑", 1000, 5);
    master_add_sub_topology(sys->topology, TOPO_PRAGMA, "语用拓扑", 1000, 4);
    master_add_sub_topology(sys->topology, TOPO_CULTURE, "文化拓扑", 1000, 3);
    master_add_sub_topology(sys->topology, TOPO_CONCEPT, "概念拓扑", 12000, 9);
    master_add_sub_topology(sys->topology, TOPO_MASTER, "主拓扑", 100, 0);
    master_add_sub_topology(sys->topology, TOPO_TEMPLATE, "模板拓扑", 4000, 8);
    
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
    printf("[3/6] 创建因果推理系统...\n");
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
    printf("[4/6] 创建主动学习器...\n");
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
    printf("[5/6] 创建对话系统...\n");
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

    // 6. 创建后台时钟
    printf("[6/6] 创建后台时钟...\n");
    sys->bg_clock = background_clock_create(sys->topology, sys->memory,
                                           sys->dialog->cognitive_state);
    if (!sys->bg_clock) {
        printf("错误: 无法创建后台时钟\n");
        dialog_system_destroy(sys->dialog);
        active_learner_destroy(sys->learner);
        causal_graph_destroy(sys->causal_graph);
        master_topology_destroy(sys->topology);
        memory_system_destroy(sys->memory);
        free(sys);
        return NULL;
    }
    printf("     ✓ 后台时钟就绪 (tick=%dms, decay=%.3f)\n",
           PM_CLOCK_TICK_INTERVAL_MS, PM_CLOCK_DECAY_PER_TICK);
    
    // 尝试加载之前保存的拓扑状态
    const char* state_file = "pivotmind_state.dat";
    if (access(state_file, F_OK) == 0) {
        int loaded = master_load_state(sys->topology, state_file);
        if (loaded >= 0) {
            printf("     ✓ 已加载拓扑状态 (%d 节点)\n", loaded);
        }
    }

    // 加载/初始化特征向量
    {
        int feat_loaded = load_features(sys->topology, "features.bin");
        if (feat_loaded > 0) {
            printf("     ✓ 已加载特征向量 (%d 节点)\n", feat_loaded);
        } else {
            int initted = init_random_features(sys->topology);
            printf("     ✓ 已初始化特征向量 (%d 节点)\n", initted);
        }
    }

    // 可选: 从预训练 Word2Vec 嵌入迁移特征
    {
        const char* pretrain_file = "pretrain_embeddings.bin";
        if (access(pretrain_file, F_OK) == 0) {
            Vocab* pretrain_vocab = vocab_create(10000);
            PretrainState* ps = pretrain_state_load(pretrain_vocab, pretrain_file);
            if (ps) {
                int migrated = feature_transfer_pretrained(sys->topology, ps);
                if (migrated > 0) {
                    printf("     ✓ 预训练嵌入迁移完成\n");
                }
                pretrain_state_destroy(ps);
            }
            vocab_destroy(pretrain_vocab);
        }
    }

    // 加载/重建跨拓扑连接
    {
        int cross_loaded = load_cross_edges(sys->topology, "cross_edges.bin");
        if (cross_loaded > 0) {
            printf("     ✓ 已加载跨拓扑连接 (%d 条)\n", cross_loaded);
        } else {
            int rebuilt = rebuild_cross_connections(sys->topology);
            printf("     ✓ 已重建跨拓扑连接 (%d 条)\n", rebuilt);
        }
    }
    
    // 加载记忆种子
    const char* mem_file = "memory_seed.dat";
    memory_load_seed(sys->memory, mem_file);

    // ========== 模板拓扑：自动构建 ==========
    {
        SubTopology* tpl = master_get_sub_topology_by_type(sys->topology, TOPO_TEMPLATE);
        SubTopology* vocab = master_get_sub_topology_by_type(sys->topology, TOPO_VOCABULARY);
        if (tpl && tpl->net && tpl->net->node_count == 0 && vocab && vocab->net->node_count > 500) {
            /* 冷启动：频率表为空，先做少量初始走边填充频率表 */
            PathFrequencyTable* freq = sys->topology->freq_table;
            if (freq && freq->entry_count < 100) {
                printf("     → 初始化路径频率表（首轮走边）...\n");
                int nc = vocab->net->node_count;
                int ns = (nc < 100) ? nc : 100;
                int bms = (nc + 7) / 8;
                for (int s = 0; s < ns; s++) {
                    ReasoningNode* sn = vocab->net->nodes[s];
                    if (!sn || sn->connection_count <= 0) continue;
                    float saved = sn->activation;
                    sn->activation = 0.8f;
                    unsigned char* vis = (unsigned char*)calloc((size_t)bms, 1);
                    int dummy_pn[32]; float dummy_ps[32];
                    (void)dummy_pn; (void)dummy_ps;
                    topology_walk_greedy(vocab, sn->node_id, dummy_pn, dummy_ps,
                                         20, vis, 1.0f, sys->topology, NULL, NULL);
                    sn->activation = saved;
                    free(vis);
                }
                printf("     → 频率表就绪 (%d 条目)\n", freq->entry_count);
            }
            printf("     → 自动构建模板拓扑...\n");
            int built = template_auto_build(sys->topology, 500, 100);
            if (built > 0) {
                printf("     ✓ 已自动构建 %d 个模板节点\n", built);
                sys->topology->use_template_voting = 1;
            } else {
                printf("     → 模板构建跳过（数据不足，将在对话中逐步积累）\n");
            }
        } else if (tpl && tpl->net && tpl->net->node_count > 0) {
            printf("     ✓ 模板拓扑已就绪 (%d 节点)\n", tpl->net->node_count);
            sys->topology->use_template_voting = 1;
        }
    }
    
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
        
        // 保存特征向量
        int feat_saved = save_features(sys->topology, "features.bin");
        if (feat_saved > 0) {
            printf("  ✓ 已保存特征向量 (%d 节点)\n", feat_saved);
        }
        
        // 保存跨拓扑连接
        int cross_saved = save_cross_edges(sys->topology, "cross_edges.bin");
        if (cross_saved > 0) {
            printf("  ✓ 已保存跨拓扑连接 (%d 条)\n", cross_saved);
        }
    }
    
    // 停止所有进程
    if (sys->learner) {
        active_learner_stop(sys->learner);
        active_learner_destroy(sys->learner);
    }
    
    if (sys->bg_clock) {
        background_clock_destroy(sys->bg_clock);
    }
    
    if (sys->dialog) {
        dialog_system_destroy(sys->dialog);
    }
    
    if (sys->topology) {
        master_topology_destroy(sys->topology);
    }
    
    if (sys->memory) {
        // 保存记忆种子
        const char* mem_file = "memory_seed.dat";
        int saved = memory_save_seed(sys->memory, mem_file);
        if (saved >= 0) {
            printf("  ✓ 已保存记忆种子 (%d 条)\n", saved);
        }
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
    
    // 启动主动学习器（后台线程，5分钟周期）
    active_learner_start(sys->learner);

    // 启动后台时钟（后台线程，1秒tick）
    background_clock_start(sys->bg_clock);
    
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
    
    background_clock_stop(sys->bg_clock);
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
    printf("  verbose on/off- 开启/关闭后台日志输出\n");
    printf("  stats         - 显示统计信息\n");
    printf("  templates     - 显示/构建模板拓扑\n");
    printf("  templates on  - 开启模板投票\n");
    printf("  templates off - 关闭模板投票\n");
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
    
    if (sys->bg_clock) {
        printf("后台时钟 tick: %d\n", sys->bg_clock->tick_count);
    }
    
    if (sys->topology) {
        int total_nodes = 0;
        for (int t = 0; t < sys->topology->sub_topo_count; t++) {
            if (sys->topology->sub_topologies[t] && sys->topology->sub_topologies[t]->net) {
                total_nodes += sys->topology->sub_topologies[t]->net->node_count;
            }
        }
        printf("拓扑节点: %d\n", total_nodes);

        SubTopology* tpl = master_get_sub_topology_by_type(sys->topology, TOPO_TEMPLATE);
        if (tpl && tpl->net) {
            printf("模板节点: %d (投票: %s)\n", tpl->net->node_count,
                   sys->topology->use_template_voting ? "ON" : "OFF");
        }
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
                printf("\n★ 已学习更好的回答: %s\n", better_answer);
                // 存入记忆系统，下次直接命中
                if (sys && sys->memory && response) {
                    // 用用户输入作为key，存入response模式
                    // 提取用户输入中的关键词
                    char* input_copy = strdup(input);
                    char* stripped = input_copy;
                    while (*stripped == ' ') stripped++;
                    // 去掉末尾的换行
                    size_t len = strlen(stripped);
                    while (len > 0 && (stripped[len-1] == '\n' || stripped[len-1] == '\r')) stripped[--len] = 0;
                    if (strlen(stripped) > 0) {
                        char key[512];
                        snprintf(key, sizeof(key), "response:%s", stripped);
                        memory_store(sys->memory, key, (void*)better_answer,
                                   strlen(better_answer) + 1, MEMORY_TYPE_STRING, 0.95f);
                        printf("  → 已存入记忆: 下次听到「%s」就会用这个回答\n", stripped);
                    }
                    free(input_copy);
                }
            }
        }
        
        if (reasoning) {
            dialog_reasoning_destroy(reasoning);
        }
        
        sys->total_dialogs++;

        /* 周期性模板维护: 每 100 轮对话尝试模板构建 + 冷路径衰减 */
        if (sys->total_dialogs % 100 == 0 && sys->topology->freq_table &&
            sys->topology->freq_table->entry_count > 500) {
            printf("     → 周期性模板维护 (第 %lld 轮)...\n", (long long)sys->total_dialogs);
            int built = template_auto_build(sys->topology, 500, 100);
            if (built > 0) printf("     ✓ 新增 %d 个模板节点\n", built);
            int decayed = template_decay_inactive_links(sys->topology, 20, 0.85f);
            if (decayed > 0) printf("     ✓ 衰减 %d 条冷链接\n", decayed);
        }
        
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
        } else if (strncmp(input, "templates", 9) == 0) {
            SubTopology* tpl = master_get_sub_topology_by_type(sys->topology, TOPO_TEMPLATE);
            if (strcmp(input, "templates") == 0) {
                if (tpl && tpl->net) {
                    printf("\n=== 模板拓扑状态 ===\n");
                    printf("模板节点: %d\n", tpl->net->node_count);
                    printf("模板投票: %s\n", sys->topology->use_template_voting ? "ON" : "OFF");
                    printf("最近 10 个模板:\n");
                    int start = (tpl->net->node_count > 10) ? tpl->net->node_count - 10 : 0;
                    for (int i = start; i < tpl->net->node_count; i++) {
                        ReasoningNode* tn = tpl->net->nodes[i];
                        if (tn) printf("  [%d] %s\n", i, tn->concept);
                    }
                } else {
                    printf("\n没有模板节点\n");
                }
            } else if (strcmp(input, "templates on") == 0) {
                sys->topology->use_template_voting = 1;
                printf("  模板投票: 已开启\n");
            } else if (strcmp(input, "templates off") == 0) {
                sys->topology->use_template_voting = 0;
                printf("  模板投票: 已关闭\n");
            } else if (strcmp(input, "templates build") == 0) {
                printf("  → 手动构建模板...\n");
                int built = template_auto_build(sys->topology, 500, 100);
                if (built > 0) {
                    sys->topology->use_template_voting = 1;
                    printf("  ✓ 已构建 %d 个模板节点，投票已开启\n", built);
                } else {
                    printf("  → 模板构建跳过（数据不足或模板已存在）\n");
                }
            } else {
                printf("  用法: templates [on|off|build]\n");
            }
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
        } else if (strncmp(input, "verbose", 7) == 0) {
            if (strcmp(input, "verbose on") == 0) {
                active_learner_set_verbose(sys->learner, 1);
                background_clock_set_verbose(sys->bg_clock, 1);
                printf("  [系统] 后台日志: 开启\n");
            } else if (strcmp(input, "verbose off") == 0) {
                active_learner_set_verbose(sys->learner, 0);
                background_clock_set_verbose(sys->bg_clock, 0);
                printf("  [系统] 后台日志: 关闭\n");
            } else {
                printf("  用法: verbose on / verbose off\n");
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