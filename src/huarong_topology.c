#include "huarong_topology.h"
#include "node_hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ==================== 推理节点实现 ==================== 

ReasoningNode* create_reasoning_node(int node_id, const char* concept, 
                                   float* features, int feature_dim) {
    ReasoningNode* node = (ReasoningNode*)malloc(sizeof(ReasoningNode));
    if (!node) return NULL;
    
    node->node_id = node_id;
    node->concept = strdup(concept);
    node->feature_dim = feature_dim;
    node->connection_count = 0;
    node->activation = 0.0f;
    node->confidence = 0.5f;
    node->is_reversible = 1;
    node->is_visited = 0;
    
    // 初始化新参数: 三维置信度
    node->cognitive_confidence = cognitive_confidence_create();
    if (node->cognitive_confidence) {
        cognitive_confidence_update(node->cognitive_confidence, 0.5f, 0.5f, 0.5f);
    }
    
    // 初始化新参数: 效价
    node->valence = 0.0f;
    
    // 初始化热度衰减
    node->heat = 1.0f;               // 初始满热度
    node->selection_count = 0;
    node->node_type = NODE_TYPE_COMMON_WORD;  // 默认普通词
    
    // 分配特征向量
    if (feature_dim > 0) {
        node->features = (float*)malloc(feature_dim * sizeof(float));
        if (node->features) {
            if (features) {
                memcpy(node->features, features, feature_dim * sizeof(float));
            } else {
                memset(node->features, 0, feature_dim * sizeof(float));
            }
        }
        /* 若 node->features==NULL (malloc 失败)，回滚时 free(NULL) 安全，
           后续 connection 数组分配失败路径会统一释放 */
    } else {
        node->features = NULL;
    }
    
    // 初始化连接数组（预分配 DEFAULT_CONNECTION_CAPACITY 个连接空间）
    node->connections = (ReasoningNode**)malloc(DEFAULT_CONNECTION_CAPACITY * sizeof(ReasoningNode*));
    node->connection_weights = (float*)malloc(DEFAULT_CONNECTION_CAPACITY * sizeof(float));
    node->connection_motivational_bias = (float*)malloc(DEFAULT_CONNECTION_CAPACITY * sizeof(float));
    node->connection_confidences = (float*)malloc(DEFAULT_CONNECTION_CAPACITY * sizeof(float));
    node->connection_capacity = DEFAULT_CONNECTION_CAPACITY;

    // 任一分配失败则全部回滚，返回 NULL
    if (!node->connections || !node->connection_weights ||
        !node->connection_motivational_bias || !node->connection_confidences) {
        free(node->connections); free(node->connection_weights);
        free(node->connection_motivational_bias); free(node->connection_confidences);
        free(node->concept);
        free(node->features);
        if (node->cognitive_confidence)
            cognitive_confidence_destroy(node->cognitive_confidence);
        free(node);
        return NULL;
    }

    // 初始化权重和动机倾向
    for (int i = 0; i < DEFAULT_CONNECTION_CAPACITY; i++) {
        node->connection_weights[i] = 0.5f;
        node->connection_motivational_bias[i] = 0.5f;
        node->connection_confidences[i] = 0.5f;
    }
    
    return node;
}

void destroy_reasoning_node(ReasoningNode* node) {
    if (!node) return;
    
    free(node->concept);
    free(node->features);
    free(node->connections);
    free(node->connection_weights);
    free(node->connection_motivational_bias);
    free(node->connection_confidences);
    
    // 释放三维置信度
    if (node->cognitive_confidence) {
        cognitive_confidence_destroy(node->cognitive_confidence);
    }
    
    free(node);
}

// ==================== 华容道拓扑网络核心实现 ==================== 

HuarongTopologyNet* huarong_net_create(int max_nodes) {
    HuarongTopologyNet* net = (HuarongTopologyNet*)malloc(sizeof(HuarongTopologyNet));
    if (!net) return NULL;
    
    net->nodes = (ReasoningNode**)calloc(max_nodes, sizeof(ReasoningNode*));
    net->max_nodes = max_nodes;  // 保存最大节点数
    net->node_count = 0;
    net->learning_rate = 0.01f;
    net->is_training = 0;

    pthread_mutex_init(&net->mutex, NULL);
    
    return net;
}

void huarong_net_destroy(HuarongTopologyNet* net) {
    if (!net) return;
    
    // 销毁所有节点
    for (int i = 0; i < net->node_count; i++) {
        destroy_reasoning_node(net->nodes[i]);
    }
    free(net->nodes);
    
    pthread_mutex_destroy(&net->mutex);
    
    free(net);
}

