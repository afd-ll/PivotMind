#include "../include/multi_topology.h"
#include "../include/string_pool.h"
#include "../include/node_hash.h"
#include "../include/associative_reasoning.h"
#include "../include/utf8_tokenizer.h"
#include "../include/cognitive_params.h"
#include "../include/common.h"
#include "../include/thread_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

// ==================== 常量定义 ====================

#define INITIAL_SUB_TOPO_CAPACITY 16
#define INITIAL_CROSS_LINK_CAPACITY 1000
#define MAX_SAFE_LIMIT 1000000
#define EPSILON 1e-10f
#define CROSS_ADJ_INITIAL_SIZE 10000  // 跨拓扑邻接表初始大小

// 拓扑类型名称（只在multi_topology.c中定义一次）
const char* TOPOLOGY_TYPE_NAMES[] = {
    "词汇拓扑", "语义拓扑", "情绪拓扑", "语法拓扑",
    "上下文拓扑", "领域拓扑", "语用拓扑", "文化拓扑", "概念拓扑", "主拓扑"
};

// ==================== 主拓扑管理实现 ====================

MasterTopology* master_topology_create(int max_sub_topos) {
#ifdef _WIN32
    // Windows: 设置控制台为UTF-8模式
    static int console_initialized = 0;
    if (!console_initialized) {
        SetConsoleOutputCP(65001);  // UTF-8代码页
        SetConsoleCP(65001);
        console_initialized = 1;
    }
#endif
    
    MasterTopology* master = (MasterTopology*)malloc(sizeof(MasterTopology));
    if (!master) return NULL;
    
    int capacity = (max_sub_topos > 0) ? max_sub_topos : INITIAL_SUB_TOPO_CAPACITY;
    
    // 创建字符串池
    master->string_pool = string_pool_create(10000);
    if (!master->string_pool) {
        free(master);
        return NULL;
    }
    
    master->sub_topologies = (SubTopology**)calloc(capacity, sizeof(SubTopology*));
    master->sub_topo_count = 0;
    master->sub_topo_capacity = capacity;
    
    master->cross_links = (CrossTopologyLink**)calloc(
        INITIAL_CROSS_LINK_CAPACITY, 
        sizeof(CrossTopologyLink*)
    );
    master->cross_link_count = 0;
    master->cross_link_capacity = INITIAL_CROSS_LINK_CAPACITY;
    
    master->active_topo_id = -1;
    master->active_node_ids = (int*)calloc(capacity, sizeof(int));
    master->activation_levels = (float*)calloc(capacity, sizeof(float));
    
    master->global_learning_rate = 0.01f;
    master->inference_depth = 0;
    master->max_inference_depth = 10;
    
    master->parallel_inference = 0;
    master->auto_switch_topo = 1;

    master->total_inferences = 0;
    master->successful_inferences = 0;
    master->training_data_count = 0;
    master->created_time = time(NULL);

    // 线程池初始化（懒创建，首次并行时再分配）
    master->thread_pool = NULL;
    master->parallel_mode = 0;

    // 初始化跨拓扑邻接表索引
    master->cross_adj = (CrossTopoAdjEntry**)calloc(
        CROSS_ADJ_INITIAL_SIZE, sizeof(CrossTopoAdjEntry*));
    master->cross_adj_count = CROSS_ADJ_INITIAL_SIZE;
    if (!master->cross_adj) {
        master_topology_destroy(master);
        return NULL;
    }

    pthread_rwlock_init(&master->rwlock, NULL);

    return master;
}

void master_topology_destroy(MasterTopology* master) {
    if (!master) return;

    // 销毁所有子拓扑
    for (int i = 0; i < master->sub_topo_count; i++) {
        SubTopology* sub = master->sub_topologies[i];
        if (sub) {
            if (sub->node_hash) node_hash_free(sub->node_hash);
            if (sub->net) huarong_net_destroy(sub->net);
            free(sub);
        }
    }
    free(master->sub_topologies);

    // 销毁所有跨拓扑连接邻接表索引
    if (master->cross_adj) {
        for (int i = 0; i < master->cross_adj_count; i++) {
            CrossTopoAdjEntry* entry = master->cross_adj[i];
            while (entry) {
                CrossTopoAdjEntry* next = entry->next;
                free(entry);
                entry = next;
            }
        }
        free(master->cross_adj);
    }

    // 销毁所有跨拓扑连接
    for (int i = 0; i < master->cross_link_count; i++) {
        free(master->cross_links[i]);
    }
    free(master->cross_links);

    // 销毁字符串池
    if (master->string_pool) {
        string_pool_destroy(master->string_pool);
    }

    free(master->active_node_ids);
    free(master->activation_levels);

    // 销毁线程池
    if (master->thread_pool) {
        thread_pool_destroy(master->thread_pool);
        master->thread_pool = NULL;
    }

    pthread_rwlock_destroy(&master->rwlock);

    free(master);
}

int master_add_sub_topology(MasterTopology* master, 
                           TopologyType type, 
                           const char* name,
                           int initial_capacity,
                           int priority) {
    if (!master || !name) return -1;
    
    // 动态扩容
    if (master->sub_topo_count >= master->sub_topo_capacity) {
        int new_capacity = master->sub_topo_capacity * 2;
        SubTopology** new_topos = (SubTopology**)realloc(
            master->sub_topologies,
            new_capacity * sizeof(SubTopology*)
        );
        if (!new_topos) return -1;
        master->sub_topologies = new_topos;
        master->sub_topo_capacity = new_capacity;
    }
    
    // 创建子拓扑
    SubTopology* sub = (SubTopology*)malloc(sizeof(SubTopology));
    if (!sub) return -1;
    
    sub->topo_id = master->sub_topo_count;
    sub->type = type;
    sub->name = string_pool_intern(master->string_pool, name);
    sub->description = string_pool_intern(master->string_pool, 
                                          TOPOLOGY_TYPE_NAMES[type]);
    
    // 创建底层拓扑网络
    sub->net = huarong_net_create(
        (initial_capacity > 0) ? initial_capacity : 1000,
        100
    );
    if (!sub->net) {
        free(sub);
        return -1;
    }
    
    // 创建节点哈希表（使用素数大小的桶数）
    int hash_buckets = (initial_capacity > 0) ?
                       ((initial_capacity / 4) | 1) : 1009;  // 确保为奇数
    sub->node_hash = node_hash_create(hash_buckets);
    if (!sub->node_hash) {
        huarong_net_destroy(sub->net);
        free(sub);
        return -1;
    }

    // P0-2: 预分配容量，避免后续扩容开销
    if (initial_capacity > 100) {
        node_hash_reserve(sub->node_hash, initial_capacity);
    }

    // 批量添加现有节点到哈希表（加速后续 O(1) 查找）
    int nodes_added = node_hash_add_all_from_net(sub->node_hash, sub->net);
    if (nodes_added > 0) {
        printf("[主拓扑] %s 哈希表已填充 %d 个节点\n", name, nodes_added);
        // P0-2: 打印哈希表详细信息
        node_hash_print_info(sub->node_hash);
    }

    sub->priority = (priority > 0) ? priority : 5;
    sub->weight = 1.0f;
    sub->is_active = 1;
    sub->total_activations = 0;
    sub->avg_activation_value = 0.0f;
    sub->last_used = time(NULL);
    
    master->sub_topologies[master->sub_topo_count++] = sub;
    
    return sub->topo_id;
}

SubTopology* master_get_sub_topology(MasterTopology* master, int topo_id) {
    if (!master || topo_id < 0 || topo_id >= master->sub_topo_count) {
        return NULL;
    }
    return master->sub_topologies[topo_id];
}

SubTopology* master_get_sub_topology_by_type(MasterTopology* master, 
                                             TopologyType type) {
    if (!master) return NULL;
    
    for (int i = 0; i < master->sub_topo_count; i++) {
        if (master->sub_topologies[i]->type == type) {
            return master->sub_topologies[i];
        }
    }
    return NULL;
}

