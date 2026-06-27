#include "huarong_topology.h"
#include "node_hash.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

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
    node->edge_count = 0;
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
    
    // 初始化连接数组（预分配 DEFAULT_CONNECTION_CAPACITY 个 Edge 空间）
    node->edges = (Edge*)calloc(DEFAULT_CONNECTION_CAPACITY, sizeof(Edge));
    node->edge_capacity = DEFAULT_CONNECTION_CAPACITY;

    if (!node->edges) {
        free(node->concept);
        free(node->features);
        if (node->cognitive_confidence)
            cognitive_confidence_destroy(node->cognitive_confidence);
        free(node);
        return NULL;
    }

    // 初始化边属性
    for (int i = 0; i < DEFAULT_CONNECTION_CAPACITY; i++) {
        node->edges[i].weight = 0.5f;
        node->edges[i].motivational_bias = 0.5f;
        node->edges[i].confidence = 0.5f;
    }
    
    return node;
}

void destroy_reasoning_node(ReasoningNode* node) {
    if (!node) return;
    
    free(node->concept);
    free(node->features);
    free(node->edges);           /* 单次 free 释放整个 Edge 数组 */
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
    for (int i = 0; i < PM_NODE_LOCK_COUNT; i++)
        pthread_mutex_init(&net->node_locks[i], NULL);
    pthread_mutex_init(&net->retire_mutex, NULL);
    net->retired_conns   = NULL;
    net->retired_pending = 0;
    net->epoch           = 0;
    net->active_readers  = 0;
    
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
    if (net->concept_hash) {
        int cap = net->concept_hash_mask + 1;
        for (int i = 0; i < cap; i++)
            free((void*)net->concept_hash[i].name);
    }
    free(net->concept_hash);
    // 清理延迟释放的旧数组
    huarong_net_cleanup_retired(net);
    
    // 销毁所有节点
    for (int i = 0; i < net->node_count; i++) {
        destroy_reasoning_node(net->nodes[i]);
    }
    free(net->nodes);
    
    pthread_mutex_destroy(&net->mutex);
    for (int i = 0; i < PM_NODE_LOCK_COUNT; i++)
        pthread_mutex_destroy(&net->node_locks[i]);
    pthread_mutex_destroy(&net->retire_mutex);
    
    free(net);
}

/* ================================================================
 *  概念哈希表 — O(1) concept→node_id 查找
 * ================================================================ */
#define CHASH_INIT_CAP 131072  /* 2^17, 覆盖67100节点 */

static unsigned int _chash_djb2(const char* s) {
    unsigned int h = 5381;
    while (*s) h = ((h << 5) + h) + (unsigned char)*s++;
    return h;
}

static void _concept_hash_insert(HuarongTopologyNet* net, const char* name, int nid) {
    if (!net || !name) return;
    /* 惰性初始化 */
    if (!net->concept_hash) {
        int cap = CHASH_INIT_CAP;
        net->concept_hash = (typeof(net->concept_hash))calloc(cap, sizeof(*net->concept_hash));
        if (!net->concept_hash) return;
        net->concept_hash_mask = cap - 1;
    }
    /* 负载因子 > 0.6 则扩容 */
    if (net->concept_hash_count > (net->concept_hash_mask + 1) * 6 / 10) {
        int old_cap = net->concept_hash_mask + 1;
        int new_cap = old_cap * 2;
        typeof(net->concept_hash) nu = (typeof(nu))calloc(new_cap, sizeof(*nu));
        if (!nu) return;
        int new_mask = new_cap - 1;
        for (int i = 0; i < old_cap; i++) {
            if (!net->concept_hash[i].name) continue;
            unsigned int h = _chash_djb2(net->concept_hash[i].name) & (unsigned)new_mask;
            while (nu[h].name) h = (h + 1) & new_mask;
            nu[h] = net->concept_hash[i];
        }
        free(net->concept_hash);
        net->concept_hash = nu;
        net->concept_hash_mask = new_mask;
    }
    unsigned int h = _chash_djb2(name) & (unsigned)net->concept_hash_mask;
    while (net->concept_hash[h].name) {
        if (strcmp(net->concept_hash[h].name, name) == 0) {
            net->concept_hash[h].node_id = nid; return;  /* 更新 */
        }
        h = (h + 1) & (unsigned)net->concept_hash_mask;
    }
    net->concept_hash[h].name = strdup(name);  /* 哈希表接管字符串所有权，防御调用方 realloc/free */
    if (!net->concept_hash[h].name) return;
    net->concept_hash[h].node_id = nid;
    net->concept_hash_count++;
}

