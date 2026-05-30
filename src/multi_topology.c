#include "multi_topology.h"
#include "string_pool.h"
#include "node_hash.h"
#include "associative_reasoning.h"
#include "utf8_tokenizer.h"
#include "cognitive_params.h"
#include "common.h"
#include "thread_pool.h"
#include "path_encoding.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
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
    "上下文拓扑", "领域拓扑", "语用拓扑", "文化拓扑", "概念拓扑", "主拓扑", "模板拓扑"
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

    // 初始化跨拓扑邻接表索引
    master->cross_adj = (CrossTopoAdjEntry**)calloc(
        CROSS_ADJ_INITIAL_SIZE, sizeof(CrossTopoAdjEntry*));
    master->cross_adj_count = CROSS_ADJ_INITIAL_SIZE;
    if (!master->cross_adj) {
        // 不能调用 master_topology_destroy：pthread_rwlock_init 还没调，锁未初始化
        if (master->string_pool) string_pool_destroy(master->string_pool);
        free(master->sub_topologies);
        free(master->cross_links);
        free(master->active_node_ids);
        free(master->activation_levels);
        free(master);
        return NULL;
    }

    // 初始化动态跨拓扑建边跟踪表（全零 = 所有条目空闲）
    memset(master->cross_hit_records, 0, sizeof(master->cross_hit_records));
    master->cross_hit_round = 0;

    // 路径编码递归抽象：创建三元组频率表
    master->freq_table = path_freq_table_create(PATH_TRIPLET_TABLE_SIZE);
    master->use_template_voting = 0;
    master->template_decay_round = 0;

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

    // 销毁路径频率表
    if (master->freq_table) {
        path_freq_table_destroy(master->freq_table);
        master->freq_table = NULL;
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

        /* 同步扩容 active_node_ids 和 activation_levels */
        int* new_node_ids = (int*)realloc(master->active_node_ids,
                                          (size_t)new_capacity * sizeof(int));
        float* new_act_lev = (float*)realloc(master->activation_levels,
                                             (size_t)new_capacity * sizeof(float));
        if (new_node_ids) master->active_node_ids = new_node_ids;
        if (new_act_lev)  master->activation_levels = new_act_lev;
    }
    
    // 创建子拓扑
    SubTopology* sub = (SubTopology*)malloc(sizeof(SubTopology));
    if (!sub) return -1;
    
    sub->topo_id = master->sub_topo_count;
    sub->type = type;

    /* 架构不变量：topo_id == sub->type 依赖按枚举顺序创建拓扑。
       所有调用方必须在添加拓扑时保持 TopologyType 枚举顺序(0,1,2,...,10)。
       若此处触发警告，说明调用顺序与枚举不匹配，跨拓扑邻接表索引将失效。 */
    if (sub->topo_id != (int)sub->type) {
        fprintf(stderr, "WARNING: topology creation order mismatch: "
                "topo_id=%d type=%d (expected %d). Fix add order!\n",
                sub->topo_id, (int)sub->type, sub->topo_id);
    }

    sub->name = string_pool_intern(master->string_pool, name);
    sub->description = string_pool_intern(master->string_pool, 
                                          TOPOLOGY_TYPE_NAMES[type]);
    
    // 创建底层拓扑网络
    sub->net = huarong_net_create(
        (initial_capacity > 0) ? initial_capacity : 1000);
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
    sub->recent_activation = 0.0f;
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
    int adj_idx = from_topo_id * MAX_NODES_PER_TOPO + from_node_id;
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

/**
 * 快速查重：使用邻接表索引判断跨拓扑连接是否已存在
 * O(出度) 而非 O(N)
 */
int cross_link_exists(MasterTopology* master,
                      int from_topo, int from_node,
                      int to_topo, int to_node) {
    if (!master || from_topo < 0 || to_topo < 0) return 0;
    int idx = from_topo * MAX_NODES_PER_TOPO + from_node;
    if (idx >= master->cross_adj_count) return 0;
    CrossTopoAdjEntry* entry = master->cross_adj[idx];
    while (entry) {
        if (entry->link_index < master->cross_link_count) {
            CrossTopologyLink* l = master->cross_links[entry->link_index];
            if (l && l->from_topo_id == from_topo &&
                l->from_node_id == from_node &&
                l->to_topo_id == to_topo &&
                l->to_node_id == to_node) {
                return 1;
            }
        }
        entry = entry->next;
    }
    return 0;
}

void master_clear_cross_links(MasterTopology* master) {
    if (!master) return;

    // 释放所有 CrossTopologyLink 对象
    for (int i = 0; i < master->cross_link_count; i++) {
        if (master->cross_links[i]) {
            free(master->cross_links[i]);
            master->cross_links[i] = NULL;
        }
    }

    // 释放所有邻接表链表节点
    for (int i = 0; i < master->cross_adj_count; i++) {
        CrossTopoAdjEntry* entry = master->cross_adj[i];
        while (entry) {
            CrossTopoAdjEntry* next = entry->next;
            free(entry);
            entry = next;
        }
        master->cross_adj[i] = NULL;
    }

    master->cross_link_count = 0;
}

int master_prune_cross_links(MasterTopology* master, float min_weight, int min_use_count) {
    if (!master || !master->cross_links) return 0;
    int pruned = 0;

    for (int i = 0; i < master->cross_link_count; i++) {
        CrossTopologyLink* link = master->cross_links[i];
        if (!link) continue;
        if (link->weight < min_weight && link->use_count < min_use_count) {
            free(link);
            master->cross_links[i] = NULL;
            pruned++;
        }
    }

    if (pruned > 0)
        printf("[跨拓扑剪枝] 移除 %d 条低质量跨拓扑连接 (min_weight=%.3f, min_use=%d)\n",
               pruned, min_weight, min_use_count);
    return pruned;
}

// ==================== 动态跨拓扑建边跟踪 ====================

/** 哈希函数：四元组映射到 [0, CROSS_HIT_TABLE_SIZE) */
static inline unsigned int cross_hit_hash(int ft, int fn, int tt, int tn) {
    unsigned int h = (unsigned int)(ft * 73856093) ^
                     (unsigned int)(fn * 19349669) ^
                     (unsigned int)(tt * 83492791) ^
                     (unsigned int)(tn);
    return h & (CROSS_HIT_TABLE_SIZE - 1);  // 2的幂取模
}

void master_record_cross_hit(MasterTopology* master,
                             int from_topo, int from_node,
                             int to_topo, int to_node) {
    if (!master) return;
    unsigned int idx = cross_hit_hash(from_topo, from_node, to_topo, to_node);
    unsigned int start = idx;

    do {
        CrossTopoHitRecord* rec = &master->cross_hit_records[idx];
        if (!rec->is_used) {
            // 新条目
            rec->from_topo = from_topo;
            rec->from_node = from_node;
            rec->to_topo = to_topo;
            rec->to_node = to_node;
            rec->hit_count = 1;
            rec->last_round = master->cross_hit_round;
            rec->is_used = 1;
            return;
        }
        if (rec->from_topo == from_topo && rec->from_node == from_node &&
            rec->to_topo == to_topo && rec->to_node == to_node) {
            // 命中已有记录
            rec->hit_count++;
            rec->last_round = master->cross_hit_round;
            return;
        }
        // 线性探测
        idx = (idx + 1) & (CROSS_HIT_TABLE_SIZE - 1);
    } while (idx != start);
    // 表满 — 静默丢弃（当前表足够大，一般不会满）
}