// ==================== 跨拓扑连接实现 ====================

int master_add_cross_link(MasterTopology* master,
                         int from_topo_id, int from_node_id,
                         int to_topo_id, int to_node_id,
                         float weight,
                         const char* relation) {
    if (!master || !relation) return -1;
    
    // 动态扩容
    if (master->cross_link_count >= master->cross_link_capacity) {
        int new_capacity = master->cross_link_capacity * 2;
        CrossTopologyLink** new_links = (CrossTopologyLink**)realloc(
            master->cross_links,
            new_capacity * sizeof(CrossTopologyLink*)
        );
        if (!new_links) return -1;
        master->cross_links = new_links;
        master->cross_link_capacity = new_capacity;
    }
    
    // 创建跨拓扑连接
    CrossTopologyLink* link = (CrossTopologyLink*)malloc(sizeof(CrossTopologyLink));
    if (!link) return -1;
    
    link->link_id = master->cross_link_count;
    link->from_topo_id = from_topo_id;
    link->from_node_id = from_node_id;
    link->to_topo_id = to_topo_id;
    link->to_node_id = to_node_id;
    link->weight = weight;
    link->relation = string_pool_intern(master->string_pool, relation);
    link->bidirectional = 0;
    link->transfer_rate = 0.8f;
    link->created_time = time(NULL);
    link->use_count = 0;
    
    master->cross_links[master->cross_link_count++] = link;

    // 更新跨拓扑邻接表索引 O(1)
    int adj_idx = from_topo_id * 10000 + from_node_id;  // 简化索引：topo_id * max_nodes + node_id
    if (adj_idx >= master->cross_adj_count) {
        // 扩容邻接表
        int new_size = adj_idx + 1000;
        CrossTopoAdjEntry** new_adj = (CrossTopoAdjEntry**)realloc(
            master->cross_adj, new_size * sizeof(CrossTopoAdjEntry*));
        if (new_adj) {
            // 初始化新条目为 NULL
            for (int i = master->cross_adj_count; i < new_size; i++) {
                new_adj[i] = NULL;
            }
            master->cross_adj = new_adj;
            master->cross_adj_count = new_size;
        }
    }

    // 添加到邻接表链表头
    if (adj_idx < master->cross_adj_count) {
        CrossTopoAdjEntry* entry = (CrossTopoAdjEntry*)malloc(sizeof(CrossTopoAdjEntry));
        if (entry) {
            entry->link_index = link->link_id;
            entry->next = master->cross_adj[adj_idx];
            master->cross_adj[adj_idx] = entry;
        }
    }

    // printf("[跨拓扑连接] %s(节点%d) -> %s(节点%d), 关系=%s, 权重=%.2f\n",
    //        from_topo ? from_topo->name : "?", from_node_id,
    //        to_topo ? to_topo->name : "?", to_node_id,
    //        relation, weight);
    
    return link->link_id;
}

// ==================== 节点连接密度优化 ====================

static int calculate_semantic_similarity(const char* concept1, const char* concept2) {
    if (!concept1 || !concept2) return 0;
    
    int len1 = strlen(concept1);
    int len2 = strlen(concept2);
    if (len1 < 2 || len2 < 2) return 0;
    
    int common_chars = 0;
    for (int i = 0; i < len1 - 1; i++) {
        for (int j = 0; j < len2 - 1; j++) {
            if (concept1[i] == concept2[j] && concept1[i+1] == concept2[j+1]) {
                common_chars++;
                break;
            }
        }
    }
    
    int min_len = (len1 < len2) ? len1 : len2;
    return (common_chars * 100) / min_len;
}

static void auto_connect_new_node(MasterTopology* master, SubTopology* sub, ReasoningNode* new_node) {
    if (!master || !sub || !sub->net || !new_node) return;
    
    int max_connections = 8;
    float base_weight = 0.5f;
    
    typedef struct {
        int node_id;
        float similarity;
    } CandidateNode;
    
    // 使用特征向量还是回退到字符相似度
    int use_vector_sim = (new_node->features != NULL && new_node->feature_dim > 0);
    
    CandidateNode candidates[20];
    int candidate_count = 0;
    
    for (int i = 0; i < sub->net->node_count && candidate_count < 20; i++) {
        if (sub->net->nodes[i] == new_node) continue;
        if (sub->net->nodes[i]->node_id == new_node->node_id) continue;
        
        float sim;
        ReasoningNode* peer = sub->net->nodes[i];
        
        if (use_vector_sim && peer->features != NULL && peer->feature_dim == new_node->feature_dim) {
            sim = cosine_similarity(new_node->features, peer->features, new_node->feature_dim);
            if (sim <= 0.2f) continue;
        } else {
            int bigram = calculate_semantic_similarity(new_node->concept, peer->concept);
            if (bigram <= 20) continue;
            sim = (float)bigram / 100.0f;  // 归一化到 [0.0, 1.0]
        }
        
        candidates[candidate_count].node_id = i;
        candidates[candidate_count].similarity = sim;
        candidate_count++;
    }
    
    for (int i = 0; i < candidate_count - 1; i++) {
        for (int j = i + 1; j < candidate_count; j++) {
            if (candidates[j].similarity > candidates[i].similarity) {
                CandidateNode temp = candidates[i];
                candidates[i] = candidates[j];
                candidates[j] = temp;
            }
        }
    }
    
    int connect_count = (candidate_count < max_connections) ? candidate_count : max_connections;
    for (int i = 0; i < connect_count; i++) {
        int target_id = candidates[i].node_id;
        float weight;
        if (use_vector_sim) {
            weight = base_weight + 0.5f * (candidates[i].similarity + 1.0f) / 2.0f;
        } else {
            weight = base_weight + candidates[i].similarity * 0.5f;
        }
        huarong_net_add_connection(sub->net, new_node->node_id, target_id, weight);
    }
    
    if (connect_count > 0) {
        // 调试信息已静音
    }
}

// ==================== 激活传播实现 ====================

int master_activate_node(MasterTopology* master,
                        int topo_id,
                        int node_id,
                        float activation_value) {
    if (!master) return -1;
    if (topo_id < 0 || topo_id >= master->sub_topo_count) return -1;
    
    SubTopology* sub = master->sub_topologies[topo_id];
    if (!sub || !sub->net) return -1;
    if (node_id < 0 || node_id >= sub->net->node_count) return -1;
    
    // 激活节点
    ReasoningNode* node = sub->net->nodes[node_id];
    if (!node) return -1;
    
    // ==================== 使用新参数计算激活 ====================
    // 带效价的激活: base * (1 + valence * 0.5)
    float valence_factor = 1.0f + node->valence * 0.5f;
    float final_activation = activation_value * valence_factor;
    final_activation = clamp_float(final_activation, 0.0f, 1.0f);
    
    node->activation = final_activation;
    
    // 注意：置信度不应该在这里直接更新
    // 置信度应该通过 learn_from_feedback 从用户反馈中学习
    // 这里只更新基础的 activation 值
    
    sub->total_activations++;
    sub->last_used = time(NULL);
    
    // 更新统计
    if (sub->total_activations > 0) {
        sub->avg_activation_value = 
            (sub->avg_activation_value * (sub->total_activations - 1) + final_activation) 
            / sub->total_activations;
    } else {
        sub->avg_activation_value = final_activation;
    }
    
    master->active_topo_id = topo_id;
    master->active_node_ids[topo_id] = node_id;
    master->activation_levels[topo_id] = final_activation;
    
    return 0;
}

int master_set_node_confidence(MasterTopology* master,
                             int topo_id,
                             int node_id,
                             float confidence) {
    if (!master) return -1;
    if (topo_id < 0 || topo_id >= master->sub_topo_count) return -1;
    
    SubTopology* sub = master->sub_topologies[topo_id];
    if (!sub || !sub->net) return -1;
    if (node_id < 0 || node_id >= sub->net->node_count) return -1;
    
    ReasoningNode* node = sub->net->nodes[node_id];
    if (!node) return -1;
    
    if (confidence < 0.0f) confidence = 0.0f;
    if (confidence > 1.0f) confidence = 1.0f;
    
    node->confidence = confidence;
    
    return 0;
}