int huarong_net_find_concept(HuarongTopologyNet* net, const char* concept) {
    if (!net || !net->concept_hash || !concept) return -1;
    /* 持锁防止与 _concept_hash_insert 扩容（free+realloc）并发 */
    pthread_mutex_lock(&net->mutex);
    unsigned int mask = (unsigned)net->concept_hash_mask;
    unsigned int h = _chash_djb2(concept) & mask;
    int ret = -1;
    while (net->concept_hash[h].name) {
        if (strcmp(net->concept_hash[h].name, concept) == 0) {
            ret = net->concept_hash[h].node_id;
            break;
        }
        h = (h + 1) & mask;
    }
    pthread_mutex_unlock(&net->mutex);
    return ret;
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
        /* 自动注册到概念哈希表 */
        _concept_hash_insert(net, concept, node_id);
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

/* 延迟释放条目 — Edge 数组旧版本暂存 */
typedef struct RetiredArrays {
    void* old_edges;                    /* 旧 Edge* 数组 */
    struct RetiredArrays* next;
} RetiredArrays;

#define CONN_HASH_INIT_SIZE 16    /* 初始桶数 (2^n) */
#define CONN_HASH_MAX_LOAD 70     /* 负载因子达到 70% 时扩容 */

/** 内部实现 — 开放寻址查找（含墓碑跳过），-1=未找到 */
int node_conn_hash_lookup(ReasoningNode* node, ReasoningNode* target) {
    if (!node || !node->conn_hash || node->conn_hash_mask < 0) return -1;
    uintptr_t h = ((uintptr_t)target >> 3);
    int mask = node->conn_hash_mask;
    for (int i = 0; i <= mask; i++) {
        int idx = (int)((h + (uintptr_t)i) & (uintptr_t)mask);
        if (node->conn_hash[idx].target == target && !node->conn_hash[idx].is_deleted)
            return node->conn_hash[idx].index;
        if (!node->conn_hash[idx].target && !node->conn_hash[idx].is_deleted)
            return -1;  /* 真空槽 = 不存在 */
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
            if (!old_hash[j].target || old_hash[j].is_deleted) continue;  /* 跳过空槽和墓碑 */
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

        /* 旧哈希表延迟释放（与 Edge 数组同样策略，需持 retire_mutex 防竞态） */
        if (net) {
            RetiredArrays* ra = (RetiredArrays*)malloc(sizeof(RetiredArrays));
            if (ra) {
                ra->old_edges = old_hash;  /* 复用 old_edges 字段存旧哈希 */
                pthread_mutex_lock(&net->retire_mutex);
                ra->next    = (RetiredArrays*)net->retired_conns;
                net->retired_conns  = ra;
                net->retired_pending = 1;
                pthread_mutex_unlock(&net->retire_mutex);
            }
        }
    }

    /* 插入：优先复用墓碑槽，其次找真空槽 */
    int tombstone_idx = -1;
    uintptr_t h = ((uintptr_t)target >> 3);
    int mask = node->conn_hash_mask;
    for (int i = 0; i <= mask; i++) {
        int idx = (int)((h + (uintptr_t)i) & (uintptr_t)mask);
        if (node->conn_hash[idx].target == target && !node->conn_hash[idx].is_deleted)
            return 0;  /* 已存在 */
        if (!node->conn_hash[idx].target && !node->conn_hash[idx].is_deleted) {
            /* 真空槽：占用即终止 */
            if (tombstone_idx < 0) {
                node->conn_hash[idx].target = target;
                node->conn_hash[idx].index  = index;
                node->conn_hash[idx].is_deleted = 0;
                node->conn_hash_entries++;
            }
            return 0;
        }
        if (node->conn_hash[idx].is_deleted && tombstone_idx < 0)
            tombstone_idx = idx;  /* 记住第一个墓碑槽 */
    }
    /* 表满但找到墓碑槽 → 复用 */
    if (tombstone_idx >= 0) {
        node->conn_hash[tombstone_idx].target = target;
        node->conn_hash[tombstone_idx].index  = index;
        node->conn_hash[tombstone_idx].is_deleted = 0;
        node->conn_hash_entries++;
        return 0;
    }
    return -1;  /* 真满了 */
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
 *  训练时 add_connection 扩容不立即 free 旧数组，而是：
 *   1) 先 swap 活指针 (from_node->edges = new_edges)
 *   2) 再将旧数组挂到 net 的退役链表上
 *   3) epoch 结束时由调用方统一 cleanup_retired 释放
 *
 *  安全保证来自三步：
 *   a) swap-before-retire 时序：活指针在 retire 之前已切换到新数组，
 *      故此间任何读线程通过 from_node->edges 访问的都是新数组。
 *   b) 调用方纪律：cleanup 只在 epoch 间隙（所有训练线程已返回）
 *      或 net 销毁时调用，不与 add_connection 并发。
 *   c) EBR 安全门：cleanup 检查 active_readers > 0 时推迟释放，
 *      防止读线程持有 from_node->edges 快照期间旧数组被 free。
 * ================================================================ */

