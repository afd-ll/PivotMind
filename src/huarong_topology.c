#include "huarong_topology.h"
#include "node_hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* 从概念文本生成确定性特征种子（FNV-1a 哈希 → 浮点向量） */
static void concept_to_feature_seed(const char* concept, float* feats, int dim) {
    if (!concept || !feats || dim <= 0) return;
    /* FNV-1a 种子哈希 */
    unsigned hash = 2166136261u;
    for (const char* p = concept; *p; p++) {
        hash ^= (unsigned char)*p;
        hash *= 16777619u;
    }
    /* 用 hash 生成 24 维确定性伪随机种子，扩展到 dim */ 
    for (int i = 0; i < dim; i++) {
        unsigned h = hash ^ (unsigned)(i * 0x9E3779B9u);
        h = (h ^ (h >> 16)) * 0x85EBCA6Bu;
        h = (h ^ (h >> 13)) * 0xC2B2AE35u;
        h = h ^ (h >> 16);
        /* 映射到 [-0.1, 0.1]，作为小信号种子 */
        feats[i] = ((float)(h & 0xFFFF) / 32768.0f - 1.0f) * 0.1f;
    }
}

// ==================== 推理节点实现 ==================== 

ReasoningNode* create_reasoning_node(int node_id, const char* concept, 
                                   float* features, int feature_dim) {
    ReasoningNode* node = (ReasoningNode*)malloc(sizeof(ReasoningNode));
    if (!node) return NULL;
    
    node->node_id = node_id;
    node->concept = strdup(concept);
    if (!node->concept) {
        free(node);
        return NULL;
    }
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
    node->tpl_pos_len = 0;     // 非模板节点
    memset(node->tpl_pos_seq, 0, sizeof(node->tpl_pos_seq));
    memset(node->tpl_connectors, 0, sizeof(node->tpl_connectors));
    node->conn_hash = NULL;
    node->conn_hash_mask = -1;
    node->conn_hash_entries = 0;
    
    // 分配特征向量
    if (feature_dim > 0 && feature_dim <= 1000000) {  /* 防止整数溢出 */
        node->features = (float*)malloc(feature_dim * sizeof(float));
        if (node->features) {
            if (features) {
                memcpy(node->features, features, feature_dim * sizeof(float));
            } else {
                /* 用概念文本哈希生成非零种子特征，而非全零 */
                concept_to_feature_seed(concept, node->features, feature_dim);
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
    node_conn_hash_free(node);
    
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
    
    pthread_mutex_init(&net->mutex, NULL);
    for (int i = 0; i < 256; i++)
        pthread_mutex_init(&net->node_locks[i], NULL);
    net->retired_conns   = NULL;
    net->retired_pending = 0;
    
    net->nodes = (ReasoningNode**)calloc(max_nodes, sizeof(ReasoningNode*));
    if (!net->nodes) {
        pthread_mutex_destroy(&net->mutex);
        free(net);
        return NULL;
    }
    net->max_nodes = max_nodes;
    net->node_count = 0;
    net->learning_rate = 0.01f;
    net->is_training = 0;
    
    return net;
}

void huarong_net_destroy(HuarongTopologyNet* net) {
    if (!net) return;
    
    // 清理延迟释放的旧数组
    huarong_net_cleanup_retired(net);
    
    // 销毁所有节点
    for (int i = 0; i < net->node_count; i++) {
        destroy_reasoning_node(net->nodes[i]);
    }
    free(net->nodes);
    
    pthread_mutex_destroy(&net->mutex);
    for (int i = 0; i < 256; i++)
        pthread_mutex_destroy(&net->node_locks[i]);
    
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
 * 乐观锁：先无锁查哈希表，命中直接返回（O(1) 无竞争）
 * 未命中再持锁 double-check + 线性扫描 + 创建
 */
ReasoningNode* huarong_net_find_or_create_node(HuarongTopologyNet* net,
                                               const char* concept,
                                               float* features,
                                               int feature_dim,
                                               NodeHashTable* hash) {
    if (!net || !concept) return NULL;

    /* 快速路径：无锁查哈希表（99%+ 的调用命中此路径） */
    if (hash) {
        ReasoningNode* existing = node_hash_find(hash, concept);
        if (existing) return existing;
    }

    /* 慢速路径：持锁 double-check */
    pthread_mutex_lock(&net->mutex);

    // 再查一次哈希表（其他线程可能在此间隙创建了）
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

/* ================================================================
 *  节点连接哈希表 — 开放寻址 O(1) 查找目标节点在 connections[] 中的位置
 *
 *  key = target 节点指针, value = connections[] 数组下标。
 *  桶数为 2 的幂，用位与替代取模。
 * ================================================================ */

/* 延迟释放条目 — 前置声明供哈希表扩容使用 */
typedef struct RetiredArrays {
    void* conns;
    void* weights;
    void* bias;
    void* conf;
    struct RetiredArrays* next;
} RetiredArrays;

#define CONN_HASH_INIT_SIZE 16    /* 初始桶数 (2^n) */
#define CONN_HASH_MAX_LOAD 70     /* 负载因子达到 70% 时扩容 */

/** 内部实现 — 开放寻址查找，-1=未找到 */
int node_conn_hash_lookup(ReasoningNode* node, ReasoningNode* target) {
    if (!node || !node->conn_hash || node->conn_hash_mask < 0) return -1;
    uintptr_t h = ((uintptr_t)target >> 3);
    int mask = node->conn_hash_mask;
    for (int i = 0; i <= mask; i++) {
        int idx = (int)((h + (uintptr_t)i) & (uintptr_t)mask);
        if (node->conn_hash[idx].target == target)
            return node->conn_hash[idx].index;
        if (!node->conn_hash[idx].target)
            return -1;  /* 空槽 = 不存在 */
    }
    return -1;
}

int node_conn_hash_insert(HuarongTopologyNet* net, ReasoningNode* node,
                           ReasoningNode* target, int index) {
    if (!node || !target || index < 0) return -1;
    if (!node->conn_hash || node->conn_hash_mask < 0) {
        int sz = CONN_HASH_INIT_SIZE;
        node->conn_hash = (ConnHashEntry*)calloc((size_t)sz, sizeof(ConnHashEntry));
        if (!node->conn_hash) return -1;
        node->conn_hash_mask = sz - 1;
        node->conn_hash_entries = 0;
    }

    /* 扩容检查 */
    int cap = node->conn_hash_mask + 1;
    if (node->conn_hash_entries * 100 / cap >= CONN_HASH_MAX_LOAD) {
        int new_cap = cap * 2;
        ConnHashEntry* old_hash = node->conn_hash;
        ConnHashEntry* nht = (ConnHashEntry*)calloc((size_t)new_cap, sizeof(ConnHashEntry));
        if (!nht) return -1;

        int new_mask = new_cap - 1;
        for (int j = 0; j < cap; j++) {
            if (!old_hash[j].target) continue;
            uintptr_t h = ((uintptr_t)old_hash[j].target >> 3);
            for (int k = 0; k < new_cap; k++) {
                int nidx = (int)((h + (uintptr_t)k) & (uintptr_t)new_mask);
                if (!nht[nidx].target) {
                    nht[nidx] = old_hash[j];
                    break;
                }
            }
        }
        node->conn_hash = nht;
        node->conn_hash_mask = new_mask;

        /* 旧哈希表延迟释放（与连接数组同样策略） */
        if (net) {
            RetiredArrays* ra = (RetiredArrays*)malloc(sizeof(RetiredArrays));
            if (ra) {
                ra->conns   = old_hash;  /* 复用 conns 字段存旧哈希 */
                ra->weights = NULL;
                ra->bias    = NULL;
                ra->conf    = NULL;
                ra->next    = (RetiredArrays*)net->retired_conns;
                net->retired_conns  = ra;
                net->retired_pending = 1;
            }
        }
    }

    /* 插入 */
    uintptr_t h = ((uintptr_t)target >> 3);
    int mask = node->conn_hash_mask;
    for (int i = 0; i <= mask; i++) {
        int idx = (int)((h + (uintptr_t)i) & (uintptr_t)mask);
        if (node->conn_hash[idx].target == target)
            return 0;  /* 已存在 */
        if (!node->conn_hash[idx].target) {
            node->conn_hash[idx].target = target;
            node->conn_hash[idx].index  = index;
            node->conn_hash_entries++;
            return 0;
        }
    }
    return -1;  /* 表满了 */
}

void node_conn_hash_free(ReasoningNode* node) {
    if (!node || !node->conn_hash) return;
    free(node->conn_hash);
    node->conn_hash = NULL;
    node->conn_hash_mask = -1;
    node->conn_hash_entries = 0;
}

/* ================================================================
 *  延迟释放 — epoch 边界统一清理扩容旧数组
 *
 *  训练时 add_connection 扩容不 free 旧数组，而是挂到 net 的链表上。
 *  等 epoch 结束（所有线程都跑完），调用 cleanup 统一释放。
 *  这保证了训练线程的 boost_connection_weighted 原子操作不会
 *  碰到已释放的悬空指针。
 * ================================================================ */

void huarong_net_cleanup_retired(HuarongTopologyNet* net) {
    if (!net) return;
    RetiredArrays* list = (RetiredArrays*)net->retired_conns;
    while (list) {
        RetiredArrays* next = list->next;
        free(list->conns);
        free(list->weights);
        free(list->bias);
        free(list->conf);
        free(list);
        list = next;
    }
    net->retired_conns  = NULL;
    net->retired_pending = 0;
}

int huarong_net_add_connection(HuarongTopologyNet* net,
                              int from_node_id,
                              int to_node_id,
                              float weight) {
    if (!net || from_node_id < 0 || from_node_id >= net->node_count ||
        to_node_id < 0 || to_node_id >= net->node_count) {
        return -1;
    }

    int li = from_node_id & (PM_NODE_LOCK_COUNT - 1);
    pthread_mutex_lock(&net->node_locks[li]);

    ReasoningNode* from_node = net->nodes[from_node_id];
    ReasoningNode* to_node = net->nodes[to_node_id];

    if (!from_node || !to_node) { pthread_mutex_unlock(&net->node_locks[li]); return -1; }

    // 检查连接是否已存在（O(1) 哈希查找）
    int idx = node_conn_find(from_node, to_node);
    if (idx >= 0) {
        from_node->connection_weights[idx] = weight;
        pthread_mutex_unlock(&net->node_locks[li]);
        return 0;
    }

    // 检查容量，必要时扩容
    if (from_node->connection_count >= from_node->connection_capacity) {
        /* 防止整数溢出 */
        if (from_node->connection_capacity > INT_MAX / 2) {
            pthread_mutex_unlock(&net->node_locks[li]);
            return -1;
        }
        int new_cap = from_node->connection_capacity * 2;

        ReasoningNode** new_conn = (ReasoningNode**)malloc(new_cap * sizeof(ReasoningNode*));
        float* new_weights = (float*)malloc(new_cap * sizeof(float));
        float* new_bias = (float*)malloc(new_cap * sizeof(float));
        float* new_conf = (float*)malloc(new_cap * sizeof(float));

        if (!new_conn || !new_weights || !new_bias || !new_conf) {
            free(new_conn); free(new_weights); free(new_bias); free(new_conf);
            pthread_mutex_unlock(&net->node_locks[li]);
            return -1;
        }

        memcpy(new_conn, from_node->connections, from_node->connection_capacity * sizeof(ReasoningNode*));
        memcpy(new_weights, from_node->connection_weights, from_node->connection_capacity * sizeof(float));
        memcpy(new_bias, from_node->connection_motivational_bias, from_node->connection_capacity * sizeof(float));
        memcpy(new_conf, from_node->connection_confidences, from_node->connection_capacity * sizeof(float));

        /* 旧数组不立即 free：训练线程可能在无锁读取。
         * 挂到 net 的延迟释放链表，epoch 结束时统一清理。 */
        RetiredArrays* ra = (RetiredArrays*)malloc(sizeof(RetiredArrays));
        if (ra) {
            ra->conns   = from_node->connections;
            ra->weights = from_node->connection_weights;
            ra->bias    = from_node->connection_motivational_bias;
            ra->conf    = from_node->connection_confidences;
            ra->next    = (RetiredArrays*)net->retired_conns;
            net->retired_conns  = ra;
            net->retired_pending = 1;
        } else {
            /* 极端情况 malloc 失败：只能立即 free */
            free(from_node->connections);
            free(from_node->connection_weights);
            free(from_node->connection_motivational_bias);
            free(from_node->connection_confidences);
        }

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
    node_conn_hash_insert(net, from_node, to_node, from_node->connection_count);
    from_node->connection_count++;

    pthread_mutex_unlock(&net->node_locks[li]);
    return 0;
}

// 添加双向连接（用节点级锁替代全局锁，允许并行双向建边）
int huarong_net_add_bidirectional_connection(HuarongTopologyNet* net,
                                              int node_a_id, int node_b_id,
                                              float weight) {
    if (!net || node_a_id < 0 || node_a_id >= net->node_count ||
        node_b_id < 0 || node_b_id >= net->node_count) return -1;

    /* 节点级锁，按ID排序避免死锁 */
    int li_a = node_a_id & (PM_NODE_LOCK_COUNT - 1);
    int li_b = node_b_id & (PM_NODE_LOCK_COUNT - 1);
    if (li_a == li_b) {
        pthread_mutex_lock(&net->node_locks[li_a]);
    } else if (li_a < li_b) {
        pthread_mutex_lock(&net->node_locks[li_a]);
        pthread_mutex_lock(&net->node_locks[li_b]);
    } else {
        pthread_mutex_lock(&net->node_locks[li_b]);
        pthread_mutex_lock(&net->node_locks[li_a]);
    }
    ReasoningNode* a = net->nodes[node_a_id];
    ReasoningNode* b = net->nodes[node_b_id];
    if (!a || !b) {
        pthread_mutex_unlock(&net->node_locks[li_a]);
        if (li_a != li_b) pthread_mutex_unlock(&net->node_locks[li_b]);
        return -1;
    }

    /* 检查 A→B 是否已存在 */
    int a_has_b = -1;
    for (int i = 0; i < a->connection_count; i++) {
        if (a->connections[i] == b) { a_has_b = i; break; }
    }
    /* 检查 B→A 是否已存在 */
    int b_has_a = -1;
    for (int i = 0; i < b->connection_count; i++) {
        if (b->connections[i] == a) { b_has_a = i; break; }
    }

    /* 原子地建立两个方向 */
    if (a_has_b < 0) {
        if (a->connection_count >= a->connection_capacity) {
            pthread_mutex_unlock(&net->node_locks[li_a]);
            if (li_a != li_b) pthread_mutex_unlock(&net->node_locks[li_b]);
            return -1;
        }
        a->connections[a->connection_count] = b;
        a->connection_weights[a->connection_count] = weight;
        a->connection_motivational_bias[a->connection_count] = 0.5f;
        a->connection_confidences[a->connection_count] = 0.5f;
        a->connection_count++;
    } else {
        a->connection_weights[a_has_b] = weight;
    }
    if (b_has_a < 0) {
        if (b->connection_count >= b->connection_capacity) {
            pthread_mutex_unlock(&net->node_locks[li_a]);
            if (li_a != li_b) pthread_mutex_unlock(&net->node_locks[li_b]);
            return -1;
        }
        b->connections[b->connection_count] = a;
        b->connection_weights[b->connection_count] = weight;
        b->connection_motivational_bias[b->connection_count] = 0.5f;
        b->connection_confidences[b->connection_count] = 0.5f;
        b->connection_count++;
    } else {
        b->connection_weights[b_has_a] = weight;
    }

    pthread_mutex_unlock(&net->node_locks[li_a]);
    if (li_a != li_b) pthread_mutex_unlock(&net->node_locks[li_b]);
    return 0;
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
    if (net->node_count == 0) { *path_length = 0; return NULL; }

    pthread_mutex_lock(&net->mutex);
    int nc = net->node_count;

    // 预建节点指针→索引查找表
    NodeIdxLookup* lookup = (NodeIdxLookup*)malloc(nc * sizeof(NodeIdxLookup));
    if (!lookup) { pthread_mutex_unlock(&net->mutex); return NULL; }
    for (int i = 0; i < nc; i++) {
        lookup[i].ptr = net->nodes[i];
        lookup[i].idx = i;
    }
    qsort(lookup, nc, sizeof(NodeIdxLookup), node_idx_cmp);

    int* sorted_nodes = (int*)malloc(nc * sizeof(int));
    int* in_degree = (int*)calloc(nc, sizeof(int));

    if (!sorted_nodes || !in_degree) {
        free(sorted_nodes); free(in_degree); free(lookup);
        pthread_mutex_unlock(&net->mutex); return NULL;
    }

    // 计算每个节点的入度 O(N+E)
    for (int i = 0; i < nc; i++) {
        ReasoningNode* node = net->nodes[i];
        for (int j = 0; j < node->connection_count; j++) {
            int k = lookup_node_by_ptr(lookup, net->node_count, node->connections[j]);
            if (k >= 0) in_degree[k]++;
        }
    }

    // Kahn算法（队列实现，O(N+E) 替代原线性扫描 O(N²)）
    int* queue = (int*)malloc(nc * sizeof(int));
    if (!queue) {
        free(sorted_nodes); free(in_degree); free(lookup);
        pthread_mutex_unlock(&net->mutex); return NULL;
    }
    int front = 0, rear = 0;

    for (int i = 0; i < nc; i++) {
        if (in_degree[i] == 0) queue[rear++] = i;
    }

    int sorted_count = 0;
    while (front < rear) {
        int i = queue[front++];
        sorted_nodes[sorted_count++] = i;

        ReasoningNode* node = net->nodes[i];
        for (int j = 0; j < node->connection_count; j++) {
            int k = lookup_node_by_ptr(lookup, nc, node->connections[j]);
            if (k >= 0 && --in_degree[k] == 0) {
                queue[rear++] = k;
            }
        }
    }

    pthread_mutex_unlock(&net->mutex);

    if (sorted_count != nc) {
        printf("警告：网络中存在环，无法进行拓扑排序\n");
    }

    *path_length = sorted_count;

    free(queue);
    free(lookup);
    free(in_degree);

    return sorted_nodes;
}

// ==================== 动态网络操作 ==================== 

int huarong_net_dynamic_add_node(HuarongTopologyNet* net,
                                const char* concept,
                                float* features,
                                int feature_dim) {
    if (!net || net->node_count >= net->max_nodes) return -1;
    
    ReasoningNode* new_node = huarong_net_add_node(net, concept, features, feature_dim);
    return new_node ? new_node->node_id : -1;
}

int huarong_net_dynamic_remove_node(HuarongTopologyNet* net, int node_id) {
    if (!net || node_id < 0 || node_id >= net->node_count) return -1;
    
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
    
    /* 数组压缩必须在锁内完成，防止并发读取者看到部分移动的数组 */
    for (int i = node_id; i < net->node_count - 1; i++) {
        net->nodes[i] = net->nodes[i + 1];
        net->nodes[i]->node_id = i;
    }
    net->nodes[net->node_count - 1] = NULL;
    net->node_count--;

    pthread_mutex_unlock(&net->mutex);
    
    /* 销毁节点在锁外进行（不涉及共享状态） */
    destroy_reasoning_node(victim);
    
    return 0;
}

// ==================== 网络优化 ==================== 

void huarong_net_optimize(HuarongTopologyNet* net) {
    if (!net) return;
    
    float threshold = 0.01f;
    
    pthread_mutex_lock(&net->mutex);
    
    for (int i = 0; i < net->node_count; i++) {
        ReasoningNode* node = net->nodes[i];
        if (!node) continue;
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

    pthread_mutex_unlock(&net->mutex);
    
    printf("网络优化完成：移除了权重小于%.3f的冗余连接\n", threshold);
}

int huarong_net_prune_edges(HuarongTopologyNet* net, float min_confidence, float min_weight) {
    if (!net) return 0;
    int total_pruned = 0;

    /* 分批处理：每批在锁内重新读取 node_count 防止并发删除导致的越界 */
    int batch_start = 0;
    while (1) {
        pthread_mutex_lock(&net->mutex);
        int nc = net->node_count;
        if (batch_start >= nc) { pthread_mutex_unlock(&net->mutex); break; }
        int batch_end = batch_start + PM_PRUNE_BATCH_SIZE;
        if (batch_end > nc) batch_end = nc;

        for (int i = batch_start; i < batch_end; i++) {
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

        pthread_mutex_unlock(&net->mutex);
        batch_start = batch_end;
    }

    if (total_pruned > 0)
        printf("[拓扑剪枝] 移除 %d 条低质量边 (min_conf=%.3f, min_weight=%.3f)\n",
               total_pruned, min_confidence, min_weight);
    return total_pruned;
}