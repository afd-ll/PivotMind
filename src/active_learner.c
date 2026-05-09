/**
 * @file active_learner.c
 * @brief 主动学习器 - 后台持续学习模块
 * 
 * 功能：
 * 1. 自动从各种来源获取新知识
 * 2. 分析概念关系并扩展拓扑网络
 * 3. 与对话系统并行运行
 * 4. 7×24小时持续学习
 */

#include "active_learner.h"
#include "multi_topology.h"
#include "memory_system.h"
#include "huarong_topology.h"
#include "utf8_tokenizer.h"
#include "dialog_system.h"
#include "cognitive_params.h"
#include "node_importance.h"
#include "topology_growth.h"
#include "catastrophic_forgetting.h"
#include "memory_arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

// ==================== 主动学习器核心 ====================

ActiveLearner* active_learner_create(MasterTopology* master, MemorySystem* memory) {
    ActiveLearner* learner = (ActiveLearner*)calloc(1, sizeof(ActiveLearner));
    if (!learner) return NULL;
    
    learner->master = master;
    learner->memory = memory;
    learner->is_running = 0;
    learner->learning_interval = 300;  // 默认5分钟一次
    
    // 学习统计
    learner->total_concepts_learned = 0;
    learner->total_relations_learned = 0;
    learner->total_forgotten = 0;
    learner->start_time = time(NULL);
    
    // 初始化线程
    pthread_mutex_init(&learner->mutex, NULL);
    
    // 创建对象池
    learner->metric_pool = object_pool_create(sizeof(ImportanceMetrics), 50);
    
    return learner;
}

void active_learner_destroy(ActiveLearner* learner) {
    if (!learner) return;
    
    if (learner->is_running) {
        active_learner_stop(learner);
    }
    
    pthread_mutex_destroy(&learner->mutex);
    if (learner->metric_pool) {
        object_pool_destroy((ObjectPool*)learner->metric_pool);
    }
    free(learner);
    
    printf("[主动学习] 学习器已销毁\n");
}

// ==================== 学习周期控制 ====================