void huarong_net_cleanup_retired(HuarongTopologyNet* net) {
    if (!net) return;
    if (!net->retired_pending) return;  /* 无待释放对象 */

    pthread_mutex_lock(&net->retire_mutex);
    /* double-check：持锁后再次确认（防止 unlock→lock 之间被其他线程清空） */
    if (!net->retired_conns) {
        net->retired_pending = 0;
        pthread_mutex_unlock(&net->retire_mutex);
        return;
    }
    /* EBR 安全门：有活跃读线程时推迟清理，防止 use-after-free。
     * 保持 retired_pending=1，下次 leave_reader 触发或 net 销毁时重试。 */
    if (net->active_readers > 0) {
        pthread_mutex_unlock(&net->retire_mutex);
        return;
    }
    RetiredArrays* list = (RetiredArrays*)net->retired_conns;
    net->retired_conns = NULL;
    net->retired_pending = 0;
    net->epoch++;  /* 推进 epoch，退役对象所属旧代彻底终结 */
    pthread_mutex_unlock(&net->retire_mutex);

    while (list) {
        RetiredArrays* next = list->next;
        free(list->old_edges);
        free(list);
        list = next;
    }
}

/** 将任意堆指针挂入退役链表，epoch 结束时安全释放（防并发 use-after-free） */
void huarong_net_retire_blob(HuarongTopologyNet* net, void* ptr) {
    if (!net || !ptr) return;
    RetiredArrays* ra = (RetiredArrays*)malloc(sizeof(RetiredArrays));
    if (!ra) { free(ptr); return; }
    ra->old_edges = ptr;
    pthread_mutex_lock(&net->retire_mutex);
    ra->next = (void*)net->retired_conns;
    net->retired_conns = ra;
    net->retired_pending = 1;
    pthread_mutex_unlock(&net->retire_mutex);
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
        from_node->edges[idx].weight = clamp(weight, 0.0f, 1.0f);
        pthread_mutex_unlock(&net->node_locks[li]);
        return 0;
    }

    // 检查容量，必要时扩容
    if (from_node->edge_count >= from_node->edge_capacity) {
        /* 硬上限：防止单节点连接爆炸导致 OOM（来自 constants.h） */
        if (from_node->edge_capacity >= PM_AUTONOMIC_MAX_CONN) {
            pthread_mutex_unlock(&net->node_locks[li]);
            return -1;
        }
        /* 防止整数溢出 */
        if (from_node->edge_capacity > INT_MAX / 2) {
            pthread_mutex_unlock(&net->node_locks[li]);
            return -1;
        }
        int new_cap = from_node->edge_capacity * 2;
        /* 扩容后不得超过硬上限 */
        if (new_cap > PM_AUTONOMIC_MAX_CONN)
            new_cap = PM_AUTONOMIC_MAX_CONN;

        Edge* new_edges = (Edge*)malloc(new_cap * sizeof(Edge));

        if (!new_edges) {
            pthread_mutex_unlock(&net->node_locks[li]);
            return -1;
        }

        memcpy(new_edges, from_node->edges, from_node->edge_capacity * sizeof(Edge));

        /* 初始化新扩容的槽位（此时 new_edges 尚未挂到 from_node->edges，
         * 但 from_node->edge_capacity 仍是旧值，用 new_cap 作为上限）。 */
        for (int i = from_node->edge_capacity; i < new_cap; i++) {
            new_edges[i].target = NULL;
            new_edges[i].weight = 0.5f;
            new_edges[i].motivational_bias = 0.5f;
            new_edges[i].confidence = 0.5f;
        }

        /* ★ 先 swap 活指针，再 retire 旧数组。
         * 此后 from_node->edges 永远指向新数组，读线程安全。 */
        Edge* old_edges_save = from_node->edges;
        from_node->edges = new_edges;
        from_node->edge_capacity = new_cap;

        /* 旧数组延迟释放：训练线程可能在无锁读取 from_node->edges，
         * 但此时已 swap，即使旧数组被 cleanup_retired 立即 free，
         * 也不存在活指针指向它。 */
        RetiredArrays* ra = (RetiredArrays*)malloc(sizeof(RetiredArrays));
        if (ra) {
            ra->old_edges = old_edges_save;
            pthread_mutex_lock(&net->retire_mutex);
            ra->next    = (RetiredArrays*)net->retired_conns;
            net->retired_conns  = ra;
            net->retired_pending = 1;
            pthread_mutex_unlock(&net->retire_mutex);
        } else {
            /* 极端情况 malloc 失败：只能立即 free。
             * 安全：from_node->edges 已指向 new_edges，没有活引用。 */
            free(old_edges_save);
        }
    }

    // 添加新连接
    from_node->edges[from_node->edge_count].target = to_node;
    from_node->edges[from_node->edge_count].weight = clamp(weight, 0.0f, 1.0f);
    from_node->edges[from_node->edge_count].motivational_bias = 0.5f;
    from_node->edges[from_node->edge_count].confidence = 0.5f;
    node_conn_hash_insert(net, from_node, to_node, from_node->edge_count);
    from_node->edge_count++;

    pthread_mutex_unlock(&net->node_locks[li]);
    return 0;
}