int master_set_edge_confidence(MasterTopology* master,
                              int topo_id,
                              int from_node_id,
                              int to_node_id,
                              float confidence) {
    if (!master) return -1;
    if (topo_id < 0 || topo_id >= master->sub_topo_count) return -1;
    
    SubTopology* sub = master->sub_topologies[topo_id];
    if (!sub || !sub->net) return -1;
    if (from_node_id < 0 || from_node_id >= sub->net->node_count) return -1;
    if (to_node_id < 0 || to_node_id >= sub->net->node_count) return -1;
    
    ReasoningNode* from_node = sub->net->nodes[from_node_id];
    if (!from_node) return -1;
    
    if (confidence < 0.0f) confidence = 0.0f;
    if (confidence > 1.0f) confidence = 1.0f;
    
    for (int i = 0; i < from_node->connection_count; i++) {
        if (from_node->connections[i] && 
            from_node->connections[i]->node_id == to_node_id) {
            from_node->connection_confidences[i] = confidence;
            printf("[边置信度] 拓扑=%s, 边 %d(%s) → %d(%s), 置信度=%.2f\n",
                   sub->name,
                   from_node_id, from_node->concept ? from_node->concept : "?",
                   to_node_id, from_node->connections[i]->concept ? from_node->connections[i]->concept : "?",
                   confidence);
            return 0;
        }
    }
    
    return -1;
}

int master_propagate_activation(MasterTopology* master,
                              int source_topo_id,
                              int source_node_id) {
    if (!master) return -1;

    static float weight_boost_factor = 1.05f;
    static int min_weight_count = 5;

    int propagated_count = 0;

    // 获取源节点
    SubTopology* source_topo = master_get_sub_topology(master, source_topo_id);
    ReasoningNode* source_node = NULL;
    float source_valence = 0.0f;
    if (source_topo && source_topo->net && source_node_id < source_topo->net->node_count) {
        source_node = source_topo->net->nodes[source_node_id];
        source_valence = source_node->valence;
    }

    // 使用 O(1) 邻接表索引查找（替代 O(N) 遍历）
    int adj_idx = source_topo_id * 10000 + source_node_id;
    if (adj_idx >= master->cross_adj_count) {
        return 0;  // 没有出边
    }

    CrossTopoAdjEntry* entry = master->cross_adj[adj_idx];
    while (entry) {
        CrossTopologyLink* link = master->cross_links[entry->link_index];
        if (link) {
            float source_activation = master->activation_levels[source_topo_id];
            
            // ==================== 动态权重学习 ====================
            link->use_count++;
            if (link->use_count > min_weight_count) {
                if (link->weight < 0.95f) {
                    link->weight = link->weight * weight_boost_factor;
                    if (link->weight > 0.95f) link->weight = 0.95f;
                }
            }
            
            // ==================== 使用新参数计算激活 ====================
            // 激活 = 输入 × 逻辑权重 × 动机倾向 × (1 + 效价因子)
            // 从节点的 connection_motivational_bias 读取真实的动机倾向
            float motivation_factor = 0.5f;
            if (source_node && source_node->connection_motivational_bias && 
                source_node->connection_count > 0) {
                int conn_idx = link->to_node_id % source_node->connection_count;
                motivation_factor = source_node->connection_motivational_bias[conn_idx];
            }
            
            float valence_factor = 1.0f + source_valence * 0.5f;
            
            float transferred_activation = source_activation *
                                         link->weight *
                                         link->transfer_rate *
                                         motivation_factor *
                                         valence_factor;

            master_activate_node(master,
                               link->to_topo_id,
                               link->to_node_id,
                               transferred_activation);

            propagated_count++;
        }
        entry = entry->next;
    }

    if (propagated_count > 0) {
        printf("[激活传播] 从拓扑%d节点%d传播到%d个节点 (效价:%.2f)\n",
               source_topo_id, source_node_id, propagated_count, source_valence);
    }

    return propagated_count;
}

void master_reset_activations(MasterTopology* master) {
    if (!master) return;
    
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (node) {
                node->activation = 0.0f;
                node->is_visited = 0;
            }
        }
    }
    
    memset(master->activation_levels, 0, sizeof(float) * master->sub_topo_capacity);
    memset(master->active_node_ids, -1, sizeof(int) * master->sub_topo_capacity);
    
    printf("[激活重置] 已清空所有拓扑的激活值\n");
}

void master_decay_activations(MasterTopology* master, float decay_rate) {
    if (!master) return;
    if (decay_rate <= 0.0f) decay_rate = 0.5f;
    if (decay_rate >= 1.0f) decay_rate = 0.9f;
    
    int decayed_count = 0;
    
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (node && node->activation > 0.0f) {
                node->activation *= decay_rate;
                if (node->activation < 0.05f) {
                    node->activation = 0.0f;
                }
                node->is_visited = 0;
                decayed_count++;
            }
        }
    }
}

void master_consolidate_confidence(MasterTopology* master, float boost_factor) {
    if (!master) return;
    if (boost_factor <= 0.0f) boost_factor = 0.1f;
    if (boost_factor >= 0.5f) boost_factor = 0.3f;
    
    int boosted_count = 0;
    
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (node && node->activation > 0.3f) {
                if (node->confidence < 0.95f) {
                    node->confidence += boost_factor;
                    if (node->confidence > 0.95f) node->confidence = 0.95f;
                    boosted_count++;
                }
            }
        }
    }
}

void knowledge_self_verify(MasterTopology* master, int topo_id, int node_id) {
    if (!master) return;
    if (topo_id < 0 || topo_id >= master->sub_topo_count) return;
    
    SubTopology* sub = master->sub_topologies[topo_id];
    if (!sub || !sub->net) return;
    if (node_id < 0 || node_id >= sub->net->node_count) return;
    
    ReasoningNode* node = sub->net->nodes[node_id];
    if (!node) return;
    
    float support_score = 0.0f;
    int connection_count = 0;
    
    for (int i = 0; i < node->connection_count; i++) {
        if (!node->connections[i]) continue;
        
        float edge_conf = node->connection_confidences[i];
        
        if (edge_conf > 0.7f) {
            support_score += 1.0f;
        } else if (edge_conf > 0.3f) {
            support_score += 0.5f;
        } else if (edge_conf > 0.0f) {
            support_score -= 0.3f;
        }
        
        connection_count++;
    }
    
    if (connection_count == 0) return;
    
    float consistency = support_score / connection_count;
    
    float old_conf = node->confidence;
    
    if (consistency > 0.3f) {
        node->confidence += 0.02f;
    } else if (consistency < -0.2f) {
        node->confidence -= 0.05f;
    }
    
    if (node->confidence > 0.95f) node->confidence = 0.95f;
    if (node->confidence < 0.05f) node->confidence = 0.05f;
    
    if (fabs(node->confidence - old_conf) > 0.01f) {
        printf("[自验证] 节点 %s: 置信度 %.3f → %.3f (一致性: %.2f)\n",
               node->concept, old_conf, node->confidence, consistency);
    }
}

void batch_self_verify(MasterTopology* master) {
    if (!master) return;
    
    int verified_count = 0;
    
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (!node || node->connection_count == 0) continue;
            
            float old_conf = node->confidence;
            
            float support_score = 0.0f;
            int connection_count = 0;
            
            for (int i = 0; i < node->connection_count; i++) {
                if (!node->connections[i]) continue;
                
                float edge_conf = node->connection_confidences[i];
                
                if (edge_conf > 0.7f) {
                    support_score += 1.0f;
                } else if (edge_conf > 0.3f) {
                    support_score += 0.5f;
                } else if (edge_conf > 0.0f) {
                    support_score -= 0.3f;
                }
                
                connection_count++;
            }
            
            if (connection_count == 0) continue;
            
            float consistency = support_score / connection_count;
            
            if (consistency > 0.3f) {
                node->confidence += 0.01f;
            } else if (consistency < -0.2f) {
                node->confidence -= 0.02f;
            }
            
            if (node->confidence > 0.95f) node->confidence = 0.95f;
            if (node->confidence < 0.05f) node->confidence = 0.05f;
            
            if (fabs(node->confidence - old_conf) > 0.005f) {
                verified_count++;
            }
        }
    }
}