ReasoningNode* huarong_net_add_node(HuarongTopologyNet* net, 
                                   const char* concept, 
                                   float* features, 
                                   int feature_dim) {
    if (!net || !concept) return NULL;
    
    pthread_mutex_lock(&net->mutex);
    
    // 检查是否已达到最大容量
    if (net->node_count >= net->max_nodes || net->nodes == NULL) {
        pthread_mutex_unlock(&net->mutex);
        return NULL;
    }
    
    int node_id = net->node_count;
    ReasoningNode* node = create_reasoning_node(node_id, concept, features, feature_dim);
    
    if (node) {
        net->nodes[net->node_count++] = node;
    }
    
    pthread_mutex_unlock(&net->mutex);
    
    return node;
}

/**
 * 线程安全的查找或创建节点
 * 在 net->mutex 保护下，先查哈希表/线性扫描，找不到则创建
 */
ReasoningNode* huarong_net_find_or_create_node(HuarongTopologyNet* net,
                                               const char* concept,
                                               float* features,
                                               int feature_dim,
                                               NodeHashTable* hash) {
    if (!net || !concept) return NULL;

    pthread_mutex_lock(&net->mutex);

    // 先查哈希表
    if (hash) {
        ReasoningNode* existing = node_hash_find(hash, concept);
        if (existing) {
            pthread_mutex_unlock(&net->mutex);
            return existing;
        }
    }

    // 再线性扫描（容错：哈希表可能不完整）
    for (int i = 0; i < net->node_count; i++) {
        ReasoningNode* node = net->nodes[i];
        if (node && node->concept && strcmp(node->concept, concept) == 0) {
            // 补入哈希表
            if (hash) node_hash_add(hash, node);
            pthread_mutex_unlock(&net->mutex);
            return node;
        }
    }

    // 没有找到 → 创建新节点
    if (net->node_count >= net->max_nodes || net->nodes == NULL) {
        pthread_mutex_unlock(&net->mutex);
        return NULL;
    }

    int node_id = net->node_count;
    ReasoningNode* node = create_reasoning_node(node_id, concept, features, feature_dim);

    if (node) {
        net->nodes[net->node_count++] = node;
        if (hash) node_hash_add(hash, node);
    }

    pthread_mutex_unlock(&net->mutex);
    return node;
}

int huarong_net_add_connection(HuarongTopologyNet* net,
                              int from_node_id,
                              int to_node_id,
                              float weight) {
    if (!net || from_node_id >= net->node_count || to_node_id >= net->node_count) {
        return -1;
    }

    pthread_mutex_lock(&net->mutex);

    ReasoningNode* from_node = net->nodes[from_node_id];
    ReasoningNode* to_node = net->nodes[to_node_id];

    if (!from_node || !to_node) { pthread_mutex_unlock(&net->mutex); return -1; }

    // 检查连接是否已存在
    for (int i = 0; i < from_node->connection_count; i++) {
        if (from_node->connections[i] == to_node) {
            from_node->connection_weights[i] = weight;
            pthread_mutex_unlock(&net->mutex);
            return 0;
        }
    }

    // 检查容量，必要时扩容
    if (from_node->connection_count >= from_node->connection_capacity) {
        int new_cap = from_node->connection_capacity * 2;

        ReasoningNode** new_conn = (ReasoningNode**)malloc(new_cap * sizeof(ReasoningNode*));
        float* new_weights = (float*)malloc(new_cap * sizeof(float));
        float* new_bias = (float*)malloc(new_cap * sizeof(float));
        float* new_conf = (float*)malloc(new_cap * sizeof(float));

        if (!new_conn || !new_weights || !new_bias || !new_conf) {
            free(new_conn); free(new_weights); free(new_bias); free(new_conf);
            pthread_mutex_unlock(&net->mutex);
            return -1;
        }

        memcpy(new_conn, from_node->connections, from_node->connection_capacity * sizeof(ReasoningNode*));
        memcpy(new_weights, from_node->connection_weights, from_node->connection_capacity * sizeof(float));
        memcpy(new_bias, from_node->connection_motivational_bias, from_node->connection_capacity * sizeof(float));
        memcpy(new_conf, from_node->connection_confidences, from_node->connection_capacity * sizeof(float));

        free(from_node->connections);
        free(from_node->connection_weights);
        free(from_node->connection_motivational_bias);
        free(from_node->connection_confidences);

        from_node->connections = new_conn;
        from_node->connection_weights = new_weights;
        from_node->connection_motivational_bias = new_bias;
        from_node->connection_confidences = new_conf;
        from_node->connection_capacity = new_cap;
    }

    // 添加新连接
    from_node->connections[from_node->connection_count] = to_node;
    from_node->connection_weights[from_node->connection_count] = weight;
    from_node->connection_motivational_bias[from_node->connection_count] = 0.5f;
    from_node->connection_confidences[from_node->connection_count] = 0.5f;
    from_node->connection_count++;

    pthread_mutex_unlock(&net->mutex);
    return 0;
}