void* learning_cycle(void* arg) {
    ActiveLearner* learner = (ActiveLearner*)arg;
    
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
    
    while (learner->is_running) {
        printf("\n=== [主动学习] 开始学习周期 ===\n");
        
        // 阶段1：从记忆系统学习
        learn_from_memory(learner);
        
        // 阶段2：发现新关系（共现分析+特征向量）
        discover_new_relations(learner);
        
        // 阶段2.5：节点重要性评估（动态评估拓扑健康度）
        printf("\n[学习阶段2.5] 评估节点重要性...\n");
        if (learner->master) {
            NodeImportanceEvaluator* eval = node_importance_create(0.85f, 20);
            if (eval) {
                for (int t = 0; t < learner->master->sub_topo_count; t++) {
                    SubTopology* sub = learner->master->sub_topologies[t];
                    if (!sub || !sub->net || sub->net->node_count < 2) continue;
                    int count = 0;
                    ImportanceMetrics** metrics = evaluate_all_nodes(eval, sub->net, &count);
                    if (metrics && count > 0) {
                        ImportanceSummary* summary = generate_importance_summary(eval, metrics, count);
                        if (summary) {
                            printf("  [%s] 高=%d 中=%d 低=%d 可剪枝=%d\n",
                                   sub->name,
                                   summary->high_importance_count,
                                   summary->medium_importance_count,
                                   summary->low_importance_count,
                                   summary->prune_candidate_count);
                            free(summary);
                        }
                        for (int m = 0; m < count; m++) free(metrics[m]);
                        free(metrics);
                    }
                }
                node_importance_destroy(eval);
            }
        }
        
        // 阶段3：清理过时知识（低置信度连接遗忘）
        cleanup_forgotten_knowledge(learner);
        
        // 阶段3.5：拓扑自动增长/收缩
        printf("\n[学习阶段3.5] 评估拓扑增长需求...\n");
        if (learner->master) {
            for (int t = 0; t < learner->master->sub_topo_count; t++) {
                SubTopology* sub = learner->master->sub_topologies[t];
                if (!sub || !sub->net) continue;
                if (check_growth_needed(learner->master, t)) {
                    int new_count = auto_extend_topology(learner->master, t);
                    if (new_count > 0) {
                        printf("  [%s] 拓扑增长: %d 节点\n", sub->name, new_count);
                    }
                }
                // 每5个周期检查一次是否需要收缩
                static int shrink_counter = 0;
                if (++shrink_counter % 5 == 0) {
                    // 清理孤立节点
                    int pruned = prune_isolated_nodes(learner->master, t);
                    if (pruned > 0) {
                        printf("  [%s] 清理孤立节点: %d\n", sub->name, pruned);
                    }
                }
            }
        }
        
        // 记忆巩固：短期→长期记忆转移
        if (learner->memory) {
            memory_consolidate(learner->memory);
        }
        
        // 阶段4：持续学习巩固（每10个周期执行一次EWC风格巩固）
        static int consolidation_counter = 0;
        if (++consolidation_counter % 10 == 0) {
            printf("\n[学习阶段4] 持续学习巩固...\n");
            if (learner->master) {
                EWCConfig* ewc = ewc_config_create();
                if (ewc) {
                    FisherInfoMatrix* fisher = fisher_info_create(100, 0.1f);
                    if (fisher) {
                        for (int t = 0; t < learner->master->sub_topo_count && t < 2; t++) {
                            SubTopology* sub = learner->master->sub_topologies[t];
                            if (!sub || !sub->net) continue;
                            for (int n = 0; n < sub->net->node_count && n < 20; n++) {
                                if (sub->net->nodes[n]) {
                                    fisher_info_update(fisher, sub->net, sub->net->nodes[n]->node_id);
                                }
                            }
                        }
                        fisher_info_destroy(fisher);
                    }
                    ewc_config_destroy(ewc);
                }
            }
        }
        
        // 打印统计
        print_learning_stats(learner);
        
        // 等待下一个周期
        printf("[主动学习] 下一周期等待 %d 秒...\n", learner->learning_interval);
        for (int i = 0; i < learner->learning_interval && learner->is_running; i++) {
            sleep(1);
        }
    }
    
    return NULL;
}

void active_learner_start(ActiveLearner* learner) {
    if (!learner) return;
    
    pthread_mutex_lock(&learner->mutex);
    if (learner->is_running) {
        pthread_mutex_unlock(&learner->mutex);
        return;
    }
    
    learner->is_running = 1;
    pthread_mutex_unlock(&learner->mutex);
    
    if (pthread_create(&learner->thread, NULL, learning_cycle, learner) != 0) {
        printf("[主动学习] 错误: 无法创建学习线程\n");
        pthread_mutex_lock(&learner->mutex);
        learner->is_running = 0;
        pthread_mutex_unlock(&learner->mutex);
        return;
    }
    
    printf("[主动学习] 学习器已启动，PID: %lu\n", (unsigned long)learner->thread);
}

void active_learner_stop(ActiveLearner* learner) {
    if (!learner) return;
    
    pthread_mutex_lock(&learner->mutex);
    if (!learner->is_running) {
        pthread_mutex_unlock(&learner->mutex);
        return;
    }
    learner->is_running = 0;
    pthread_mutex_unlock(&learner->mutex);
    
    pthread_join(learner->thread, NULL);
    
    printf("[主动学习] 学习器已停止\n");
}

void active_learner_set_interval(ActiveLearner* learner, int seconds) {
    if (seconds < 60) seconds = 60;  // 最小1分钟
    if (seconds > 86400) seconds = 86400;  // 最大1天
    
    learner->learning_interval = seconds;
}

// ==================== 学习源：从记忆系统学习 ====================