// ==================== 拓扑级并行激活传播 ====================
//
// 原理：
// - 每个活跃子拓扑作为一个独立任务
// - 提交到线程池，由 x 个 worker 并行窃取执行
// - 未来新增子拓扑（语音/图像等）自动加入线程池调度
// - 线程数 = CPU核数，由 thread_pool 自动检测和管理

/** 拓扑级传播任务 */
typedef struct {
    MasterTopology* master;
    int topo_id;                // 子拓扑ID
    float threshold;            // 激活阈值
    int propagated_count;       // 输出：该拓扑内传播了多少节点
} TopoPropTask;

/** worker 函数：在指定子拓扑内传播激活 */
static void topo_propagate_worker(void* arg) {
    TopoPropTask* task = (TopoPropTask*)arg;
    MasterTopology* master = task->master;
    SubTopology* sub = master_get_sub_topology(master, task->topo_id);
    if (!sub || !sub->net || !sub->is_active) {
        task->propagated_count = 0;
        return;
    }

    int count = 0;
    for (int n = 0; n < sub->net->node_count; n++) {
        ReasoningNode* node = sub->net->nodes[n];
        if (!node || node->activation < task->threshold) continue;

        for (int c = 0; c < node->connection_count; c++) {
            ReasoningNode* target = node->connections[c];
            if (!target) continue;
            float transferred = node->activation * node->connection_weights[c];
            if (transferred > 0.1f) {
                // 线程安全：每个拓扑只在一个线程内处理
                target->activation += transferred;
                if (target->activation > 1.0f) target->activation = 1.0f;
                count++;
            }
        }
    }
    task->propagated_count = count;
}

/** 获取或创建线程池（懒创建，自动检测CPU核数） */
ThreadPool* master_get_thread_pool(MasterTopology* master) {
    if (!master) return NULL;
    if (!master->thread_pool) {
        master->thread_pool = thread_pool_create();
        master->parallel_mode = 1;  // 自动切换到并行模式
    }
    return master->thread_pool;
}

/**
 * 增强版并行激活传播 — 拓扑级并行
 *
 * 1. 扫描所有子拓扑，找出有活跃节点的
 * 2. 每个活跃子拓扑作为一个任务提交到线程池
 * 3. workers + 主线程并发窃取执行
 * 4. 全部完成后返回总传播数
 *
 * @param master    主拓扑
 * @param threshold 激活阈值（activation >= threshold 的节点才传播）
 * @return 总传播节点数
 */
int master_propagate_parallel_topology(MasterTopology* master, float threshold) {
    if (!master) return -1;

    // 1. 统计活跃子拓扑数
    int active_count = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net || !sub->is_active) continue;

        for (int n = 0; n < sub->net->node_count; n++) {
            if (sub->net->nodes[n] && sub->net->nodes[n]->activation >= threshold) {
                active_count++;
                break;
            }
        }
    }

    if (active_count == 0) return 0;

    // 2. 获取线程池
    ThreadPool* pool = master_get_thread_pool(master);
    if (!pool) return -1;

    int nworkers = thread_pool_num_workers(pool);
    printf("[并行传播] %d 个活跃拓扑 → %d 个 worker\n", active_count, nworkers);

    // 3. 构建任务数组（栈分配，上限9个子拓扑）
    TopoPropTask tasks[10];
    ThreadTask th_tasks[10];
    int task_idx = 0;

    for (int t = 0; t < master->sub_topo_count && task_idx < 10; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net || !sub->is_active) continue;

        int has_active = 0;
        for (int n = 0; n < sub->net->node_count; n++) {
            if (sub->net->nodes[n] && sub->net->nodes[n]->activation >= threshold) {
                has_active = 1;
                break;
            }
        }
        if (!has_active) continue;

        tasks[task_idx].master = master;
        tasks[task_idx].topo_id = t;
        tasks[task_idx].threshold = threshold;
        tasks[task_idx].propagated_count = 0;
        th_tasks[task_idx].func = topo_propagate_worker;
        th_tasks[task_idx].arg = &tasks[task_idx];
        task_idx++;
    }

    if (task_idx == 0) return 0;

    // 4. 批量提交 — 主线程 + workers 并行窃取执行
    thread_pool_batch(pool, th_tasks, task_idx);

    // 5. 汇总结果
    int total = 0;
    for (int i = 0; i < task_idx; i++) total += tasks[i].propagated_count;

    printf("[并行传播] 完成，共传播 %d 个节点（%d 个拓扑并行）\n", total, task_idx);
    return total;
}

/**
 * 旧版并行传播（向后兼容）
 * 内部调用新的拓扑级并行实现
 */
int master_propagate_parallel(MasterTopology* master, int max_concurrent) {
    (void)max_concurrent;  // 不再使用固定并发数，由线程池自动管理
    return master_propagate_parallel_topology(master, 0.3f);
}

// ==================== 生成式推理实现 ====================

// 基于多拓扑网络的生成式推理（通过拓扑激活状态生成自然语言回复）
char* master_generate_response(MasterTopology* master,
                              const char* input_text,
                              int max_output_len) {
    if (!master || !input_text || max_output_len <= 0) {
        return strdup("...");
    }
    
    master->total_inferences++;
    
    // ==================== UTF-8分词 ====================
    char* tokens[100];
    int token_count = utf8_tokenize(input_text, tokens, 100);
    
    if (token_count == 0) {
        return strdup("...");
    }
    
    // ==================== 使用联想引擎 ====================
    AssociativeEngine* assoc_engine = assoc_engine_create(master);
    if (assoc_engine) {
        int assoc_count = associate_from_text(assoc_engine, input_text, 3);
        
        if (assoc_count > 0) {
            // 基于联想生成回复
            char* assoc_response = generate_from_associations(assoc_engine, max_output_len, input_text);
            if (assoc_response && strlen(assoc_response) > 0) {
                // 先复制回复内容，再释放引擎（避免assoc_response指向引擎内部内存）
                char* safe_response = strdup(assoc_response);
                assoc_engine_free(assoc_engine);
                free(assoc_response);
                for (int i = 0; i < token_count; i++) {
                    free(tokens[i]);
                }
                return safe_response;
            }
            if (assoc_response) {
                free(assoc_response);
                assoc_response = NULL;
            }
        }
        
        assoc_engine_free(assoc_engine);
    }
    
    // 清理 tokens
    for (int i = 0; i < token_count; i++) {
        free(tokens[i]);
    }
    
    // 默认回复
    char* result = (char*)malloc(max_output_len);
    if (result) {
        snprintf(result, max_output_len, "我理解了，正在学习这个概念。");
    }
    return result;
}

// ==================== 走边贪心路径生成 ====================

/**
 * 混合评分常量
 *
 * 加法部分（五维）：边(逻辑强度+置信度+动机倾向) + 节点(激活值+置信度)
 * 乘法部分（一维）：效价 → 乘法调节因子
 *
 * 最终得分 = base_score × valence_modifier
 * base_score = Σ(加法部分)
 * valence_modifier = 1.0 + VALENCE_COEFF × raw_valence
 *   raw_valence ∈ [-1, 1] →
 *     -1.0 → modifier = 0.4 （强否决）
 *      0.0 → modifier = 1.0 （中性）
 *     +1.0 → modifier = 1.6 （强偏好）
 */