// 添加双向连接（用节点级锁替代全局锁，允许并行双向建边）
int huarong_net_add_bidirectional_connection(HuarongTopologyNet* net,
                                              int node_a_id, int node_b_id,
                                              float weight) {
    if (!net || node_a_id < 0 || node_a_id >= net->node_count ||
        node_b_id < 0 || node_b_id >= net->node_count) return -1;

    /* 节点级锁，按ID排序避免死锁（使用封装的辅助函数） */
    int locked_both = 0;
    lock_two_nodes_by_id(net, node_a_id, node_b_id, &locked_both);
    ReasoningNode* a = net->nodes[node_a_id];
    ReasoningNode* b = net->nodes[node_b_id];
    if (!a || !b) {
        unlock_two_nodes_by_id(net, node_a_id, node_b_id, locked_both);
        return -1;
    }

    /* 检查 A→B 是否已存在 */
    int a_has_b = -1;
    for (int i = 0; i < a->edge_count; i++) {
        if (a->edges[i].target == b) { a_has_b = i; break; }
    }
    /* 检查 B→A 是否已存在 */
    int b_has_a = -1;
    for (int i = 0; i < b->edge_count; i++) {
        if (b->edges[i].target == a) { b_has_a = i; break; }
    }

    /* 原子地建立两个方向 */
    if (a_has_b < 0) {
        if (a->edge_count >= a->edge_capacity) {
            unlock_two_nodes_by_id(net, node_a_id, node_b_id, locked_both);
            return -1;
        }
        a->edges[a->edge_count].target = b;
        a->edges[a->edge_count].weight = clamp(weight, 0.0f, 1.0f);
        a->edges[a->edge_count].motivational_bias = 0.5f;
        a->edges[a->edge_count].confidence = 0.5f;
        a->edge_count++;
    } else {
        a->edges[a_has_b].weight = clamp(weight, 0.0f, 1.0f);
    }
    if (b_has_a < 0) {
        if (b->edge_count >= b->edge_capacity) {
            unlock_two_nodes_by_id(net, node_a_id, node_b_id, locked_both);
            return -1;
        }
        b->edges[b->edge_count].target = a;
        b->edges[b->edge_count].weight = clamp(weight, 0.0f, 1.0f);
        b->edges[b->edge_count].motivational_bias = 0.5f;
        b->edges[b->edge_count].confidence = 0.5f;
        b->edge_count++;
    } else {
        b->edges[b_has_a].weight = clamp(weight, 0.0f, 1.0f);
    }

    unlock_two_nodes_by_id(net, node_a_id, node_b_id, locked_both);
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
        for (int j = 0; j < node->edge_count; j++) {
            int k = lookup_node_by_ptr(lookup, net->node_count, node->edges[j].target);
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
        for (int j = 0; j < node->edge_count; j++) {
            int k = lookup_node_by_ptr(lookup, nc, node->edges[j].target);
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
        for (int c = 0; c < other->edge_count; c++) {
            if (other->edges[c].target != victim) {
                if (kept != c) {
                    other->edges[kept].target = other->edges[c].target;
                    other->edges[kept].weight = other->edges[c].weight;
                    other->edges[kept].motivational_bias = other->edges[c].motivational_bias;
                    other->edges[kept].confidence = other->edges[c].confidence;
                }
                kept++;
            }
        }
        other->edge_count = kept;
        /* 边压缩后重建 conn_hash，索引已变化。
         * 先置 NULL 再 free，防止无锁读 node_conn_hash_lookup 踩悬空指针 */
        {
            void* old_hash = other->conn_hash;
            other->conn_hash = NULL;
            other->conn_hash_mask = -1;
            other->conn_hash_entries = 0;
            free(old_hash);
        }
        for (int ci = 0; ci < other->edge_count; ci++) {
            if (other->edges[ci].target)
                node_conn_hash_insert(NULL, other, other->edges[ci].target, ci);
        }
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
        int new_edge_count = 0;
        
        for (int j = 0; j < node->edge_count; j++) {
            if (fabs(node->edges[j].weight) > threshold) {
                node->edges[new_edge_count].target = node->edges[j].target;
                node->edges[new_edge_count].weight = node->edges[j].weight;
                if (node->edges)
                    node->edges[new_edge_count].motivational_bias = node->edges[j].motivational_bias;
                if (node->edges)
                    node->edges[new_edge_count].confidence = node->edges[j].confidence;
                new_edge_count++;
            }
        }
        
        node->edge_count = new_edge_count;
        /* 边压缩后重建 conn_hash，索引已变化。
         * 先置 NULL 再 free，防止无锁读 node_conn_hash_lookup 踩悬空指针 */
        {
            void* old_hash = node->conn_hash;
            node->conn_hash = NULL;
            node->conn_hash_mask = -1;
            node->conn_hash_entries = 0;
            free(old_hash);
        }
        for (int ci = 0; ci < node->edge_count; ci++) {
            if (node->edges[ci].target)
                node_conn_hash_insert(NULL, node, node->edges[ci].target, ci);
        }
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

            for (int j = 0; j < node->edge_count; j++) {
                float conf = node->edges[j].confidence;
                float w = node->edges[j].weight;
                // 保留条件：置信度够高 或 权重大
                if (conf >= min_confidence || fabsf(w) >= min_weight) {
                    if (kept != j) {
                        node->edges[kept] = node->edges[j];  /* 整体拷贝 Edge */
                    }
                    kept++;
                } else {
                    total_pruned++;
                }
            }
            node->edge_count = kept;
            /* 边压缩后重建 conn_hash，索引已变化。
             * 先置 NULL 再 free，防止无锁读 node_conn_hash_lookup 踩悬空指针 */
            {
                void* old_hash = node->conn_hash;
                node->conn_hash = NULL;
                node->conn_hash_mask = -1;
                node->conn_hash_entries = 0;
                free(old_hash);
            }
            for (int ci = 0; ci < node->edge_count; ci++) {
                if (node->edges[ci].target)
                    node_conn_hash_insert(NULL, node, node->edges[ci].target, ci);
            }
        }

        pthread_mutex_unlock(&net->mutex);
        batch_start = batch_end;
    }

    if (total_pruned > 0)
        printf("[拓扑剪枝] 移除 %d 条低质量边 (min_conf=%.3f, min_weight=%.3f)\n",
               total_pruned, min_confidence, min_weight);
    return total_pruned;
}