// 添加双向连接（两个方向都有连接）
int huarong_net_add_bidirectional_connection(HuarongTopologyNet* net,
                                              int node_a_id, int node_b_id,
                                              float weight) {
    int ret1 = huarong_net_add_connection(net, node_a_id, node_b_id, weight);
    int ret2 = huarong_net_add_connection(net, node_b_id, node_a_id, weight);
    return (ret1 == 0 && ret2 == 0) ? 0 : -1;
}

// ==================== 动态网络操作 ==================== 

/**
 * 查找表条目：将节点指针映射到其在 net->nodes 中的索引
 */
typedef struct {
    ReasoningNode* ptr;
    int idx;
} NodeIdxLookup;

/** 按指针值排序的比较函数 */
static int node_idx_cmp(const void* a, const void* b) {
    const NodeIdxLookup* la = (const NodeIdxLookup*)a;
    const NodeIdxLookup* lb = (const NodeIdxLookup*)b;
    return (la->ptr < lb->ptr) ? -1 : (la->ptr > lb->ptr) ? 1 : 0;
}

/**
 * 二分查找节点指针在 net->nodes 中的索引
 * 预建排序查找表，O(log N) 替代原 O(N) 线性搜索
 */
static inline int lookup_node_by_ptr(NodeIdxLookup* table, int count, ReasoningNode* ptr) {
    NodeIdxLookup key;
    key.ptr = ptr;
    NodeIdxLookup* found = (NodeIdxLookup*)bsearch(&key, table, count, sizeof(NodeIdxLookup), node_idx_cmp);
    return found ? found->idx : -1;
}

int* huarong_net_topological_sort(HuarongTopologyNet* net, int* path_length) {
    if (!net || !path_length) return NULL;

    // 预建节点指针→索引查找表（按指针排序，bsearch可用）
    // 将原 O(N²)（N节点×E边×N搜索）降为 O(N log N + E log N)
    NodeIdxLookup* lookup = (NodeIdxLookup*)malloc(net->node_count * sizeof(NodeIdxLookup));
    if (!lookup) return NULL;
    for (int i = 0; i < net->node_count; i++) {
        lookup[i].ptr = net->nodes[i];
        lookup[i].idx = i;
    }
    qsort(lookup, net->node_count, sizeof(NodeIdxLookup), node_idx_cmp);

    int* sorted_nodes = (int*)malloc(net->node_count * sizeof(int));
    int* in_degree = (int*)calloc(net->node_count, sizeof(int));
    int* visited = (int*)calloc(net->node_count, sizeof(int));

    if (!sorted_nodes || !in_degree || !visited) {
        free(sorted_nodes); free(in_degree); free(visited); free(lookup);
        return NULL;
    }

    // 计算每个节点的入度 O(N+E)
    for (int i = 0; i < net->node_count; i++) {
        ReasoningNode* node = net->nodes[i];
        for (int j = 0; j < node->connection_count; j++) {
            int k = lookup_node_by_ptr(lookup, net->node_count, node->connections[j]);
            if (k >= 0) in_degree[k]++;
        }
    }

    // Kahn算法（队列实现，O(N+E) 替代原线性扫描 O(N²)）
    int* queue = (int*)malloc(net->node_count * sizeof(int));
    if (!queue) {
        free(sorted_nodes); free(in_degree); free(lookup);
        return NULL;
    }
    int front = 0, rear = 0;

    // 第一批零入度节点入队
    for (int i = 0; i < net->node_count; i++) {
        if (in_degree[i] == 0) queue[rear++] = i;
    }

    int sorted_count = 0;
    while (front < rear) {
        int i = queue[front++];
        sorted_nodes[sorted_count++] = i;

        // 更新相邻节点的入度，新零入度节点入队
        ReasoningNode* node = net->nodes[i];
        for (int j = 0; j < node->connection_count; j++) {
            int k = lookup_node_by_ptr(lookup, net->node_count, node->connections[j]);
            if (k >= 0 && --in_degree[k] == 0) {
                queue[rear++] = k;
            }
        }
    }

    if (sorted_count != net->node_count) {
        // 存在环，无法进行拓扑排序
        printf("警告：网络中存在环，无法进行拓扑排序\n");
    }

    *path_length = sorted_count;

    free(queue);
    free(lookup);
    free(in_degree);
    free(visited);

    return sorted_nodes;
}