#define EDGE_WALK_W_WEIGHT      0.27f   // 边逻辑强度
#define EDGE_WALK_W_CONF        0.20f   // 边置信度
#define EDGE_WALK_W_BIAS        0.10f   // 边动机倾向
#define EDGE_WALK_W_ACTIVATION  0.23f   // 目标节点激活值
#define EDGE_WALK_W_NODE_CONF   0.10f   // 目标节点置信度
// 效维已改为乘法因子，见 VALENCE_COEFF
#define EDGE_WALK_W_SEMANTIC    0.10f   // 语义得分（余弦相似度）

/** 效价乘法系数（0=无影响，越大效价否决力越强） */
#define EDGE_WALK_VALENCE_COEFF 0.6f

/** 走边最低得分阈值 — 低于此值停止继续走 */
#define EDGE_WALK_MIN_SCORE     0.05f

int topology_walk_greedy(SubTopology* sub, int start_node_id,
                         int* path_out, float* scores_out,
                         int max_len, unsigned char* visited) {
    if (!sub || !sub->net || !path_out || max_len <= 0) return 0;

    HuarongTopologyNet* net = sub->net;
    int node_count = net->node_count;
    if (node_count <= 0) return 0;

    // visited 位图：每8个节点用1字节标记
    int bitmap_size = (node_count + 7) / 8;
    unsigned char* local_visited = NULL;
    if (!visited) {
        local_visited = (unsigned char*)calloc(bitmap_size, 1);
        if (!local_visited) return 0;
        visited = local_visited;
    }

    // 有效性检查：起点必须在范围内
    if (start_node_id < 0 || start_node_id >= node_count ||
        !net->nodes[start_node_id]) {
        if (local_visited) free(local_visited);
        return 0;
    }

    int path_len = 0;
    int current_id = start_node_id;

    // 语义上下文：累积已走过节点的特征向量均值
    float context_features[NODE_FEATURE_DIM] = {0};
    int context_count = 0;

    // 标记起点为已访问并写入路径
    visited[current_id / 8] |= (unsigned char)(1 << (current_id % 8));
    path_out[path_len] = current_id;
    if (scores_out) scores_out[path_len] = 1.0f;
    // 更新上下文
    ReasoningNode* start_node_ptr = net->nodes[current_id];
    if (start_node_ptr && start_node_ptr->features) {
        for (int d = 0; d < NODE_FEATURE_DIM; d++)
            context_features[d] += start_node_ptr->features[d];
        context_count++;
    }
    path_len++;

    // 贪心走边循环
    while (path_len < max_len) {
        ReasoningNode* current = net->nodes[current_id];
        if (!current || current->connection_count <= 0) break;

        int best_next_id = -1;
        float best_score = -1.0f;

        for (int i = 0; i < current->connection_count; i++) {
            ReasoningNode* target = current->connections[i];
            if (!target) continue;
            int tid = target->node_id;
            if (tid < 0 || tid >= node_count) continue;

            // 跳过已访问节点（防循环）
            if (visited[tid / 8] & (unsigned char)(1 << (tid % 8))) continue;

            // --- 边三维 ---
            float edge_weight = current->connection_weights[i];

            float edge_conf = 0.0f;
            if (current->connection_confidences && i < current->connection_count)
                edge_conf = current->connection_confidences[i];
            else
                edge_conf = edge_weight;  // 兜底

            float edge_bias = 0.0f;
            if (current->connection_motivational_bias && i < current->connection_count)
                edge_bias = current->connection_motivational_bias[i];

            // --- 目标节点 ---
            float node_act   = target->activation;
            float node_conf  = target->confidence;
            float raw_val    = target->valence;  // 原始效价 [-1, 1]，保留符号

            // --- 语义得分（第7维） ---
            float semantic_score = 0.0f;
            if (target->features && context_count > 0) {
                float mean_features[NODE_FEATURE_DIM];
                for (int d = 0; d < NODE_FEATURE_DIM; d++)
                    mean_features[d] = context_features[d] / context_count;
                semantic_score = cosine_similarity(target->features, mean_features, NODE_FEATURE_DIM);
            }

            // --- 混合评分 ---
            // 加法部分：边三维 + 节点二维 + 语义一维（不含效价）
            float base_score =
                EDGE_WALK_W_WEIGHT     * edge_weight +
                EDGE_WALK_W_CONF       * edge_conf   +
                EDGE_WALK_W_BIAS       * edge_bias   +
                EDGE_WALK_W_ACTIVATION * node_act    +
                EDGE_WALK_W_NODE_CONF  * node_conf   +
                EDGE_WALK_W_SEMANTIC   * (semantic_score + 1.0f) * 0.5f;  // 语义得分映射到 [0,1]

            // 乘法部分：效价作为调节因子
            float valence_mod = 1.0f + EDGE_WALK_VALENCE_COEFF * raw_val;

            float score = base_score * valence_mod;

            if (score > best_score) {
                best_score = score;
                best_next_id = tid;
            }
        }

        // 无合适的下一步或得分过低
        if (best_next_id < 0 || best_score < EDGE_WALK_MIN_SCORE) break;

        // 走一步
        current_id = best_next_id;
        visited[current_id / 8] |= (unsigned char)(1 << (current_id % 8));
        path_out[path_len] = current_id;
        if (scores_out) scores_out[path_len] = best_score;
        // 更新语义上下文
        ReasoningNode* stepped_node = net->nodes[current_id];
        if (stepped_node && stepped_node->features) {
            for (int d = 0; d < NODE_FEATURE_DIM; d++)
                context_features[d] += stepped_node->features[d];
            context_count++;
        }
        path_len++;
    }

    if (local_visited) free(local_visited);
    return path_len;
}

// ==================== 跨拓扑走边路径生成 ====================

/**
 * 检查一个字符是否在 avoid_chars 中（用于防回声）
 */
static int char_in_set(const char* ch, const char* set) {
    if (!ch || !set) return 0;
    while (*set) {
        if ((unsigned char)ch[0] == (unsigned char)set[0]) {
            int len = 1;
            if ((ch[0] & 0xE0) == 0xC0) len = 2;
            else if ((ch[0] & 0xF0) == 0xE0) len = 3;
            else if ((ch[0] & 0xF8) == 0xF0) len = 4;
            int match = 1;
            for (int b = 0; b < len; b++) {
                if ((unsigned char)ch[b] != (unsigned char)set[b]) { match = 0; break; }
            }
            if (match) return 1;
        }
        set++;
        while ((unsigned char)*set >= 0x80 && (unsigned char)*set < 0xC0) set++;
    }
    return 0;
}