void learn_from_memory(ActiveLearner* learner) {
    printf("\n[学习阶段1] 从记忆系统学习...\n");
    
    if (!learner->memory || !learner->master) return;
    
    // 记忆巩固：短期记忆 → 长期记忆
    memory_consolidate(learner->memory);
    
    // 遍历短期记忆中的条目，提取新概念加入拓扑
    int learned_count = 0;
    
    // 尝试搜索高频记忆条目
    MemorySearchResult* results = NULL;
    int result_count = 0;
    
    // 以空查询搜索，获取所有高置信度条目
    results = memory_search(learner->memory, "", &result_count, 0.3f);
    
    if (results && result_count > 0) {
        printf("  待巩固条目: %d\n", result_count);
        
        for (int i = 0; i < result_count && i < 20; i++) {
            MemoryEntry* entry = results[i].entry;
            if (!entry || !entry->key) continue;
            
            // 提取关键词作为概念
            // 如果拓扑中暂无该概念，就添加到词汇拓扑
            SubTopology* vocab = master_get_sub_topology_by_type(learner->master, TOPO_VOCABULARY);
            if (vocab && vocab->net) {
                // 检查是否已存在
                int exists = 0;
                for (int n = 0; n < vocab->net->node_count; n++) {
                    if (vocab->net->nodes[n] && vocab->net->nodes[n]->concept &&
                        strstr(results[i].entry->key, vocab->net->nodes[n]->concept)) {
                        exists = 1;
                        break;
                    }
                }
                if (!exists && strlen(results[i].entry->key) > 2) {
                    // 提取简短的key作为概念名
                    huarong_net_add_node(vocab->net, results[i].entry->key, NULL, 0);
                    learned_count++;
                }
            }
        }
        
        free(results);
    }
    
    // 更新统计
    if (learned_count > 0) {
        learner->total_concepts_learned += learned_count;
        printf("  ✓ 从记忆提取了 %d 个新概念\n", learned_count);
    } else {
        printf("  - 暂无新知识需要学习\n");
    }
}

// ==================== 关系发现 ====================

// ==================== P2: 自动连接发现与特征向量 ====================

/**
 * 计算两个特征向量的余弦相似度
 * @return [-1, 1] 范围，1表示完全相似
 */
static float cosine_similarity(const float* vec_a, const float* vec_b, int dim) {
    if (!vec_a || !vec_b || dim <= 0) return 0.0f;

    float dot_product = 0.0f;
    float norm_a = 0.0f;
    float norm_b = 0.0f;

    for (int i = 0; i < dim; i++) {
        dot_product += vec_a[i] * vec_b[i];
        norm_a += vec_a[i] * vec_a[i];
        norm_b += vec_b[i] * vec_b[i];
    }

    if (norm_a < 1e-8 || norm_b < 1e-8) return 0.0f;
    return dot_product / (sqrtf(norm_a) * sqrtf(norm_b));
}

/**
 * 概念字符串相似度（简单编辑距离归一化）
 * @return [0, 1] 范围，1表示完全相同
 */
static float concept_string_similarity(const char* a, const char* b) {
    if (!a || !b) return 0.0f;
    if (strcmp(a, b) == 0) return 1.0f;

    // 简单检查：是否有共同子串
    int len_a = strlen(a);
    int len_b = strlen(b);
    int common = 0;

    for (int i = 0; i < len_a - 1; i++) {
        for (int j = 0; j < len_b - 1; j++) {
            if (a[i] == b[j] && a[i+1] == b[j+1]) {
                common++;
            }
        }
    }

    int max_len = (len_a > len_b) ? len_a : len_b;
    return (float)common / (max_len * 0.5f);
}

/**
 * 检查两个节点是否已有连接
 */
static int has_connection(ReasoningNode* a, ReasoningNode* b) {
    for (int i = 0; i < a->connection_count; i++) {
        if (a->connections[i] == b) {
            return 1;
        }
    }
    return 0;
}