// ==================== 动态网络操作 ==================== 

int huarong_net_dynamic_add_node(HuarongTopologyNet* net,
                                const char* concept,
                                float* features,
                                int feature_dim) {
    if (!net || net->node_count >= 1000) return -1;
    
    ReasoningNode* new_node = huarong_net_add_node(net, concept, features, feature_dim);
    return new_node ? new_node->node_id : -1;
}

int huarong_net_dynamic_remove_node(HuarongTopologyNet* net, int node_id) {
    if (!net || node_id >= net->node_count) return -1;
    
    pthread_mutex_lock(&net->mutex);
    
    ReasoningNode* victim = net->nodes[node_id];
    if (!victim) {
        pthread_mutex_unlock(&net->mutex);
        return -1;
    }
    
    // 关键修复：清理所有其他节点中指向被删节点的连接引用
    // 避免后续走边/激活传播访问悬空指针导致 use-after-free
    for (int i = 0; i < net->node_count; i++) {
        if (i == node_id) continue;
        ReasoningNode* other = net->nodes[i];
        if (!other) continue;
        int kept = 0;
        for (int c = 0; c < other->connection_count; c++) {
            if (other->connections[c] != victim) {
                if (kept != c) {
                    other->connections[kept] = other->connections[c];
                    other->connection_weights[kept] = other->connection_weights[c];
                    other->connection_motivational_bias[kept] = other->connection_motivational_bias[c];
                    other->connection_confidences[kept] = other->connection_confidences[c];
                }
                kept++;
            }
        }
        other->connection_count = kept;
    }
    
    pthread_mutex_unlock(&net->mutex);
    
    // 移除节点并重新组织数组
    destroy_reasoning_node(victim);
    
    for (int i = node_id; i < net->node_count - 1; i++) {
        net->nodes[i] = net->nodes[i + 1];
        net->nodes[i]->node_id = i; // 更新节点ID
    }
    
    net->node_count--;
    return 0;
}

// ==================== 网络优化 ==================== 

void huarong_net_optimize(HuarongTopologyNet* net) {
    if (!net) return;
    
    // 去除冗余连接（权重过小的连接）
    float threshold = 0.01f;
    
    for (int i = 0; i < net->node_count; i++) {
        ReasoningNode* node = net->nodes[i];
        int new_connection_count = 0;
        
        for (int j = 0; j < node->connection_count; j++) {
            if (fabs(node->connection_weights[j]) > threshold) {
                node->connections[new_connection_count] = node->connections[j];
                node->connection_weights[new_connection_count] = node->connection_weights[j];
                if (node->connection_motivational_bias)
                    node->connection_motivational_bias[new_connection_count] = node->connection_motivational_bias[j];
                if (node->connection_confidences)
                    node->connection_confidences[new_connection_count] = node->connection_confidences[j];
                new_connection_count++;
            }
        }
        
        node->connection_count = new_connection_count;
    }
    
    printf("网络优化完成：移除了权重小于%.3f的冗余连接\n", threshold);
}

int huarong_net_prune_edges(HuarongTopologyNet* net, float min_confidence, float min_weight) {
    if (!net) return 0;
    int total_pruned = 0;

    for (int i = 0; i < net->node_count; i++) {
        ReasoningNode* node = net->nodes[i];
        if (!node) continue;
        int kept = 0;

        for (int j = 0; j < node->connection_count; j++) {
            float conf = (node->connection_confidences && j < node->connection_count)
                         ? node->connection_confidences[j] : 0.5f;
            float w = node->connection_weights[j];
            // 保留条件：置信度够高 或 权重大
            if (conf >= min_confidence || fabsf(w) >= min_weight) {
                if (kept != j) {
                    node->connections[kept] = node->connections[j];
                    node->connection_weights[kept] = w;
                    if (node->connection_motivational_bias)
                        node->connection_motivational_bias[kept] = node->connection_motivational_bias[j];
                    if (node->connection_confidences)
                        node->connection_confidences[kept] = conf;
                }
                kept++;
            } else {
                total_pruned++;
            }
        }
        node->connection_count = kept;
    }

    if (total_pruned > 0)
        printf("[拓扑剪枝] 移除 %d 条低质量边 (min_conf=%.3f, min_weight=%.3f)\n",
               total_pruned, min_confidence, min_weight);
    return total_pruned;
}