int topology_walk_cross(MasterTopology* master,
                        int start_topo_id, int start_node_id,
                        int* path_topos_out, int* path_nodes_out,
                        float* scores_out,
                        int max_len,
                        unsigned char** visited_bitmaps,
                        const char* avoid_chars,
                        const float* topo_act) {
    if (!master || !path_topos_out || !path_nodes_out || max_len <= 0) return 0;
    if (start_topo_id < 0 || start_topo_id >= master->sub_topo_count) return 0;

    SubTopology* start_sub = master_get_sub_topology(master, start_topo_id);
    if (!start_sub || !start_sub->net) return 0;
    if (start_node_id < 0 || start_node_id >= start_sub->net->node_count) return 0;
    if (!start_sub->net->nodes[start_node_id]) return 0;

    int visited_count = master->sub_topo_count;
    unsigned char** local_bitmaps = NULL;
    if (!visited_bitmaps) {
        local_bitmaps = (unsigned char**)calloc(visited_count, sizeof(unsigned char*));
        if (!local_bitmaps) return 0;
        for (int t = 0; t < visited_count; t++) {
            SubTopology* sub = master_get_sub_topology(master, t);
            if (sub && sub->net && sub->net->node_count > 0) {
                int bm_size = (sub->net->node_count + 7) / 8;
                local_bitmaps[t] = (unsigned char*)calloc(bm_size, 1);
            }
        }
        visited_bitmaps = local_bitmaps;
    }

    int path_len = 0;
    int cur_topo = start_topo_id;
    int cur_node = start_node_id;

    // 语义上下文：跨拓扑累积特征向量均值
    float context_features[NODE_FEATURE_DIM] = {0};
    int context_count = 0;

    if (cur_topo >= 0 && cur_topo < visited_count && visited_bitmaps[cur_topo]) {
        SubTopology* sub = master_get_sub_topology(master, cur_topo);
        if (sub && sub->net && cur_node < sub->net->node_count) {
            visited_bitmaps[cur_topo][cur_node / 8] |= (unsigned char)(1 << (cur_node % 8));
        }
    }
    path_topos_out[path_len] = cur_topo;
    path_nodes_out[path_len] = cur_node;
    if (scores_out) scores_out[path_len] = 1.0f;
    // 更新语义上下文
    {
        ReasoningNode* start_node = NULL;
        SubTopology* start_sub = master_get_sub_topology(master, cur_topo);
        if (start_sub && start_sub->net && cur_node < start_sub->net->node_count)
            start_node = start_sub->net->nodes[cur_node];
        if (start_node && start_node->features) {
            for (int d = 0; d < NODE_FEATURE_DIM; d++)
                context_features[d] += start_node->features[d];
            context_count++;
        }
    }
    path_len++;

    while (path_len < max_len) {
        SubTopology* cur_sub = master_get_sub_topology(master, cur_topo);
        if (!cur_sub || !cur_sub->net) break;
        if (cur_node < 0 || cur_node >= cur_sub->net->node_count) break;
        ReasoningNode* cur_ra = cur_sub->net->nodes[cur_node];
        if (!cur_ra) break;

        int best_topo = -1, best_node = -1;
        float best_score = -1.0f;

        // --- 评估本拓扑内的连接 ---
        for (int i = 0; i < cur_ra->connection_count; i++) {
            ReasoningNode* target = cur_ra->connections[i];
            if (!target) continue;
            int tid = target->node_id;
            if (tid < 0 || tid >= cur_sub->net->node_count) continue;

            int bm_sz = (cur_sub->net->node_count + 7) / 8;
            if (tid >= bm_sz * 8) continue;
            if (visited_bitmaps[cur_topo] &&
                (visited_bitmaps[cur_topo][tid / 8] & (unsigned char)(1 << (tid % 8))))
                continue;

            if (avoid_chars && target->concept && char_in_set(target->concept, avoid_chars))
                continue;

            float edge_weight = (cur_ra->connection_weights && i < cur_ra->connection_count)
                                ? cur_ra->connection_weights[i] : 0.0f;
            float edge_conf = (cur_ra->connection_confidences && i < cur_ra->connection_count)
                              ? cur_ra->connection_confidences[i] : edge_weight;
            float edge_bias = (cur_ra->connection_motivational_bias && i < cur_ra->connection_count)
                              ? cur_ra->connection_motivational_bias[i] : 0.0f;
            float node_act = target->activation;
            float node_conf = target->confidence;
            float raw_val = target->valence;

            // --- 语义得分 ---
            float semantic_score = 0.0f;
            if (target->features && context_count > 0) {
                float mean_features[NODE_FEATURE_DIM];
                for (int d = 0; d < NODE_FEATURE_DIM; d++)
                    mean_features[d] = context_features[d] / context_count;
                semantic_score = cosine_similarity(target->features, mean_features, NODE_FEATURE_DIM);
            }

            float base_score =
                EDGE_WALK_W_WEIGHT     * edge_weight +
                EDGE_WALK_W_CONF       * edge_conf   +
                EDGE_WALK_W_BIAS       * edge_bias   +
                EDGE_WALK_W_ACTIVATION * node_act    +
                EDGE_WALK_W_NODE_CONF  * node_conf   +
                EDGE_WALK_W_SEMANTIC   * (semantic_score + 1.0f) * 0.5f;

            float valence_mod = 1.0f + EDGE_WALK_VALENCE_COEFF * raw_val;
            float score = base_score * valence_mod;

            if (score > best_score) {
                best_score = score;
                best_topo = cur_topo;
                best_node = tid;
            }
        }

        // --- 评估跨拓扑连接 ---
        int adj_idx = cur_topo * 10000 + cur_node;
        if (adj_idx < master->cross_adj_count && master->cross_adj[adj_idx]) {
            CrossTopoAdjEntry* entry = master->cross_adj[adj_idx];
            while (entry) {
                CrossTopologyLink* link = (entry->link_index < master->cross_link_count)
                                          ? master->cross_links[entry->link_index] : NULL;
                if (link) {
                    int to_topo = link->to_topo_id;
                    int to_node = link->to_node_id;

                    SubTopology* tgt_sub = master_get_sub_topology(master, to_topo);
                    if (tgt_sub && tgt_sub->net && to_node < tgt_sub->net->node_count) {
                        if (visited_bitmaps[to_topo]) {
                            int bm_sz = (tgt_sub->net->node_count + 7) / 8;
                            if (to_node < bm_sz * 8 &&
                                (visited_bitmaps[to_topo][to_node / 8] &
                                 (unsigned char)(1 << (to_node % 8)))) {
                                entry = entry->next;
                                continue;
                            }
                        }

                        ReasoningNode* tgt_node = tgt_sub->net->nodes[to_node];
                        if (avoid_chars && tgt_node && tgt_node->concept &&
                            char_in_set(tgt_node->concept, avoid_chars)) {
                            entry = entry->next;
                            continue;
                        }

                        float cross_weight = link->weight * link->transfer_rate;
                        float node_act = tgt_node ? tgt_node->activation : 0.0f;
                        float node_conf = tgt_node ? tgt_node->confidence : 0.0f;
                        float raw_val = tgt_node ? tgt_node->valence : 0.0f;

                        // --- 跨拓扑语义得分 ---
                        float semantic_score = 0.0f;
                        if (tgt_node && tgt_node->features && context_count > 0) {
                            float mean_features[NODE_FEATURE_DIM];
                            for (int d = 0; d < NODE_FEATURE_DIM; d++)
                                mean_features[d] = context_features[d] / context_count;
                            semantic_score = cosine_similarity(tgt_node->features, mean_features, NODE_FEATURE_DIM);
                        }

                        float base_score =
                            EDGE_WALK_W_WEIGHT     * cross_weight +
                            EDGE_WALK_W_CONF       * cross_weight +
                            EDGE_WALK_W_BIAS       * 0.0f +
                            EDGE_WALK_W_ACTIVATION * node_act    +
                            EDGE_WALK_W_NODE_CONF  * node_conf +
                            EDGE_WALK_W_SEMANTIC   * (semantic_score + 1.0f) * 0.5f;

                        float valence_mod = 1.0f + EDGE_WALK_VALENCE_COEFF * raw_val;
                        float score = base_score * valence_mod;

                        if (topo_act && to_topo < master->sub_topo_count) {
                            score += topo_act[to_topo] * 0.1f;
                        }

                        if (score > best_score) {
                            best_score = score;
                            best_topo = to_topo;
                            best_node = to_node;
                        }
                    }
                }
                entry = entry->next;
            }
        }

        if (best_topo < 0 || best_node < 0 || best_score < EDGE_WALK_MIN_SCORE) break;

        cur_topo = best_topo;
        cur_node = best_node;

        if (cur_topo >= 0 && cur_topo < visited_count && visited_bitmaps[cur_topo]) {
            SubTopology* sub = master_get_sub_topology(master, cur_topo);
            if (sub && sub->net && cur_node < sub->net->node_count) {
                visited_bitmaps[cur_topo][cur_node / 8] |= (unsigned char)(1 << (cur_node % 8));
            }
        }

        path_topos_out[path_len] = cur_topo;
        path_nodes_out[path_len] = cur_node;
        if (scores_out) scores_out[path_len] = best_score;
        // 更新语义上下文
        {
            ReasoningNode* stepped_node = NULL;
            SubTopology* step_sub = master_get_sub_topology(master, cur_topo);
            if (step_sub && step_sub->net && cur_node < step_sub->net->node_count)
                stepped_node = step_sub->net->nodes[cur_node];
            if (stepped_node && stepped_node->features) {
                for (int d = 0; d < NODE_FEATURE_DIM; d++)
                    context_features[d] += stepped_node->features[d];
                context_count++;
            }
        }
        path_len++;
    }

    if (local_bitmaps) {
        for (int t = 0; t < visited_count; t++) {
            if (local_bitmaps[t]) free(local_bitmaps[t]);
        }
        free(local_bitmaps);
    }
    return path_len;
}