/**
 * 基于共现分析发现潜在连接
 * 如果两个节点在相同上下文中出现，它们可能有语义关联
 */
static float cooccurrence_score(ReasoningNode* a, ReasoningNode* b,
                                int same_topo, int hop_distance) {
    float score = 0.0f;

    // 同一拓扑内的节点加分
    if (same_topo) score += 0.3f;

    // 跳数近的节点加分
    if (hop_distance == 1) score += 0.4f;
    else if (hop_distance == 2) score += 0.2f;

    // 检查激活值相似度（相似的激活模式可能表示相关概念）
    float act_diff = fabsf(a->activation - b->activation);
    if (act_diff < 0.1f) score += 0.2f;

    return score;
}

/**
 * 计算两个节点的综合相似度（用于决定是否建立连接）
 */
static float compute_connection_score(ReasoningNode* a, ReasoningNode* b,
                                       int same_topo, int hop_distance) {
    float score = 0.0f;

    // 1. 特征向量相似度（如果有特征向量）
    if (a->feature_dim > 0 && b->feature_dim > 0 && a->features && b->features) {
        int dim = (a->feature_dim < b->feature_dim) ? a->feature_dim : b->feature_dim;
        score += cosine_similarity(a->features, b->features, dim) * 0.4f;
    }

    // 2. 字符串相似度
    score += concept_string_similarity(a->concept, b->concept) * 0.3f;

    // 3. 共现分析
    score += cooccurrence_score(a, b, same_topo, hop_distance) * 0.3f;

    return score;
}

/**
 * 基于共现分析自动发现并建立连接
 * @param learner 主动学习器
 * @param topo_id 拓扑ID
 * @param window_size 共现窗口大小
 * @return 新建立的连接数
 */
int discover_connections_by_cooccurrence(ActiveLearner* learner,
                                         int topo_id, int window_size) {
    if (!learner || !learner->master) return 0;

    SubTopology* sub = master_get_sub_topology(learner->master, topo_id);
    if (!sub || !sub->net || sub->net->node_count < 2) return 0;

    int new_connections = 0;
    float threshold = 0.35f;  // 相似度阈值，超过此值建立连接

    printf("  [共现分析] 窗口大小=%d, 阈值=%.2f\n", window_size, threshold);

    // 遍历节点对
    for (int i = 0; i < sub->net->node_count; i++) {
        ReasoningNode* node_a = sub->net->nodes[i];
        if (!node_a) continue;

        for (int j = i + 1; j < sub->net->node_count; j++) {
            ReasoningNode* node_b = sub->net->nodes[j];
            if (!node_b) continue;

            // 跳过已有连接
            if (has_connection(node_a, node_b)) continue;

            // 计算综合相似度
            float score = compute_connection_score(node_a, node_b, 1, 1);

            if (score >= threshold) {
                // 根据相似度计算连接权重
                float weight = 0.3f + (score - threshold) * 0.5f;
                if (weight > 0.9f) weight = 0.9f;

                huarong_net_add_connection(sub->net, i, j, weight);
                new_connections++;

                if (new_connections <= 5) {  // 只打印前几个
                    printf("    发现关联: 【%s】 <-> 【%s】 (score=%.2f, weight=%.2f)\n",
                           node_a->concept, node_b->concept, score, weight);
                }
            }
        }
    }

    return new_connections;
}

/**
 * 基于特征向量相似度更新现有连接的权重
 */