int master_process_cross_hits(MasterTopology* master, int threshold, int round_timeout) {
    if (!master || threshold <= 0) return 0;

    int created = 0;
    int current_round = master->cross_hit_round;

    for (int i = 0; i < CROSS_HIT_TABLE_SIZE; i++) {
        CrossTopoHitRecord* rec = &master->cross_hit_records[i];
        if (!rec->is_used) continue;

        // 过期条目 — 重置
        if (current_round - rec->last_round > round_timeout) {
            rec->is_used = 0;
            continue;
        }

        // 达到建边阈值
        if (rec->hit_count >= threshold) {
            // 先检查边是否已存在
            if (!cross_link_exists(master, rec->from_topo, rec->from_node,
                                   rec->to_topo, rec->to_node)) {
                // 创建跨拓扑边（默认权重0.5，关系"联想"）
                int ret = master_add_cross_link(master,
                    rec->from_topo, rec->from_node,
                    rec->to_topo, rec->to_node,
                    0.5f, "联想");
                if (ret >= 0) created++;
            }
            // 建边后重置记录
            rec->is_used = 0;
        }
    }

    return created;
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
    sub->recent_activation += 0.2f;
    if (sub->recent_activation > 1.0f) sub->recent_activation = 1.0f;
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
    int adj_idx = source_topo_id * MAX_NODES_PER_TOPO + source_node_id;
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

    // 3. 构建任务数组（动态分配，匹配实际活跃拓扑数）
    TopoPropTask* tasks = (TopoPropTask*)calloc((size_t)active_count, sizeof(TopoPropTask));
    ThreadTask* th_tasks = (ThreadTask*)calloc((size_t)active_count, sizeof(ThreadTask));
    if (!tasks || !th_tasks) { free(tasks); free(th_tasks); return -1; }
    int task_idx = 0;

    for (int t = 0; t < master->sub_topo_count; t++) {
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

    free(tasks);
    free(th_tasks);

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
    
    // ==================== 语义提取 + 走边贪心生成 ====================
    // 联想引擎回归知识检索定位，不再用于回复生成
    // 生成改用：输入分词 → 拓扑走边贪心
    {
        // 找到词汇拓扑
        SubTopology* vocab_sub = NULL;
        for (int t = 0; t < master->sub_topo_count; t++) {
            SubTopology* st = master->sub_topologies[t];
            if (st && st->type == TOPO_VOCABULARY) { vocab_sub = st; break; }
        }
        
        if (vocab_sub && vocab_sub->net && vocab_sub->node_hash && token_count > 0) {
            // 为每个输入token找拓扑节点
            int start_nodes[100];
            int start_count = 0;
            for (int i = 0; i < token_count && start_count < 100; i++) {
                ReasoningNode* node = node_hash_find(vocab_sub->node_hash, tokens[i]);
                if (node) {
                    // 重复杂输入字只取一次
                    int dup = 0;
                    for (int j = 0; j < start_count; j++) {
                        if (start_nodes[j] == node->node_id) { dup = 1; break; }
                    }
                    if (!dup) start_nodes[start_count++] = node->node_id;
                }
            }
            
            // 分配结果缓冲区
            char* response = (char*)calloc(max_output_len, 1);
            int pos = 0;
            
            // 全局 visited 位图（跨起点共享）
            int node_count = vocab_sub->net->node_count;
            int bitmap_size = (node_count + 7) / 8;
            unsigned char* global_visited = (unsigned char*)calloc(bitmap_size, 1);
            
            // 从每个起点走边贪心
            for (int si = 0; si < start_count && pos < max_output_len - 10; si++) {
                int start_id = start_nodes[si];
                if (start_id < 0 || start_id >= node_count) continue;
                
                // 标记起点已访问
                global_visited[start_id / 8] |= (unsigned char)(1 << (start_id % 8));
                
                // 走边贪心，从起点出发最多走20步
                int path_nodes[32];
                float path_scores[32];
                int path_len = topology_walk_greedy(
                    vocab_sub, start_id,
                    path_nodes, path_scores,
                    20, global_visited, 1.0f, master, NULL);
                
                // 从路径[1]开始（路径[0]是起点本身），拼接输出
                for (int p = 1; p < path_len && pos < max_output_len - 10; p++) {
                    int nid = path_nodes[p];
                    if (nid < 0 || nid >= node_count) continue;
                    ReasoningNode* node = vocab_sub->net->nodes[nid];
                    if (!node || !node->concept) continue;
                    pos += snprintf(response + pos, max_output_len - pos, "%s", node->concept);
                }
            }
            
            free(global_visited);
            
            // 走边有结果就返回
            if (pos > 0) {
                char* result = strdup(response);
                free(response);
                for (int i = 0; i < token_count; i++) free(tokens[i]);
                return result;
            }
            free(response);
        }
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
#define EDGE_WALK_W_WEIGHT      0.22f   // 边逻辑强度（原0.27→0.22：削弱rich-get-richer）
#define EDGE_WALK_W_CONF        0.15f   // 边置信度（原0.20→0.15）
#define EDGE_WALK_W_BIAS        0.05f   // 边动机倾向（原0.10→0.05）
#define EDGE_WALK_W_ACTIVATION  0.18f   // 目标节点激活值（原0.23→0.18）
#define EDGE_WALK_W_NODE_CONF   0.10f   // 目标节点置信度
// 效维已改为乘法因子，见 VALENCE_COEFF
#define EDGE_WALK_W_SEMANTIC    0.20f   // 语义得分（原0.10→0.20：更强的主题约束）

/** 效价乘法系数 — 扩大否决范围，极端不匹配时产生3x差距 */
#define EDGE_WALK_VALENCE_COEFF 0.85f   // 原0.6→0.85：modifier范围[0.15, 1.85]

/** 路径回溯权重 — 候选节点与已走路径中其他节点的连接强度 */
#define EDGE_WALK_W_PATH_CTX    0.10f   // 路径回溯得分（原0.15→0.10：语义权重已提升，降低冗余）

/** 走边最低得分阈值 — 低于此值停止继续走 */
#define EDGE_WALK_MIN_SCORE     0.05f

int topology_walk_greedy(SubTopology* sub, int start_node_id,
                         int* path_out, float* scores_out,
                         int max_len, unsigned char* visited,
                         float intent_weight,
                         MasterTopology* master,
                         const float* query_anchor) {
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

    // 情感基调：路径的情感色彩走向，EMA 累积（效价约束）
    float context_valence = start_node_ptr ? start_node_ptr->valence : 0.0f;

    // 语义上下文均值（增量维护，避免每步重算）
    float mean_features[NODE_FEATURE_DIM] = {0};
    int has_mean = 0;

    // 贪心走边循环
    while (path_len < max_len) {
        ReasoningNode* current = net->nodes[current_id];
        if (!current || current->connection_count <= 0) break;

        int best_next_id = -1;
        float best_score = -1.0f;

        // 动态剪枝阈值：平滑增长，避免过早扼杀长路径
        float prune_threshold;
        if (path_len <= 3) {
            prune_threshold = PM_WALK_PRUNE_FLOOR;              // 低保期 0.03
        } else {
            // 线性增长斜率减半：第4步 0.055, 第10步 0.135, 第20步 0.285
            prune_threshold = 0.03f + (path_len - 3) * 0.015f;
            if (prune_threshold > PM_WALK_PRUNE_CEIL) prune_threshold = PM_WALK_PRUNE_CEIL;
        }

        // 预计算路径回溯权重缓存（每步一次，替代每候选 O(path_len×avg_degree)）
        float* path_target_weights = (float*)calloc(node_count, sizeof(float));
        if (path_target_weights) {
            for (int pi = 0; pi < path_len; pi++) {
                int pid = path_out[pi];
                if (pid < 0 || pid >= node_count || pid == current_id) continue;
                ReasoningNode* pn = net->nodes[pid];
                if (!pn) continue;
                for (int ej = 0; ej < pn->connection_count; ej++) {
                    ReasoningNode* pt = pn->connections[ej];
                    if (!pt) continue;
                    int pt_id = pt->node_id;
                    if (pt_id < 0 || pt_id >= node_count) continue;
                    float w = pn->connection_weights[ej];
                    if (w > path_target_weights[pt_id])
                        path_target_weights[pt_id] = w;
                }
            }
        }

        // (mean_features 增量维护，见步进更新)

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
            // 回路3b: 混入cognitive_confidence三维综合置信度
            if (target->cognitive_confidence) {
                cognitive_confidence_compute(target->cognitive_confidence);
                node_conf = node_conf * 0.6f + target->cognitive_confidence->combined * 0.4f;
            }
            float raw_val    = target->valence;  // 原始效价 [-1, 1]，保留符号

            // --- 语义得分（第7维）---
            float semantic_score = 0.0f;
            if (target->features && has_mean) {
                semantic_score = cosine_similarity(target->features, mean_features, NODE_FEATURE_DIM);
            }

            // --- 路径回溯（第8维）：已走路径的联合投票（O(1) 缓存版）---
            float path_context_score = (target->node_id >= 0 && target->node_id < node_count && path_target_weights)
                                        ? path_target_weights[target->node_id] : 0.0f;
            int path_ctx_count = path_len;

            // 语义 + 模板 + 概念 跨拓扑联合投票（单次 cross_adj 遍历）
            float semantic_cross_score = 0.0f;
            float template_cross_score = 0.0f;
            float concept_cross_score  = 0.0f;
            int   semantic_hit_count  = 0;
            int   template_hit_count  = 0;
            int   concept_hit_count   = 0;
            if (master && master->cross_adj && master->cross_adj_count > 0) {
                int check_semantic = 1;  /* 语义投票始终开启 */
                int check_template = master->use_template_voting;
                int check_concept  = master->use_template_voting;  /* 概念随模板启用 */
                for (int pi = 0; pi < path_len && (check_semantic || check_template || check_concept); pi++) {
                    int pid = path_out[pi];
                    if (pid < 0 || pid >= node_count || pid == current_id) continue;
                    int adj_idx = sub->topo_id * MAX_NODES_PER_TOPO + pid;
                    if (adj_idx < master->cross_adj_count) {
                        CrossTopoAdjEntry* entry = master->cross_adj[adj_idx];
                        while (entry) {
                            if (entry->link_index < master->cross_link_count) {
                                CrossTopologyLink* link = master->cross_links[entry->link_index];
                                if (link->to_topo_id == TOPO_SEMANTIC && check_semantic) {
                                    semantic_cross_score += link->weight;
                                    semantic_hit_count++;
                                } else if (link->to_topo_id == TOPO_TEMPLATE && check_template) {
                                    template_cross_score += link->weight;
                                    template_hit_count++;
                                } else if (link->to_topo_id == TOPO_CONCEPT && check_concept) {
                                    concept_cross_score += link->weight;
                                    concept_hit_count++;
                                }
                            }
                            entry = entry->next;
                        }
                    }
                }
            }

            // 归一化
            float path_ctx_norm = (path_ctx_count > 0)
                ? path_context_score / sqrtf((float)path_ctx_count) : 0.0f;
            float semantic_cross_norm = (semantic_hit_count > 0)
                ? semantic_cross_score / sqrtf((float)semantic_hit_count) : 0.0f;
            float template_cross_norm = (template_hit_count > 0)
                ? template_cross_score / sqrtf((float)template_hit_count) : 0.0f;
            float concept_cross_norm = (concept_hit_count > 0)
                ? concept_cross_score / sqrtf((float)concept_hit_count) : 0.0f;

            // --- 混合评分 ---
            float base_score =
                EDGE_WALK_W_WEIGHT     * edge_weight +
                EDGE_WALK_W_CONF       * edge_conf   +
                EDGE_WALK_W_BIAS       * edge_bias   +
                EDGE_WALK_W_ACTIVATION * node_act    +
                EDGE_WALK_W_NODE_CONF  * node_conf   +
                (EDGE_WALK_W_SEMANTIC + (context_count > 5 ? 0.10f : 0.0f)) *
                (semantic_score + 1.0f) * 0.5f +
                // 路径回溯：词汇 0.6 + 语义 0.4 + 模板 0.4 + 概念 0.3
                EDGE_WALK_W_PATH_CTX * path_ctx_norm * 0.6f +
                EDGE_WALK_W_PATH_CTX * semantic_cross_norm * 0.4f +
                EDGE_WALK_W_PATH_CTX * template_cross_norm * 0.4f +
                EDGE_WALK_W_PATH_CTX * concept_cross_norm * 0.3f;

            // 乘法部分：效价作为调节因子 × 意图权重（神经调质式）
            // 效价匹配：候选节点与路径情感基调的一致性
            float valence_match = 1.0f - fabsf(context_valence - raw_val) * 0.5f;
            float valence_mod = 0.5f + 0.5f * valence_match;  // [0.5, 1.0]

            // 神经调质式意图调节：
            // connection_motivational_bias 是边的"受体敏感度"——
            // 高偏置边（动机学习时被强化）对意图变化更敏感
            // intent_weight 越大 → 高偏置边获得更大增益
            float intent_mod = 0.5f + 0.5f * intent_weight * (0.3f + 0.7f * edge_bias);

            float score = base_score * valence_mod * intent_mod;

            // --- 输入锚点偏离惩罚（防主题漂移） ---
            // 候选节点特征向量与原始输入特征的余弦相似度
            // 偏离越大 → 惩罚越重 → 保持输出围绕输入主题
            if (query_anchor && target->features && target->feature_dim == NODE_FEATURE_DIM) {
                float anchor_align = cosine_similarity(
                    target->features, query_anchor, NODE_FEATURE_DIM);
                // [-1,1] → [0.7, 1.0]：正相关保留，负相关惩罚
                // anchor_align >= 0 → penalty 0~15%，anchor_align < 0 → penalty 15~30%
                float anchor_penalty = 1.0f - (1.0f - fmaxf(anchor_align, -1.0f)) * 0.15f;
                if (anchor_align < 0.0f) {
                    // 负相关额外惩罚：每 -0.1 多扣 3%
                    anchor_penalty -= fabsf(anchor_align) * 0.3f;
                    if (anchor_penalty < 0.5f) anchor_penalty = 0.5f;
                }
                score *= anchor_penalty;
            }

            // 热度衰减：被选次数越多，得分越低（鼓励路径多样性）
            // 软下限5%确保即使热门节点也有基础分
            float heat_mod = 0.05f + 0.95f * target->heat;
            score *= heat_mod;

            // 三元组链式奖励：如果 prev->current->target 构成两步链，额外加分
            if (path_len > 1 && edge_weight > 0.2f) {
                int prev_id = path_out[path_len - 1];
                if (prev_id >= 0 && prev_id < node_count) {
                    ReasoningNode* prev = net->nodes[prev_id];
                    if (prev) {
                        for (int ej = 0; ej < prev->connection_count; ej++) {
                            if (prev->connections[ej] == current) {
                                if (prev->connection_weights[ej] > 0.2f)
                                    score += 0.05f;  // 三元组链式奖励
                                break;
                            }
                        }
                    }
                }
            }

            if (score > best_score) {
                best_score = score;
                best_next_id = tid;
            }
        }

        // 无合适的下一步或得分过低（使用动态剪枝阈值）
        if (best_next_id < 0 || best_score < prune_threshold) break;

        // 走一步
        current_id = best_next_id;

        /* 路径编码: 实时记录三元组 (prev, from, to) 到频率表 */
        if (master && master->freq_table && path_len >= 2) {
            path_freq_table_record(master->freq_table, sub->topo_id,
                                   path_out[path_len - 2],
                                   path_out[path_len - 1],
                                   best_next_id);
        }

        visited[current_id / 8] |= (unsigned char)(1 << (current_id % 8));
        path_out[path_len] = current_id;
        if (scores_out) scores_out[path_len] = best_score;

        // 更新被选节点的热度（热度衰减在线更新）
        ReasoningNode* stepped_node = net->nodes[current_id];
        if (stepped_node) {
            stepped_node->selection_count++;
            float decay;
            switch (stepped_node->node_type) {
                case NODE_TYPE_FUNCTION_WORD: decay = 0.999f; break;
                case NODE_TYPE_PROPER_NOUN:  decay = 0.990f; break;
                default:                     decay = 0.995f; break;
            }
            // 增量更新：heat *= decay（比 pow() 快10倍）
            stepped_node->heat *= decay;
            // 软下限保护
            if (stepped_node->heat < 0.05f) stepped_node->heat = 0.05f;
        }

        // 更新语义上下文 + 增量维护均值
        if (stepped_node && stepped_node->features) {
            if (context_count == 0) {
                for (int d = 0; d < NODE_FEATURE_DIM; d++)
                    mean_features[d] = stepped_node->features[d];
                context_count = 1;
            } else {
                float inv_new = 1.0f / (float)(context_count + 1);
                for (int d = 0; d < NODE_FEATURE_DIM; d++) {
                    context_features[d] += stepped_node->features[d];
                    mean_features[d] = (mean_features[d] * context_count + stepped_node->features[d]) * inv_new;
                }
                context_count++;
            }
            has_mean = 1;
        }
        // 更新情感基调（EMA，α=0.3）
        if (stepped_node)
            context_valence = context_valence * 0.7f + stepped_node->valence * 0.3f;
        path_len++;
        free(path_target_weights);
    }

    if (local_visited) free(local_visited);
    return path_len;
}

// ==================== 竞争队列生成（替代贪心走边） ====================

/**
 * 竞争队列路径生成 — 基于全局工作空间理论
 *
 * 与贪心走边的本质区别：
 * - 走边：每一步只看当前节点的出边邻居（局部最优）→ 易漂移
 * - 竞争队列：每一步看全图激活场中最活跃的节点（全局视野）→ 可任意跳跃
 *
 * 算法流程（类比大脑的竞争队列 + 全局工作空间）：
 * 1. 输入种子激活 → 全图扩散一轮
 * 2. 收集所有活跃节点作为候选（不限邻居）
 * 3. 竞争评分：激活值 + 意图匹配 + 锚点对齐 + 效价一致 - 热度惩罚
 * 4. 胜者读出 → 写入输出路径
 * 5. 胜者广播（激活其邻居）→ 胜者抑制（防重复）
 * 6. 全局衰减 → 下轮
 *
 * @param sub          工作子拓扑（通常是词汇或语义拓扑）
 * @param master       主拓扑（用于跨拓扑投票）
 * @param intent_weight 当前意图对此拓扑的权重
 * @param query_anchor  输入锚点特征向量（NULL=无锚定）
 * @param max_len       最大路径长度
 * @param path_out      输出路径节点ID
 * @param scores_out    每步评分
 * @return              路径长度
 */
#define CQ_MAX_CANDIDATES  20   // 每轮最多候选数
#define CQ_MAX_ROUNDS      20   // 最大竞争轮数
#define CQ_BROADCAST_GAIN  0.20f // 胜者广播增益（原0.15，增强）
#define CQ_SUPPRESS_FACTOR 0.15f // 胜者抑制因子（原0.10，放宽）

int competitive_queue_generate(
    SubTopology* sub,
    MasterTopology* master,
    float intent_weight,
    const float* query_anchor,
    int max_len,
    int* path_out,
    float* scores_out)
{
    if (!sub || !sub->net || !path_out || max_len <= 0) return 0;
    HuarongTopologyNet* net = sub->net;
    int node_count = net->node_count;
    if (node_count <= 0) return 0;

    // 保存原始激活值（函数退出时恢复）
    float* saved_activations = (float*)malloc(node_count * sizeof(float));
    if (!saved_activations) return 0;
    for (int i = 0; i < node_count; i++) {
        saved_activations[i] = net->nodes[i] ? net->nodes[i]->activation : 0.0f;
    }

    // visited 位图
    int bitmap_size = (node_count + 7) / 8;
    unsigned char* visited = (unsigned char*)calloc(bitmap_size, 1);
    if (!visited) { free(saved_activations); return 0; }

    int path_len = 0;
    float context_valence = 0.0f;

    // 候选节点缓存（复用避免每轮 malloc）
    typedef struct { int node_id; float score; } CQCandidate;
    CQCandidate* candidates = (CQCandidate*)malloc(
        node_count * sizeof(CQCandidate));
    if (!candidates) { free(visited); free(saved_activations); return 0; }

    for (int round = 0; round < CQ_MAX_ROUNDS && path_len < max_len; round++) {
        // ---- 1. 全图激活扩散 ----
        // 使用主拓扑的并行传播引擎
        if (master && round > 0) {
            master_propagate_parallel_topology(master, 0.1f);
        }

        // ---- 2. 收集候选：遍历全图找高激活节点 ----
        int cand_count = 0;
        for (int i = 0; i < node_count && cand_count < node_count; i++) {
            ReasoningNode* node = net->nodes[i];
            if (!node) continue;
            if (node->activation < 0.1f) continue;
            // 跳过已选节点
            if (visited[i / 8] & (unsigned char)(1 << (i % 8))) continue;

            // ---- 3. 竞争评分 ----
            float score = node->activation;  // 基础分 = 激活值

            // 意图调制：利用节点的 motivational_bias 均值作为受体敏感度
            float avg_bias = 0.0f;
            if (node->connection_count > 0) {
                for (int c = 0; c < node->connection_count && c < 10; c++) {
                    if (node->connection_motivational_bias)
                        avg_bias += node->connection_motivational_bias[c];
                }
                avg_bias /= (float)(node->connection_count < 10 ? node->connection_count : 10);
            }
            // 神经调质：高偏置节点对意图更敏感
            score *= (0.5f + 0.5f * intent_weight * (0.3f + 0.7f * avg_bias));

            // 锚点对齐：候选与输入语义一致性的奖励
            if (query_anchor && node->features && node->feature_dim == NODE_FEATURE_DIM) {
                float anchor_align = cosine_similarity(
                    node->features, query_anchor, NODE_FEATURE_DIM);
                // 正相关加分，负相关减分
                score *= (0.7f + 0.3f * fmaxf(anchor_align, -1.0f));
            }

            // 效价一致性
            float valence_match = 1.0f - fabsf(context_valence - node->valence) * 0.5f;
            score *= (0.5f + 0.5f * valence_match);

            // 热度衰减
            score *= (0.05f + 0.95f * node->heat);

            // 置信度加权
            score *= (0.3f + 0.7f * node->confidence);

            candidates[cand_count].node_id = i;
            candidates[cand_count].score = score;
            cand_count++;
        }

        if (cand_count == 0) break;

        // ---- 4. 选胜者（最高分） ----
        int best_idx = 0;
        float best_score = candidates[0].score;
        for (int c = 1; c < cand_count; c++) {
            if (candidates[c].score > best_score) {
                best_score = candidates[c].score;
                best_idx = c;
            }
        }

        if (best_score < 0.05f) break;

        int winner_id = candidates[best_idx].node_id;
        ReasoningNode* winner = net->nodes[winner_id];
        if (!winner) continue;

        // ---- 5. 胜者读出 ----
        visited[winner_id / 8] |= (unsigned char)(1 << (winner_id % 8));
        path_out[path_len] = winner_id;
        if (scores_out) scores_out[path_len] = best_score;

        // ---- 6. 胜者广播：激活其邻居 ----
        for (int c = 0; c < winner->connection_count; c++) {
            ReasoningNode* neighbor = winner->connections[c];
            if (!neighbor) continue;
            int nid = neighbor->node_id;
            if (nid < 0 || nid >= node_count) continue;
            if (visited[nid / 8] & (unsigned char)(1 << (nid % 8))) continue;
            float boost = winner->connection_weights[c] * CQ_BROADCAST_GAIN * winner->activation;
            if (neighbor->activation < boost) neighbor->activation = boost;
        }

        // ---- 7. 胜者抑制：压低自身避免重复 ----
        winner->activation *= CQ_SUPPRESS_FACTOR;

        // ---- 8. 全局衰减 ----
        // 原 0.7f 衰减太快导致一轮耗尽候选 → 改为 0.85f，保留更多激活信号
        for (int i = 0; i < node_count; i++) {
            if (net->nodes[i]) {
                net->nodes[i]->activation *= 0.85f;
                if (net->nodes[i]->activation < 0.01f) net->nodes[i]->activation = 0.0f;
            }
        }

        // 更新热度
        winner->selection_count++;
        winner->heat *= 0.995f;
        if (winner->heat < 0.05f) winner->heat = 0.05f;

        // 更新情感基调
        context_valence = context_valence * 0.7f + winner->valence * 0.3f;
        path_len++;
    }

    // ---- 恢复原始激活值 ----
    for (int i = 0; i < node_count; i++) {
        if (net->nodes[i]) net->nodes[i]->activation = saved_activations[i];
    }

    free(candidates);
    free(visited);
    free(saved_activations);
    return path_len;
}

// ==================== Beam Search 走边路径生成 (K=3) ====================

#define BEAM_K 3

int topology_walk_beam(SubTopology* sub, int start_node_id,
                       int* path_out, float* scores_out,
                       int max_len, unsigned char* visited,
                       float intent_weight,
                       MasterTopology* master,
                       const float* query_anchor) {
    (void)master; (void)query_anchor;  /* 预留: 跨拓扑 beam search */
    if (!sub || !sub->net || !path_out || max_len <= 0) return 0;

    HuarongTopologyNet* net = sub->net;
    int node_count = net->node_count;
    if (node_count <= 0) return 0;

    // visited 位图（Beam 每条路径独立维护，不需要全局 visited）
    int bitmap_size = (node_count + 7) / 8;
    (void)visited;  // 参数保留兼容，beam 内部不使用

    if (start_node_id < 0 || start_node_id >= node_count || !net->nodes[start_node_id]) {
        return 0;
    }

    // Beam 结构
    typedef struct {
        int nodes[32];
        float scores[32];
        float cum_score;
        int len;
        unsigned char* vis;
        int current_id;
        float context_valence;
        float context_features[NODE_FEATURE_DIM];
        int context_count;
    } Beam;

    Beam beams[BEAM_K];
    int active_beams = 0;

    // 初始化第一个 beam
    memset(&beams[0], 0, sizeof(Beam));
    beams[0].vis = (unsigned char*)calloc(bitmap_size, 1);
    if (!beams[0].vis) { return 0; }
    beams[0].nodes[0] = start_node_id;
    beams[0].scores[0] = 1.0f;
    beams[0].cum_score = 1.0f;
    beams[0].len = 1;
    beams[0].current_id = start_node_id;
    beams[0].vis[start_node_id / 8] |= (unsigned char)(1 << (start_node_id % 8));
    ReasoningNode* start_node = net->nodes[start_node_id];
    beams[0].context_valence = start_node ? start_node->valence : 0.0f;
    if (start_node && start_node->features) {
        for (int d = 0; d < NODE_FEATURE_DIM; d++)
            beams[0].context_features[d] += start_node->features[d];
        beams[0].context_count = 1;
    }
    active_beams = 1;

    // 主循环
    while (active_beams > 0) {
        // 收集所有候选（去重：同一 target 保留最高 cum_score）
        #define MAX_CANDIDATES (BEAM_K * 512)
        typedef struct {
            int beam_idx;
            int next_node_id;
            float step_score;
            float cum_score;
        } Candidate;
        Candidate candidates[MAX_CANDIDATES];
        int cand_count = 0;
        // seen[node_id] = index in candidates, -1 = not seen
        int* seen = (int*)malloc(node_count * sizeof(int));
        if (seen) {
            for (int s = 0; s < node_count; s++) seen[s] = -1;
        }

        for (int b = 0; b < active_beams; b++) {
            Beam* beam = &beams[b];
            if (beam->len >= max_len) continue;

            ReasoningNode* current = net->nodes[beam->current_id];
            if (!current || current->connection_count <= 0) continue;

            // 预计算语义上下文均值
            float mean_features[NODE_FEATURE_DIM];
            int has_mean = 0;
            if (beam->context_count > 0) {
                for (int d = 0; d < NODE_FEATURE_DIM; d++)
                    mean_features[d] = beam->context_features[d] / beam->context_count;
                has_mean = 1;
            }

            for (int i = 0; i < current->connection_count; i++) {
                ReasoningNode* target = current->connections[i];
                if (!target) continue;
                int tid = target->node_id;
                if (tid < 0 || tid >= node_count) continue;
                if (beam->vis[tid / 8] & (unsigned char)(1 << (tid % 8))) continue;

                // 评分（简化版：边三维 + 节点二维 + 语义，跳过路径回溯）
                float edge_weight = current->connection_weights[i];
                float edge_conf = (current->connection_confidences && i < current->connection_count)
                                  ? current->connection_confidences[i] : edge_weight;
                float edge_bias = (current->connection_motivational_bias && i < current->connection_count)
                                  ? current->connection_motivational_bias[i] : 0.0f;

                float node_act  = target->activation;
                float node_conf = target->confidence;
                // 回路3b: 混入cognitive_confidence三维综合置信度
                if (target->cognitive_confidence) {
                    cognitive_confidence_compute(target->cognitive_confidence);
                    node_conf = node_conf * 0.6f + target->cognitive_confidence->combined * 0.4f;
                }

                float semantic_score = 0.0f;
                if (target->features && has_mean)
                    semantic_score = cosine_similarity(target->features, mean_features, NODE_FEATURE_DIM);

                float base_score =
                    EDGE_WALK_W_WEIGHT     * edge_weight +
                    EDGE_WALK_W_CONF       * edge_conf   +
                    EDGE_WALK_W_BIAS       * edge_bias   +
                    EDGE_WALK_W_ACTIVATION * node_act    +
                    EDGE_WALK_W_NODE_CONF  * node_conf   +
                    (EDGE_WALK_W_SEMANTIC + (beam->context_count > 5 ? 0.10f : 0.0f)) *
                    (semantic_score + 1.0f) * 0.5f;

                float valence_match = 1.0f - fabsf(beam->context_valence - target->valence) * 0.5f;
                float valence_mod = 0.5f + 0.5f * valence_match;
                float heat_mod = 0.05f + 0.95f * target->heat;
                float score = base_score * valence_mod * (0.5f + 0.5f * intent_weight) * heat_mod;

                // 三元组链式奖励：如果 prev->current->target 构成两步链
                if (beam->len > 1 && edge_weight > 0.2f) {
                    int prev_id = beam->nodes[beam->len - 1];
                    if (prev_id >= 0 && prev_id < node_count) {
                        ReasoningNode* prev = net->nodes[prev_id];
                        if (prev) {
                            for (int ej = 0; ej < prev->connection_count; ej++) {
                                if (prev->connections[ej] == current) {
                                    if (prev->connection_weights[ej] > 0.2f)
                                        score += 0.05f;
                                    break;
                                }
                            }
                        }
                    }
                }

                float new_cum = beam->cum_score + score;
                if (seen && tid >= 0 && tid < node_count) {
                    int existing = seen[tid];
                    if (existing >= 0 && existing < cand_count) {
                        // 已存在：保留更高 cum_score 的
                        if (new_cum > candidates[existing].cum_score) {
                            candidates[existing].beam_idx = b;
                            candidates[existing].step_score = score;
                            candidates[existing].cum_score = new_cum;
                        }
                    } else if (cand_count < MAX_CANDIDATES - 1) {
                        seen[tid] = cand_count;
                        candidates[cand_count].beam_idx = b;
                        candidates[cand_count].next_node_id = tid;
                        candidates[cand_count].step_score = score;
                        candidates[cand_count].cum_score = new_cum;
                        cand_count++;
                    }
                } else if (cand_count < MAX_CANDIDATES - 1) {
                    candidates[cand_count].beam_idx = b;
                    candidates[cand_count].next_node_id = tid;
                    candidates[cand_count].step_score = score;
                    candidates[cand_count].cum_score = new_cum;
                    cand_count++;
                }
            }
        }

        if (cand_count == 0) { free(seen); break; }

        // 按累积得分排序，保留 top-K
        for (int i = 0; i < cand_count && i < BEAM_K; i++) {
            for (int j = i + 1; j < cand_count; j++) {
                if (candidates[j].cum_score > candidates[i].cum_score) {
                    Candidate tmp = candidates[i];
                    candidates[i] = candidates[j];
                    candidates[j] = tmp;
                }
            }
        }

        // 创建下一轮 beams
        Beam next_beams[BEAM_K];
        int next_count = 0;

        for (int ci = 0; ci < cand_count && next_count < BEAM_K; ci++) {
            Candidate* cand = &candidates[ci];
            Beam* src = &beams[cand->beam_idx];

            // 分配新 beam
            Beam* nb = &next_beams[next_count];
            memcpy(nb->nodes, src->nodes, src->len * sizeof(int));
            memcpy(nb->scores, src->scores, src->len * sizeof(float));
            nb->nodes[src->len] = cand->next_node_id;
            nb->scores[src->len] = cand->step_score;
            nb->cum_score = cand->cum_score;
            nb->len = src->len + 1;
            nb->current_id = cand->next_node_id;
            nb->context_valence = src->context_valence;
            memcpy(nb->context_features, src->context_features, NODE_FEATURE_DIM * sizeof(float));
            nb->context_count = src->context_count;

            nb->vis = (unsigned char*)malloc(bitmap_size);
            if (nb->vis) {
                memcpy(nb->vis, src->vis, bitmap_size);
                nb->vis[cand->next_node_id / 8] |= (unsigned char)(1 << (cand->next_node_id % 8));
            }

            // 更新上下文
            ReasoningNode* stepped = net->nodes[cand->next_node_id];
            if (stepped) {
                nb->context_valence = nb->context_valence * 0.7f + stepped->valence * 0.3f;
                if (stepped->features) {
                    for (int d = 0; d < NODE_FEATURE_DIM; d++)
                        nb->context_features[d] += stepped->features[d];
                    nb->context_count++;
                }
            }

            next_count++;
        }

        // 释放旧 beams 的 visited
        for (int b = 0; b < active_beams; b++) {
            if (beams[b].vis && beams[b].vis != visited)
                free(beams[b].vis);
        }
        memcpy(beams, next_beams, next_count * sizeof(Beam));
        active_beams = next_count;
        free(seen);
        seen = NULL;
    }

    // 选累积得分最高的 beam 输出
    int best_idx = 0;
    for (int b = 1; b < active_beams; b++) {
        if (beams[b].cum_score > beams[best_idx].cum_score)
            best_idx = b;
    }

    Beam* best = (active_beams > 0) ? &beams[best_idx] : &beams[0];
    int out_len = best->len;
    if (out_len > max_len) out_len = max_len;
    memcpy(path_out, best->nodes, out_len * sizeof(int));
    if (scores_out)
        memcpy(scores_out, best->scores, out_len * sizeof(float));

    // 释放 visited
    for (int b = 0; b < active_beams; b++) {
        if (beams[b].vis && beams[b].vis != visited)
            free(beams[b].vis);
    }
    return out_len;
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
                        const float* topo_act,
                        const float* query_anchor) {
    (void)query_anchor;  /* 预留: 跨拓扑锚点引导 */
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

    // 防回声：记录已输出的概念字符（路径内防重复）
    char used_chars[256] = {0};
    int used_len = 0;

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

        // 动态剪枝阈值（同 topology_walk_greedy 保持一致）
        float prune_threshold;
        if (path_len <= 3) {
            prune_threshold = 0.03f;
        } else if (path_len <= 6) {
            prune_threshold = 0.10f + (path_len - 3) * 0.04f;
        } else {
            prune_threshold = fminf(0.22f + (path_len - 6) * 0.02f, 0.30f);
        }

        // 预计算语义上下文均值（候选评估循环中不变）
        float mean_features[NODE_FEATURE_DIM];
        int has_mean = 0;
        if (context_count > 0) {
            for (int d = 0; d < NODE_FEATURE_DIM; d++)
                mean_features[d] = context_features[d] / context_count;
            has_mean = 1;
        }

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

            if (used_len > 0 && target->concept && char_in_set(target->concept, used_chars))
                continue;

            float edge_weight = (cur_ra->connection_weights && i < cur_ra->connection_count)
                                ? cur_ra->connection_weights[i] : 0.0f;
            float edge_conf = (cur_ra->connection_confidences && i < cur_ra->connection_count)
                              ? cur_ra->connection_confidences[i] : edge_weight;
            float edge_bias = (cur_ra->connection_motivational_bias && i < cur_ra->connection_count)
                              ? cur_ra->connection_motivational_bias[i] : 0.0f;
            float node_act = target->activation;
            float node_conf = target->confidence;
            // 回路3b: 混入cognitive_confidence三维综合置信度
            if (target->cognitive_confidence) {
                cognitive_confidence_compute(target->cognitive_confidence);
                node_conf = node_conf * 0.6f + target->cognitive_confidence->combined * 0.4f;
            }
            float raw_val = target->valence;

            // --- 语义得分（第7维）---
            float semantic_score = 0.0f;
            if (target->features && has_mean) {
                semantic_score = cosine_similarity(target->features, mean_features, NODE_FEATURE_DIM);
            }

            float base_score =
                EDGE_WALK_W_WEIGHT     * edge_weight +
                EDGE_WALK_W_CONF       * edge_conf   +
                EDGE_WALK_W_BIAS       * edge_bias   +
                EDGE_WALK_W_ACTIVATION * node_act    +
                EDGE_WALK_W_NODE_CONF  * node_conf   +
                (EDGE_WALK_W_SEMANTIC + (context_count > 5 ? 0.10f : 0.0f)) *
                (semantic_score + 1.0f) * 0.5f;

            float valence_mod = 1.0f + EDGE_WALK_VALENCE_COEFF * raw_val;
            // 意图权重乘数：当前拓扑的 topo_act
            float intent_mult = (topo_act && cur_topo < master->sub_topo_count)
                                ? (0.5f + 0.5f * topo_act[cur_topo]) : 1.0f;
            float score = base_score * valence_mod * intent_mult;

            // 热度衰减：被选次数越多得分越低（同 topology_walk_greedy）
            float heat_mod = 0.05f + 0.95f * target->heat;
            score *= heat_mod;


            if (score > best_score) {
                best_score = score;
                best_topo = cur_topo;
                best_node = tid;
            }
        }

        // --- 评估跨拓扑连接 ---
        int adj_idx = cur_topo * MAX_NODES_PER_TOPO + cur_node;
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

                        if (used_len > 0 && tgt_node && tgt_node->concept &&
                            char_in_set(tgt_node->concept, used_chars)) {
                            entry = entry->next;
                            continue;
                        }

                        float cross_weight = link->weight * link->transfer_rate;
                        float node_act = tgt_node ? tgt_node->activation : 0.0f;
                        float node_conf = tgt_node ? tgt_node->confidence : 0.0f;
                        if (tgt_node && tgt_node->cognitive_confidence) {
                            cognitive_confidence_compute(tgt_node->cognitive_confidence);
                            node_conf = node_conf * 0.6f + tgt_node->cognitive_confidence->combined * 0.4f;
                        }
                        float raw_val = tgt_node ? tgt_node->valence : 0.0f;

                        // --- 语义得分（第7维） ---
                        float semantic_score = 0.0f;
                        if (tgt_node && tgt_node->features && has_mean) {
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
                        // 意图权重乘数：目标拓扑的 topo_act
                        float intent_mult = (topo_act && to_topo < master->sub_topo_count)
                                            ? (0.5f + 0.5f * topo_act[to_topo]) : 1.0f;
                        float score = base_score * valence_mod * intent_mult;

                        // 热度衰减：跨拓扑节点同样受热度影响
                        {
                            SubTopology* tgt_sub2 = master_get_sub_topology(master, to_topo);
                            ReasoningNode* tgt_node2 = (tgt_sub2 && tgt_sub2->net && to_node < tgt_sub2->net->node_count)
                                                       ? tgt_sub2->net->nodes[to_node] : NULL;
                            if (tgt_node2) {
                                float heat_mod2 = 0.05f + 0.95f * tgt_node2->heat;
                                score *= heat_mod2;
                            }
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

        if (best_topo < 0 || best_node < 0 || best_score < prune_threshold) break;

        // 记录跨拓扑命中（如果从不同拓扑跳跃过来）
        int prev_topo = cur_topo;
        int prev_node = cur_node;

        cur_topo = best_topo;
        cur_node = best_node;

        // 记录动态跨拓扑建边跟踪
        if (best_topo != prev_topo) {
            master_record_cross_hit(master, prev_topo, prev_node, cur_topo, cur_node);
        }

        // 更新被选节点的热度（跨拓扑同样需要热度衰减）
        {
            SubTopology* selected_sub = master_get_sub_topology(master, cur_topo);
            if (selected_sub && selected_sub->net && cur_node < selected_sub->net->node_count) {
                ReasoningNode* selected_node = selected_sub->net->nodes[cur_node];
                if (selected_node) {
                    selected_node->selection_count++;
                    float decay;
                    switch (selected_node->node_type) {
                        case NODE_TYPE_FUNCTION_WORD: decay = 0.999f; break;
                        case NODE_TYPE_PROPER_NOUN:  decay = 0.990f; break;
                        default:                     decay = 0.995f; break;
                    }
                    selected_node->heat *= decay;
                    if (selected_node->heat < 0.05f) selected_node->heat = 0.05f;
                }
            }
        }

        if (cur_topo >= 0 && cur_topo < visited_count && visited_bitmaps[cur_topo]) {
            SubTopology* sub = master_get_sub_topology(master, cur_topo);
            if (sub && sub->net && cur_node < sub->net->node_count) {
                visited_bitmaps[cur_topo][cur_node / 8] |= (unsigned char)(1 << (cur_node % 8));
            }
        }

        // 将当前节点的概念字符加入 used_chars（防回声）
        {
            SubTopology* sub = master_get_sub_topology(master, cur_topo);
            if (sub && sub->net && cur_node < sub->net->node_count) {
                ReasoningNode* stepped = sub->net->nodes[cur_node];
                if (stepped && stepped->concept) {
                    for (const char* cp = stepped->concept; *cp && used_len < 250; ) {
                        int clen = 1;
                        if ((unsigned char)*cp >= 0xC0 && (unsigned char)*cp < 0xE0) clen = 2;
                        else if ((unsigned char)*cp >= 0xE0 && (unsigned char)*cp < 0xF0) clen = 3;
                        else if ((unsigned char)*cp >= 0xF0) clen = 4;
                        for (int b = 0; b < clen && used_len < 250; b++)
                            used_chars[used_len++] = *cp++;
                    }
                }
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

// ==================== 特征相似度边重建 ====================

int master_rebuild_edges_by_similarity(MasterTopology* master, float threshold, int max_connections) {
    if (!master) return -1;
    if (threshold <= 0.0f) threshold = 0.35f;
    if (max_connections <= 0) max_connections = 8;

    int total_edges = 0;

    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net || !sub->node_hash) continue;

        HuarongTopologyNet* net = sub->net;
        if (net->node_count < 2) continue;

        // 用 top-N 选择（保持一个固定大小的数组，只在有新高分时插入）
        typedef struct { int idx; float sim; } SimEntry;

        for (int n = 0; n < net->node_count; n++) {
            ReasoningNode* src = net->nodes[n];
            if (!src || !src->features || src->feature_dim <= 0) continue;

            // top-N 候选（初始化为低分）
            SimEntry top[64];
            int top_count = 0;
            int max_t = max_connections > 64 ? 64 : max_connections;

            for (int m = n + 1; m < net->node_count; m++) {
                ReasoningNode* tgt = net->nodes[m];
                if (!tgt || !tgt->features || tgt->feature_dim != src->feature_dim) continue;
                float sim = cosine_similarity(src->features, tgt->features, src->feature_dim);
                if (isnan(sim) || isinf(sim)) sim = 0.0f;
                if (sim <= 0.0f || sim < threshold) continue;

                // 插入到 top-N 的合适位置
                if (top_count < max_t) {
                    top[top_count].idx = m;
                    top[top_count].sim = sim;
                    top_count++;
                } else {
                    // 替换最低分
                    int min_idx = 0;
                    for (int k = 1; k < top_count; k++) {
                        if (top[k].sim < top[min_idx].sim) min_idx = k;
                    }
                    if (sim > top[min_idx].sim) {
                        top[min_idx].idx = m;
                        top[min_idx].sim = sim;
                    }
                }
            }

            // 建边
            for (int p = 0; p < top_count; p++) {
                int ret = huarong_net_add_connection(net, n, top[p].idx, top[p].sim);
                if (ret == 0) total_edges++;
            }
        }
    }

    printf("[边重建] 基于特征相似度重建 %d 条边\n", total_edges);
    return total_edges;
}

// ==================== 状态持久化功能 (v5 format: 含特征向量+sentinel) ====================

#define STATE_FORMAT_VERSION 5

int master_save_state(MasterTopology* master, const char* file_path) {
    if (!master || !file_path) return -1;
    
    FILE* fp = fopen(file_path, "wb");
    if (!fp) {
        printf("[状态持久化] 无法创建文件: %s\n", file_path);
        return -1;
    }
    
    // 写文件头: 格式版本 + 特征维度校验
    int fmt_ver = STATE_FORMAT_VERSION;
    fwrite(&fmt_ver, sizeof(int), 1, fp);
    int feat_dim = NODE_FEATURE_DIM;
    fwrite(&feat_dim, sizeof(int), 1, fp);
    
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
            
            // [v4] 写特征向量 (feature_dim + NODE_FEATURE_DIM 个float)
            int feat_dim = (node->features && node->feature_dim > 0) ? node->feature_dim : 0;
            fwrite(&feat_dim, sizeof(int), 1, fp);
            if (feat_dim > 0) {
                int write_dim = feat_dim < NODE_FEATURE_DIM ? feat_dim : NODE_FEATURE_DIM;
                fwrite(node->features, sizeof(float), write_dim, fp);
                // 补零到 NODE_FEATURE_DIM
                if (write_dim < NODE_FEATURE_DIM) {
                    float zero = 0.0f;
                    for (int p = write_dim; p < NODE_FEATURE_DIM; p++) {
                        fwrite(&zero, sizeof(float), 1, fp);
                    }
                }
            } else {
                // 无特征, 写 NODE_FEATURE_DIM 个零
                float zero = 0.0f;
                for (int p = 0; p < NODE_FEATURE_DIM; p++) {
                    fwrite(&zero, sizeof(float), 1, fp);
                }
            }
            
            // [v3] 写连接数 + 连接数据 (target_concept_len, target_concept, weight, bias, confidence)
            int safe_conn_count = node->connection_count;
            if (!node->connections) safe_conn_count = 0;
            fwrite(&safe_conn_count, sizeof(int), 1, fp);
            for (int c = 0; c < safe_conn_count; c++) {
                if (node->connections[c] && node->connections[c]->concept) {
                    int tgt_len = strlen(node->connections[c]->concept) + 1;
                    fwrite(&tgt_len, sizeof(int), 1, fp);
                    fwrite(node->connections[c]->concept, 1, tgt_len, fp);
                } else {
                    int tgt_len = 0;
                    fwrite(&tgt_len, sizeof(int), 1, fp);
                }
                float w = node->connection_weights ? node->connection_weights[c] : 0.0f;
                float b = node->connection_motivational_bias ? node->connection_motivational_bias[c] : 0.0f;
                float c2 = node->connection_confidences ? node->connection_confidences[c] : 0.0f;
                fwrite(&w, sizeof(float), 1, fp);
                fwrite(&b, sizeof(float), 1, fp);
                fwrite(&c2, sizeof(float), 1, fp);
            }
            
            saved_nodes++;
        }
    }
    
    // Sentinel: mark end of node data section
    uint32_t sentinel = 0xDEADBEEF;
    fwrite(&sentinel, sizeof(uint32_t), 1, fp);
    
    // 统计实际非 NULL 的跨拓扑连接数（剪枝可能造成 NULL 槽位）
    int actual_cross_links = 0;
    for (int i = 0; i < master->cross_link_count; i++) {
        if (master->cross_links[i]) actual_cross_links++;
    }
    fwrite(&actual_cross_links, sizeof(int), 1, fp);
    
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

    // ========== 频率表扩展保存 ==========
    {
        // 写入一个哨兵记录([-1,0,0,0,0,0])标记跨链接束/频率表开始
        int freq_sentinel[6] = {-1, 0, 0, 0, 0, 0};
        fwrite(freq_sentinel, sizeof(int), 6, fp);

        int tpl_voting = master->use_template_voting;
        fwrite(&tpl_voting, sizeof(int), 1, fp);
        fwrite(&master->template_decay_round, sizeof(int), 1, fp);

        if (master->freq_table) {
            pthread_mutex_lock(&master->freq_table->mutex);
            fwrite(&master->freq_table->entry_count, sizeof(int), 1, fp);
            fwrite(&master->freq_table->total_triplets, sizeof(int64_t), 1, fp);
            fwrite(&master->freq_table->round, sizeof(int), 1, fp);

            int iter = 0;
            const PathTripletRecord* rec;
            while ((rec = path_freq_table_iter(master->freq_table, &iter)) != NULL) {
                fwrite(rec, sizeof(PathTripletRecord), 1, fp);
            }
            pthread_mutex_unlock(&master->freq_table->mutex);
        } else {
            int zero = 0;
            fwrite(&zero, sizeof(int), 1, fp);
            fwrite(&zero, sizeof(int), 1, fp);
            fwrite(&zero, sizeof(int), 1, fp);
        }
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
    // fmt_ver ∈ {2,3,4,5} = 带版本头的格式化文件
    // fmt_ver 为其他值 = v1 格式（文件头就是节点数据，回退重读）
    if (fmt_ver != 2 && fmt_ver != 3 && fmt_ver != 4 && fmt_ver != 5) {
        fseek(fp, 0, SEEK_SET);
        fmt_ver = 1;
    }

    // v5+: 读取特征维度校验
    int file_feat_dim = -1;
    if (fmt_ver >= 5) {
        if (fread(&file_feat_dim, sizeof(int), 1, fp) == 1) {
            if (file_feat_dim != NODE_FEATURE_DIM) {
                fprintf(stderr, "[状态持久化] 特征维度不匹配: 文件=%d 当前=%d\n",
                        file_feat_dim, NODE_FEATURE_DIM);
                fclose(fp);
                return -1;
            }
        } else {
            fclose(fp);
            return -1;
        }
    }
    (void)file_feat_dim;
    
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
        
        // [v4] 读特征向量 — 临时缓冲区, 创建节点后再赋值
        float feat_buf[NODE_FEATURE_DIM];
        int has_v4_features = 0;
        if (fmt_ver >= 4) {
            int feat_dim;
            if (fread(&feat_dim, sizeof(int), 1, fp) != 1) break;
            if (feat_dim > 0) {
                int read_dim = feat_dim < NODE_FEATURE_DIM ? feat_dim : NODE_FEATURE_DIM;
                if (fread(feat_buf, sizeof(float), read_dim, fp) != (size_t)read_dim) break;
                // 跳过补零部分
                if (read_dim < NODE_FEATURE_DIM) {
                    float skip;
                    for (int p = read_dim; p < NODE_FEATURE_DIM; p++) {
                        if (fread(&skip, sizeof(float), 1, fp) != 1) break;
                    }
                }
            } else {
                // feat_dim == 0: 跳过 NODE_FEATURE_DIM 个零
                float skip;
                for (int p = 0; p < NODE_FEATURE_DIM; p++) {
                    if (fread(&skip, sizeof(float), 1, fp) != 1) break;
                }
            }
            has_v4_features = 1;
        }
        
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
                // v3: 跳过自动连接—边数据随后从文件中恢复
                // v2/v1: 自动连接（因为没有边数据可恢复）
                if (fmt_ver < 3) {
                    auto_connect_new_node(master, target_topo, node);
                }
            }
        }
        if (node) {
            node->activation = activation;
            
            // [v4] 恢复特征向量
            if (has_v4_features && node) {
                if (!node->features) {
                    node->features = (float*)malloc(NODE_FEATURE_DIM * sizeof(float));
                    node->feature_dim = NODE_FEATURE_DIM;
                }
                if (node->features) {
                    memcpy(node->features, feat_buf, NODE_FEATURE_DIM * sizeof(float));
                    node->feature_dim = NODE_FEATURE_DIM;
                }
            }
        }

        // 连接数据处理
        if (fmt_ver >= 3) {
            // [v3] 按概念名恢复连接
            for (int c = 0; c < conn_count && conn_count > 0 && conn_count < 10000; c++) {
                int tgt_concept_len;
                if (fread(&tgt_concept_len, sizeof(int), 1, fp) != 1) break;
                if (tgt_concept_len > 0 && tgt_concept_len <= 4096) {
                    char tgt_concept[4096];
                    if (fread(tgt_concept, 1, tgt_concept_len, fp) != (size_t)tgt_concept_len) break;
                    tgt_concept[tgt_concept_len - 1] = '\0';

                    float conn_w, conn_b, conn_c;
                    if (fread(&conn_w, sizeof(float), 1, fp) != 1) break;
                    if (fread(&conn_b, sizeof(float), 1, fp) != 1) break;
                    if (fread(&conn_c, sizeof(float), 1, fp) != 1) break;

                    if (node && target_topo->net) {
                        ReasoningNode* tgt = node_hash_find(target_topo->node_hash, tgt_concept);
                        if (tgt && tgt != node) {
                            int ret = huarong_net_add_connection(target_topo->net,
                                node->node_id, tgt->node_id, conn_w);
                            if (ret == 0) {
                                // 覆盖默认的 bias/confidence 为保存的值
                                int idx = node->connection_count - 1;
                                if (idx >= 0) {
                                    if (node->connection_motivational_bias)
                                        node->connection_motivational_bias[idx] = conn_b;
                                    if (node->connection_confidences)
                                        node->connection_confidences[idx] = conn_c;
                                }
                            }
                        }
                    }
                } else {
                    // tgt_concept_len == 0: 空连接，跳过 weight/bias/confidence
                    float skip_w, skip_b, skip_c;
                    if (fread(&skip_w, sizeof(float), 1, fp) != 1) break;
                    if (fread(&skip_b, sizeof(float), 1, fp) != 1) break;
                    if (fread(&skip_c, sizeof(float), 1, fp) != 1) break;
                }
            }
        } else if (fmt_ver >= 2) {
            // [v2] 读取并丢弃连接数据（目标用 node_id 存储，加载后无效）
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
    // Read sentinel between node and cross-link sections
    uint32_t sentinel = 0;
    int expected_cross_count = 0;
    if (fread(&sentinel, sizeof(uint32_t), 1, fp) == 1) {
        if (sentinel == 0xDEADBEEF) {
            // 新格式：有 sentinel，读取 cross_link_count
            if (fread(&expected_cross_count, sizeof(int), 1, fp) != 1) {
                expected_cross_count = 0;
            }
        } else {
            // 旧格式：回退 4 字节，以原始格式读取跨链接
            fseek(fp, -(long)sizeof(uint32_t), SEEK_CUR);
            expected_cross_count = 0;
        }
    }
    
    int from_topo = 0, from_node = 0, to_topo = 0, to_node = 0;
    while (1) {
        float weight;
        int use_count;
        
        if (fread(&from_topo, sizeof(int), 1, fp) != 1) break;
        if (fread(&from_node, sizeof(int), 1, fp) != 1) break;
        if (fread(&to_topo, sizeof(int), 1, fp) != 1) break;
        if (fread(&to_node, sizeof(int), 1, fp) != 1) break;
        if (fread(&weight, sizeof(float), 1, fp) != 1) break;
        if (fread(&use_count, sizeof(int), 1, fp) != 1) break;
        
        // Validate topology indices dynamically
        int max_topo = master->sub_topo_count;
        if (from_topo < 0 || from_topo >= max_topo ||
            to_topo < 0 || to_topo >= max_topo) {
            break;
        }
        
        int link_result = master_add_cross_link(master, from_topo, from_node,
                                                       to_topo, to_node, 
                                                       weight, "seed");
        if (link_result >= 0) {
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

    // ========== 频率表扩展加载 ==========
    // 哨兵记录 [-1,0,0,0,0,0] 已在跨拓扑循环中消耗(from_topo=-1触发break)
    if (from_topo == -1) {
        int tpl_voting = 0;
        fread(&tpl_voting, sizeof(int), 1, fp);
        master->use_template_voting = tpl_voting;
        fread(&master->template_decay_round, sizeof(int), 1, fp);

        int freq_entry_count = 0;
        int64_t freq_total = 0;
        int freq_round = 0;
        if (fread(&freq_entry_count, sizeof(int), 1, fp) == 1 &&
            fread(&freq_total, sizeof(int64_t), 1, fp) == 1 &&
            fread(&freq_round, sizeof(int), 1, fp) == 1) {

            if (freq_entry_count > 0 && master->freq_table) {
                master->freq_table->total_triplets = freq_total;
                master->freq_table->round = freq_round;

                for (int i = 0; i < freq_entry_count; i++) {
                    PathTripletRecord rec;
                    if (fread(&rec, sizeof(PathTripletRecord), 1, fp) != 1) break;
                    if (!rec.is_active) continue;
                    path_freq_table_set(master->freq_table,
                                        rec.topo_id, rec.node_a, rec.node_b, rec.node_c,
                                        rec.count);
                }
            }
        }
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