// ==================== 可视化实现 ====================

void master_visualize_topology(MasterTopology* master, int topo_id) {
    if (!master || topo_id < 0 || topo_id >= master->sub_topo_count) return;
    
    SubTopology* sub = master->sub_topologies[topo_id];
    if (!sub) return;
    
    printf("\n========== %s ==========\n", sub->name);
    printf("类型: %s\n", TOPOLOGY_TYPE_NAMES[sub->type]);
    printf("节点数: %d\n", sub->net->node_count);
    printf("优先级: %d\n", sub->priority);
    printf("权重: %.2f\n", sub->weight);
    printf("总激活次数: %d\n", sub->total_activations);
    printf("平均激活值: %.4f\n", sub->avg_activation_value);
    
    printf("\n节点列表:\n");
    for (int i = 0; i < sub->net->node_count && i < 20; i++) {
        ReasoningNode* node = sub->net->nodes[i];
        if (node) {
            printf("  [%d] %s (激活=%.3f)\n",
                   node->node_id, 
                   node->concept ? node->concept : "?",
                   node->activation);
        }
    }
    
    if (sub->net->node_count > 20) {
        printf("  ... 还有 %d 个节点\n", sub->net->node_count - 20);
    }
    
    printf("==============================\n\n");
}

void master_visualize_cross_links(MasterTopology* master) {
    if (!master) return;
    
    printf("\n===== 跨拓扑连接 =====\n");
    printf("总数: %d\n\n", master->cross_link_count);
    
    for (int i = 0; i < master->cross_link_count && i < 30; i++) {
        CrossTopologyLink* link = master->cross_links[i];
        SubTopology* from = master_get_sub_topology(master, link->from_topo_id);
        SubTopology* to = master_get_sub_topology(master, link->to_topo_id);
        
        printf("[%d] %s(节点%d) --[%s]--> %s(节点%d) 权重=%.2f\n",
               link->link_id,
               from ? from->name : "?", link->from_node_id,
               link->relation,
               to ? to->name : "?", link->to_node_id,
               link->weight);
    }
    
    if (master->cross_link_count > 30) {
        printf("... 还有 %d 个连接\n", master->cross_link_count - 30);
    }
    
    printf("======================\n\n");
}

void master_get_system_status(MasterTopology* master,
                             int* total_nodes,
                             int* total_links,
                             float* avg_activation) {
    if (!master) {
        if (total_nodes) *total_nodes = 0;
        if (total_links) *total_links = 0;
        if (avg_activation) *avg_activation = 0.0f;
        return;
    }
    
    int nodes = 0;
    float total_act = 0.0f;
    
    for (int i = 0; i < master->sub_topo_count; i++) {
        nodes += master->sub_topologies[i]->net->node_count;
        
        for (int j = 0; j < master->sub_topologies[i]->net->node_count; j++) {
            total_act += master->sub_topologies[i]->net->nodes[j]->activation;
        }
    }
    
    if (total_nodes) *total_nodes = nodes;
    if (total_links) *total_links = master->cross_link_count;
    if (avg_activation) *avg_activation = (nodes > 0) ? total_act / nodes : 0.0f;
}

// ==================== 增量训练功能 ====================

int master_add_training_data(MasterTopology* master, const char* input_text, 
                            const char* response_text, float reward) {
    if (!master || !input_text || !response_text) return -1;
    
    printf("[增量训练] 添加训练数据: 输入='%s', 输出='%s', 奖励=%.2f\n",
           input_text, response_text, reward);
    
    char** tokens = NULL;
    int token_count = utf8_tokenize(input_text, tokens, 100);
    if (token_count <= 0) {
        return -1;
    }
    
    SubTopology* vocab_topo = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    if (!vocab_topo) {
        for (int i = 0; i < token_count; i++) free(tokens[i]);
        free(tokens);
        return -1;
    }
    
    for (int i = 0; i < token_count; i++) {
        ReasoningNode* node = node_hash_find(vocab_topo->node_hash, tokens[i]);
        if (!node) {
            node = huarong_net_add_node(vocab_topo->net, tokens[i], NULL, 0);
            if (node) {
                node_hash_add(vocab_topo->node_hash, node);
                auto_connect_new_node(master, vocab_topo, node);
            }
        }
        
        if (node && reward > 0) {
            node->activation = reward;
        }
    }
    
    for (int i = 0; i < token_count; i++) free(tokens[i]);
    free(tokens);
    
    master->training_data_count++;
    
    return 0;
}

int master_batch_train(MasterTopology* master, const char* train_file_path) {
    if (!master || !train_file_path) return -1;
    
    FILE* fp = fopen(train_file_path, "r");
    if (!fp) {
        printf("[增量训练] 无法打开训练文件: %s\n", train_file_path);
        return -1;
    }
    
    char line[2048];
    int added_count = 0;
    
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
        if (len > 1 && line[len-2] == '\r') line[len-2] = '\0';
        
        if (strlen(line) < 3 || line[0] == '#') continue;
        
        char input_text[512] = "";
        char response_text[512] = "";
        float reward = 0.5f;
        
        char* tab_pos = strchr(line, '\t');
        if (tab_pos) {
            size_t input_len = tab_pos - line;
            if (input_len < sizeof(input_text)) {
                strncpy(input_text, line, input_len);
                input_text[input_len] = '\0';
            }
            tab_pos++;
            if (sscanf(tab_pos, "%f", &reward) != 1) {
                strncpy(response_text, tab_pos, sizeof(response_text) - 1);
            }
        } else {
            strncpy(input_text, line, sizeof(input_text) - 1);
        }
        
        if (master_add_training_data(master, input_text, response_text, reward) == 0) {
            added_count++;
        }
    }
    
    fclose(fp);
    printf("[增量训练] 从文件 %s 加载了 %d 条训练数据\n", train_file_path, added_count);
    
    return added_count;
}

// ==================== 状态持久化功能 (v2 format: 含拓扑ID+连接数据) ====================

#define STATE_FORMAT_VERSION 2