int update_connection_weights_by_features(ActiveLearner* learner, int topo_id) {
    if (!learner || !learner->master) return 0;

    SubTopology* sub = master_get_sub_topology(learner->master, topo_id);
    if (!sub || !sub->net) return 0;

    int updated = 0;

    for (int i = 0; i < sub->net->node_count; i++) {
        ReasoningNode* node = sub->net->nodes[i];
        if (!node || node->connection_count == 0) continue;

        if (node->feature_dim <= 0 || !node->features) continue;

        for (int c = 0; c < node->connection_count; c++) {
            ReasoningNode* connected = node->connections[c];
            if (!connected) continue;
            if (connected->feature_dim <= 0 || !connected->features) continue;

            // 计算特征向量相似度
            int dim = (node->feature_dim < connected->feature_dim) ?
                       node->feature_dim : connected->feature_dim;
            float sim = cosine_similarity(node->features, connected->features, dim);

            // 动态调整权重：基础权重 + 相似度调整
            float base_weight = node->connection_weights[c];
            float new_weight = base_weight * 0.7f + sim * 0.3f;

            // 确保权重在合理范围
            if (new_weight > 0.95f) new_weight = 0.95f;
            if (new_weight < 0.1f) new_weight = 0.1f;

            if (fabsf(new_weight - base_weight) > 0.05f) {
                node->connection_weights[c] = new_weight;
                updated++;
            }
        }
    }

    if (updated > 0) {
        printf("  [特征更新] 更新了 %d 个连接权重\n", updated);
    }

    return updated;
}

void discover_new_relations(ActiveLearner* learner) {
    printf("\n[学习阶段2] 发现新概念关系 (智能发现)...\n");

    if (!learner->master) return;

    int total_new_relations = 0;
    int total_updated_weights = 0;

    // 遍历现有拓扑，发现潜在关系
    for (int t = 0; t < learner->master->sub_topo_count; t++) {
        SubTopology* sub = learner->master->sub_topologies[t];
        if (!sub || !sub->net || sub->net->node_count < 2) continue;

        printf("  扫描 %s (节点数=%d):\n", sub->name, sub->net->node_count);

        // P2-1: 基于共现分析发现新连接
        int new_connections = discover_connections_by_cooccurrence(learner, t, 2);
        total_new_relations += new_connections;

        // P2-2: 基于特征向量更新现有连接权重
        int updated = update_connection_weights_by_features(learner, t);
        total_updated_weights += updated;
    }

    learner->total_relations_learned += total_new_relations;

    if (total_new_relations > 0) {
        printf("  ✓ 共发现 %d 个新关系，更新 %d 个连接权重\n",
               total_new_relations, total_updated_weights);
    } else {
        printf("  - 暂无新关系需要建立\n");
    }
}

// ==================== 遗忘机制 ====================

void cleanup_forgotten_knowledge(ActiveLearner* learner) {
    printf("\n[学习阶段3] 清理过时知识...\n");
    
    if (!learner->master) return;
    
    int forgotten_count = 0;
    float threshold = 0.15f;  // 置信度低于0.15视为遗忘
    
    // 遍历所有拓扑，清理低激活节点
    for (int t = 0; t < learner->master->sub_topo_count; t++) {
        SubTopology* sub = learner->master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        
        // 简化处理：随机清理一些低权重的连接
        for (int i = 0; i < sub->net->node_count; i++) {
            ReasoningNode* node = sub->net->nodes[i];
            if (!node) continue;
            
            // 清理低权重连接
            int new_count = 0;
            for (int c = 0; c < node->connection_count; c++) {
                if (node->connection_weights[c] > threshold) {
                    // 保留
                    if (new_count != c) {
                        node->connections[new_count] = node->connections[c];
                        node->connection_weights[new_count] = node->connection_weights[c];
                    }
                    new_count++;
                } else {
                    forgotten_count++;
                }
            }
            node->connection_count = new_count;
        }
    }
    
    learner->total_forgotten += forgotten_count;
    
    if (forgotten_count > 0) {
        printf("  ✓ 清理了 %d 个过时连接\n", forgotten_count);
    } else {
        printf("  - 暂无需要清理的知识\n");
    }
}

// ==================== 学习统计 ====================

void print_learning_stats(ActiveLearner* learner) {
    printf("\n=== 主动学习统计 ===\n");
    printf("  运行时间: %lld 秒\n", (long long)(time(NULL) - learner->start_time));
    printf("  学习周期间隔: %d 秒\n", learner->learning_interval);
    printf("  累计学习概念: %d\n", learner->total_concepts_learned);
    printf("  累计建立关系: %d\n", learner->total_relations_learned);
    printf("  累计遗忘/清理: %d\n", learner->total_forgotten);
    
    // 计算活跃度
    if (learner->master) {
        int total_nodes = 0;
        for (int t = 0; t < learner->master->sub_topo_count; t++) {
            if (learner->master->sub_topologies[t] && 
                learner->master->sub_topologies[t]->net) {
                total_nodes += learner->master->sub_topologies[t]->net->node_count;
            }
        }
        printf("  当前概念数: %d\n", total_nodes);
    }
}

// ==================== 用户反馈学习 ====================
// 参数说明:
// - should_lock: 是否需要加锁 (true=独立调用, false=已在外层加锁)

static void update_node_from_feedback(ReasoningNode* node, float feedback_valence, int is_correct) {
    if (!node) return;
    
    node_update_valence(&node->valence, feedback_valence, 0.3f);
    printf("  → 节点[%d]%s 效价更新: %.3f\n", 
           node->node_id, node->concept ? node->concept : "?", node->valence);
    
    if (node->cognitive_confidence) {
        float new_satisfaction = is_correct ? 0.9f : 0.1f;
        cognitive_confidence_update(node->cognitive_confidence,
            node->cognitive_confidence->predictive_accuracy,
            new_satisfaction,
            node->cognitive_confidence->novelty_bonus);
    }
    
    float motivation_delta = is_correct ? 0.1f : -0.1f;
    if (node->connection_motivational_bias && node->connection_count > 0) {
        for (int c = 0; c < node->connection_count; c++) {
            float new_motivation = node->connection_motivational_bias[c] + motivation_delta;
            node->connection_motivational_bias[c] = clamp_float(new_motivation, 0.1f, 1.0f);
        }
        printf("  → 边动机倾向已更新\n");
    }
}

void learn_from_feedback(ActiveLearner* learner, const char* question,
                        const char* correct_answer, int is_correct) {
    printf("\n[学习] 从用户反馈学习...\n");
    printf("  问题: %s\n", question);
    printf("  回答: %s\n", correct_answer);
    printf("  结果: %s\n", is_correct ? "✓ 正确 - 强化" : "✗ 错误 - 修正");
    
    if (!learner->master || !learner->memory) return;
    
    pthread_mutex_lock(&learner->mutex);
    
    float feedback_valence = is_correct ? 0.6f : -0.6f;
    
    char key[512];
    snprintf(key, 511, "input:%s", question);
    
    if (is_correct) {
        MemoryEntry* mem = memory_retrieve(learner->memory, key);
        if (mem) {
            float new_importance = mem->importance + 0.1f;
            if (new_importance > 1.0f) new_importance = 1.0f;
            memory_update_confidence(learner->memory, key, new_importance);
            printf("  → 重要性提升: %.3f → %.3f\n", mem->importance, new_importance);
        }
    } else {
        MemoryEntry* mem = memory_retrieve(learner->memory, key);
        if (mem) {
            float new_importance = mem->importance - 0.15f;
            printf("  → 重要性下降: %.3f → %.3f\n", mem->importance, new_importance);
            
            if (new_importance < 0.1f) {
                printf("  → 知识被遗忘\n");
                learner->total_forgotten++;
            } else {
                memory_update_confidence(learner->memory, key, new_importance);
            }
        }
        
        memory_store(learner->memory, key, strdup(correct_answer),
                    strlen(correct_answer) + 1, MEMORY_TYPE_STRING, 0.3f);
    }
    
    for (int t = 0; t < learner->master->sub_topo_count; t++) {
        SubTopology* sub = learner->master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (!node) continue;
            
            if (node->concept && strstr(node->concept, question)) {
                update_node_from_feedback(node, feedback_valence, is_correct);
            }
        }
    }
    
    pthread_mutex_unlock(&learner->mutex);
}