int master_save_state(MasterTopology* master, const char* file_path) {
    if (!master || !file_path) return -1;
    
    FILE* fp = fopen(file_path, "wb");
    if (!fp) {
        printf("[状态持久化] 无法创建文件: %s\n", file_path);
        return -1;
    }
    
    // 写文件头: 格式版本
    int fmt_ver = STATE_FORMAT_VERSION;
    fwrite(&fmt_ver, sizeof(int), 1, fp);
    
    int saved_nodes = 0;
    int saved_links = 0;
    
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (!node || !node->concept) continue;
            
            // [v2] 写拓扑类型 + 节点ID
            int topo_type = (int)sub->type;
            fwrite(&topo_type, sizeof(int), 1, fp);
            fwrite(&node->node_id, sizeof(int), 1, fp);
            
            // 概念字符串
            int concept_len = strlen(node->concept) + 1;
            fwrite(&concept_len, sizeof(int), 1, fp);
            fwrite(node->concept, 1, concept_len, fp);
            
            // 激活值
            fwrite(&node->activation, sizeof(float), 1, fp);
            
            // [v2] 写连接数 + 连接数据 (target_node_id, weight, bias, confidence)
            fwrite(&node->connection_count, sizeof(int), 1, fp);
            for (int c = 0; c < node->connection_count; c++) {
                if (node->connections[c]) {
                    int target_id = node->connections[c]->node_id;
                    fwrite(&target_id, sizeof(int), 1, fp);
                } else {
                    int target_id = -1;
                    fwrite(&target_id, sizeof(int), 1, fp);
                }
                fwrite(&node->connection_weights[c], sizeof(float), 1, fp);
                fwrite(&node->connection_motivational_bias[c], sizeof(float), 1, fp);
                fwrite(&node->connection_confidences[c], sizeof(float), 1, fp);
            }
            
            saved_nodes++;
        }
    }
    
    // 跨拓扑连接
    for (int i = 0; i < master->cross_link_count; i++) {
        CrossTopologyLink* link = master->cross_links[i];
        if (!link) continue;
        
        fwrite(&link->from_topo_id, sizeof(int), 1, fp);
        fwrite(&link->from_node_id, sizeof(int), 1, fp);
        fwrite(&link->to_topo_id, sizeof(int), 1, fp);
        fwrite(&link->to_node_id, sizeof(int), 1, fp);
        fwrite(&link->weight, sizeof(float), 1, fp);
        fwrite(&link->use_count, sizeof(int), 1, fp);
        
        saved_links++;
    }
    
    fclose(fp);
    printf("[状态持久化] 已保存到 %s (节点=%d, 链接=%d)\n", 
           file_path, saved_nodes, saved_links);
    
    return saved_nodes;
}

int master_load_state(MasterTopology* master, const char* file_path) {
    if (!master || !file_path) return -1;
    
    FILE* fp = fopen(file_path, "rb");
    if (!fp) {
        printf("[状态持久化] 无法打开文件: %s\n", file_path);
        return -1;
    }
    
    // 读文件头: 格式版本
    int fmt_ver = 1;
    if (fread(&fmt_ver, sizeof(int), 1, fp) != 1) {
        fclose(fp);
        return -1;
    }
    // v1格式: 没有版本头，读到的其实是node_id，回退重读
    if (fmt_ver != STATE_FORMAT_VERSION) {
        // 旧格式: 文件开头就是节点数据，回退
        fseek(fp, 0, SEEK_SET);
        fmt_ver = 1;
    }
    
    int loaded_nodes = 0;
    int loaded_links = 0;
    
    while (1) {
        int topo_type;
        if (fmt_ver >= 2) {
            if (fread(&topo_type, sizeof(int), 1, fp) != 1) break;
        } else {
            topo_type = TOPO_VOCABULARY; // v1: 全部塞进词汇拓扑
        }
        
        int node_id;
        if (fread(&node_id, sizeof(int), 1, fp) != 1) break;
        
        int concept_len;
        if (fread(&concept_len, sizeof(int), 1, fp) != 1) break;
        if (concept_len <= 0 || concept_len > 4096) break;
        
        char concept[4096];
        if (fread(concept, 1, concept_len, fp) != (size_t)concept_len) break;
        concept[concept_len - 1] = '\0';
        
        float activation;
        if (fread(&activation, sizeof(float), 1, fp) != 1) break;
        
        int conn_count;
        if (fread(&conn_count, sizeof(int), 1, fp) != 1) break;
        
        // 找到目标拓扑
        SubTopology* target_topo = NULL;
        for (int t = 0; t < master->sub_topo_count; t++) {
            SubTopology* sub = master->sub_topologies[t];
            if (sub && (int)sub->type == topo_type) {
                target_topo = sub;
                break;
            }
        }
        if (!target_topo) {
            loaded_nodes++;
            if (fmt_ver == 1) continue;
            // v2: 跳过连接数据
            for (int c = 0; c < conn_count && conn_count > 0 && conn_count < 10000; c++) {
                int skip_id; float skip_w, skip_b, skip_c;
                if (fread(&skip_id, sizeof(int), 1, fp) != 1) break;
                if (fread(&skip_w, sizeof(float), 1, fp) != 1) break;
                if (fread(&skip_b, sizeof(float), 1, fp) != 1) break;
                if (fread(&skip_c, sizeof(float), 1, fp) != 1) break;
            }
            continue;
        }
        
        // 添加或查找节点
        ReasoningNode* node = node_hash_find(target_topo->node_hash, concept);
        if (!node) {
            node = huarong_net_add_node(target_topo->net, concept, NULL, 0);
            if (node) {
                node_hash_add(target_topo->node_hash, node);
                auto_connect_new_node(master, target_topo, node);
            }
        }
        if (node) {
            node->activation = activation;
            // 不覆盖 node_id：huarong_net_add_node 已分配正确的数组下标
        }
        
        // [v2] 读取并丢弃连接数据（重建由主动学习器完成）
        if (fmt_ver >= 2) {
            for (int c = 0; c < conn_count && conn_count > 0 && conn_count < 10000; c++) {
                int skip_id; float skip_w, skip_b, skip_c;
                if (fread(&skip_id, sizeof(int), 1, fp) != 1) break;
                if (fread(&skip_w, sizeof(float), 1, fp) != 1) break;
                if (fread(&skip_b, sizeof(float), 1, fp) != 1) break;
                if (fread(&skip_c, sizeof(float), 1, fp) != 1) break;
            }
        }
        
        loaded_nodes++;
    }
    
    // 加载跨拓扑连接（v1和v2通用）
    while (1) {
        int from_topo, from_node, to_topo, to_node;
        float weight;
        int use_count;
        
        if (fread(&from_topo, sizeof(int), 1, fp) != 1) break;
        if (fread(&from_node, sizeof(int), 1, fp) != 1) break;
        if (fread(&to_topo, sizeof(int), 1, fp) != 1) break;
        if (fread(&to_node, sizeof(int), 1, fp) != 1) break;
        if (fread(&weight, sizeof(float), 1, fp) != 1) break;
        if (fread(&use_count, sizeof(int), 1, fp) != 1) break;
        
        // 启发式验证：from_topo应在合法范围内
        if (from_topo < 0 || from_topo > 20 || to_topo < 0 || to_topo > 20) {
            break;
        }
        
        int link_result = master_add_cross_link(master, from_topo, from_node,
                                                       to_topo, to_node, 
                                                       weight, "seed");
        if (link_result == 0) {
            for (int i = 0; i < master->cross_link_count; i++) {
                CrossTopologyLink* link = master->cross_links[i];
                if (link && link->from_topo_id == from_topo && 
                    link->from_node_id == from_node &&
                    link->to_topo_id == to_topo && 
                    link->to_node_id == to_node) {
                    link->use_count = use_count;
                    break;
                }
            }
        }
        
        loaded_links++;
    }
    
    fclose(fp);
    printf("[状态持久化] 已从 %s 加载 (节点=%d, 链接=%d)\n", 
           file_path, loaded_nodes, loaded_links);
    
    return loaded_nodes;
}

// ==================== 统计输出 ====================

void master_print_stats(MasterTopology* master) {
    if (!master) return;
    
    printf("\n========== 拓扑网络统计 ==========\n");
    printf("子拓扑数量: %d\n", master->sub_topo_count);
    printf("跨拓扑连接: %d\n", master->cross_link_count);
    printf("训练数据: %d\n", master->training_data_count);
    printf("总推理次数: %ld\n", master->total_inferences);
    printf("成功推理: %ld\n", master->successful_inferences);
    
    int total_nodes = 0;
    int total_links = 0;
    
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        
        int active_count = 0;
        for (int n = 0; n < sub->net->node_count; n++) {
            if (sub->net->nodes[n] && sub->net->nodes[n]->activation > 0.1f) {
                active_count++;
            }
        }
        
        printf("  %s: %d 节点, %d 活跃\n", 
               sub->name ? sub->name : "?",
               sub->net->node_count, active_count);
        
        total_nodes += sub->net->node_count;
        total_links += sub->net->node_count * 2;
    }
    
    printf("总节点: %d\n", total_nodes);
    printf("估计连接: %d\n", total_links);
    printf("====================================\n\n");
}