// ==================== 非自主纠偏 ====================
// 注意：学习本身已移到 autonomic_learn_from_dialog()
// feedback_correct 只做一件事：用户说不对时压下置信度

void feedback_correct(ActiveLearner* learner, const char* user_input,
                     const char* ai_response, const char* user_feedback) {
    // feedback_correct 是 learn_from_dialog 的语义别名
    // 两者行为一致：都需要显式用户反馈才能操作
    learn_from_dialog(learner, user_input, ai_response, user_feedback);
}

// ==================== 对话中学习（旧接口，保留兼容） ====================
// 注意：此函数必须有 user_feedback 才会执行
// 没有反馈则直接返回（不做任何事）
// 真正的自主学习请使用 autonomic_learn_from_dialog()

void learn_from_dialog(ActiveLearner* learner, const char* user_input,
                      const char* ai_response, const char* user_feedback) {
    if (!learner || !user_feedback || strlen(user_feedback) == 0) return;
    if (!learner->master || !learner->memory) return;
    
    printf("\n[学习] 从对话中学习...\n");
    
    pthread_mutex_lock(&learner->mutex);
    
    int is_correct = -1;
    if (strcmp(user_feedback, "correct") == 0 || 
        strcmp(user_feedback, "对") == 0 ||
        strcmp(user_feedback, "是的") == 0 ||
        strcmp(user_feedback, "👍") == 0) {
        is_correct = 1;
    } else if (strcmp(user_feedback, "wrong") == 0 ||
             strcmp(user_feedback, "错") == 0 ||
             strcmp(user_feedback, "不是") == 0 ||
             strcmp(user_feedback, "👎") == 0) {
        is_correct = 0;
    } else if (strncmp(user_feedback, "更好的回答:", 12) == 0) {
        const char* better_answer = user_feedback + 12;
        char key[512];
        snprintf(key, 511, "input:%s", user_input);
        memory_store(learner->memory, key, strdup(better_answer),
                    strlen(better_answer) + 1, MEMORY_TYPE_STRING, 0.5f);
        printf("  → 已学习更好的回答: %s\n", better_answer);
        learner->total_concepts_learned++;
        pthread_mutex_unlock(&learner->mutex);
        return;
    }
    
    if (is_correct < 0) {
        pthread_mutex_unlock(&learner->mutex);
        return;
    }
    
    float feedback_valence = is_correct ? 0.6f : -0.6f;
    char key[512];
    snprintf(key, 511, "input:%s", user_input);
    
    if (is_correct) {
        MemoryEntry* mem = memory_retrieve(learner->memory, key);
        if (mem) {
            float new_importance = mem->importance + 0.1f;
            if (new_importance > 1.0f) new_importance = 1.0f;
            memory_update_confidence(learner->memory, key, new_importance);
            printf("  → 重要性提升: %.3f → %.3f\n", mem->importance, new_importance);
        }
    } else {
        MemoryEntry* mem = memory_retrieve(learner->memory, key);
        if (mem) {
            float new_importance = mem->importance - 0.15f;
            printf("  → 重要性下降: %.3f → %.3f\n", mem->importance, new_importance);
            if (new_importance < 0.1f) {
                printf("  → 知识被遗忘\n");
                learner->total_forgotten++;
            } else {
                memory_update_confidence(learner->memory, key, new_importance);
            }
        }
        memory_store(learner->memory, key, strdup(ai_response),
                    strlen(ai_response) + 1, MEMORY_TYPE_STRING, 0.3f);
    }
    
    for (int t = 0; t < learner->master->sub_topo_count; t++) {
        SubTopology* sub = learner->master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (!node) continue;
            
            if (node->concept && strstr(node->concept, user_input)) {
                update_node_from_feedback(node, feedback_valence, is_correct);
            }
        }
    }
    
    pthread_mutex_unlock(&learner->mutex);
}