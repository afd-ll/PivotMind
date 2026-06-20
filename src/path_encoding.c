/**
 * @file path_encoding.c
 * @brief 路径三元组频率表 — LRU 开放寻址哈希表 + 不可分解性分析
 */

#include "path_encoding.h"
#include "huarong_topology.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ================================================================
 *  哈希函数
 * ================================================================ */

static inline int hash_triplet(int a, int b, int c, int topo_id, int capacity) {
    unsigned int h = (unsigned int)a * 31U + (unsigned int)b * 17U
                   + (unsigned int)c * 7U  + (unsigned int)topo_id;
    return (int)(h % (unsigned int)capacity);
}

/* ================================================================
 *  PathFrequencyTable 生命周期
 * ================================================================ */

PathFrequencyTable* path_freq_table_create(int capacity) {
    if (capacity <= 0) return NULL;

    PathFrequencyTable* table = (PathFrequencyTable*)calloc(1, sizeof(PathFrequencyTable));
    if (!table) return NULL;

    table->buckets = (PathTripletRecord*)calloc((size_t)capacity, sizeof(PathTripletRecord));
    if (!table->buckets) {
        free(table);
        return NULL;
    }

    table->capacity    = capacity;
    table->entry_count = 0;
    table->total_triplets = 0;
    table->round       = 0;

    if (pthread_mutex_init(&table->mutex, NULL) != 0) {
        free(table->buckets);
        free(table);
        return NULL;
    }

    return table;
}

void path_freq_table_destroy(PathFrequencyTable* table) {
    if (!table) return;
    pthread_mutex_destroy(&table->mutex);
    free(table->buckets);
    free(table);
}

/* ================================================================
 *  记录三元组
 * ================================================================ */

int path_freq_table_record(PathFrequencyTable* table,
                           int topo_id, int a, int b, int c) {
    if (!table || !table->buckets || table->capacity <= 0) return -1;

    pthread_mutex_lock(&table->mutex);

    int cap = table->capacity;
    int idx = hash_triplet(a, b, c, topo_id, cap);

    /* 线性探测查找 */
    for (int probe = 0; probe < cap; probe++) {
        int cur = (idx + probe) % cap;
        PathTripletRecord* rec = &table->buckets[cur];

        if (rec->is_active) {
            /* 匹配已存在的记录 */
            if (rec->node_a  == a && rec->node_b  == b &&
                rec->node_c  == c && rec->topo_id == topo_id) {
                rec->count++;
                rec->last_seen = table->round;
                table->total_triplets++;
                int cnt = rec->count;
                pthread_mutex_unlock(&table->mutex);
                return cnt;
            }
        } else {
            /* 空槽 — 插入新记录 */
            rec->node_a   = a;
            rec->node_b   = b;
            rec->node_c   = c;
            rec->topo_id  = topo_id;
            rec->count    = 1;
            rec->last_seen = table->round;
            rec->is_active = 1;
            table->entry_count++;
            table->total_triplets++;
            table->round++;
            pthread_mutex_unlock(&table->mutex);
            return 1;
        }
    }

    /* 表满 — count 优先淘汰：驱逐 count 最小的条目（保留高频稳定模式）。
       同 count 时选 last_seen 最旧的作为 tiebreaker。 */
    int evict_idx     = -1;
    int evict_count   = INT_MAX;
    int evict_age     = -1;

    for (int probe = 0; probe < cap; probe++) {
        int cur = (idx + probe) % cap;
        PathTripletRecord* rec = &table->buckets[cur];
        int age = table->round - rec->last_seen;
        if (rec->count < evict_count ||
            (rec->count == evict_count && age > evict_age)) {
            evict_count = rec->count;
            evict_age   = age;
            evict_idx   = cur;
        }
    }

    if (evict_idx >= 0) {
        PathTripletRecord* rec = &table->buckets[evict_idx];
        rec->node_a    = a;
        rec->node_b    = b;
        rec->node_c    = c;
        rec->topo_id   = topo_id;
        rec->count     = 1;
        rec->last_seen = table->round;
        rec->is_active = 1;
        table->total_triplets++;
        table->round++;
        pthread_mutex_unlock(&table->mutex);
        return 1;
    }

    pthread_mutex_unlock(&table->mutex);
    return -1;  /* should not reach */
}

/* ================================================================
 *  查找
 * ================================================================ */

/**
 * 直接设置三元组计数（无锁，供状态恢复）
 */
int path_freq_table_set(PathFrequencyTable* table,
                        int topo_id, int a, int b, int c,
                        int count) {
    if (!table || !table->buckets || table->capacity <= 0) return -1;

    int cap = table->capacity;
    int idx = hash_triplet(a, b, c, topo_id, cap);

    /* 线性探测 — 无锁版本 */
    for (int probe = 0; probe < cap; probe++) {
        int cur = (idx + probe) % cap;
        PathTripletRecord* rec = &table->buckets[cur];

        if (rec->is_active) {
            if (rec->node_a  == a && rec->node_b  == b &&
                rec->node_c  == c && rec->topo_id == topo_id) {
                rec->count += count;
                rec->last_seen = table->round;
                return 1;
            }
        } else {
            rec->node_a    = a;
            rec->node_b    = b;
            rec->node_c    = c;
            rec->topo_id   = topo_id;
            rec->count     = count;
            rec->last_seen = table->round;
            rec->is_active = 1;
            table->entry_count++;
            table->total_triplets += count;
            return 1;
        }
    }

    return -1;
}

int path_freq_table_lookup(PathFrequencyTable* table,
                           int topo_id, int a, int b, int c,
                           int* count_out) {
    if (!table || !table->buckets || table->capacity <= 0) return -1;

    int cap = table->capacity;
    int idx = hash_triplet(a, b, c, topo_id, cap);

    for (int probe = 0; probe < cap; probe++) {
        int cur = (idx + probe) % cap;
        PathTripletRecord* rec = &table->buckets[cur];

        if (!rec->is_active) return 0;  /* empty slot: not found */

        if (rec->node_a  == a && rec->node_b  == b &&
            rec->node_c  == c && rec->topo_id == topo_id) {
            if (count_out) *count_out = rec->count;
            return 1;
        }
    }

    return 0;  /* table full and not found */
}

/* ================================================================
 *  迭代器
 * ================================================================ */

const PathTripletRecord* path_freq_table_iter(
    const PathFrequencyTable* table, int* iter) {
    if (!table || !table->buckets || !iter) return NULL;

    int cap = table->capacity;
    while (*iter < cap) {
        const PathTripletRecord* rec = &table->buckets[*iter];
        (*iter)++;
        if (rec->is_active) return rec;
    }
    return NULL;
}

/* ================================================================
 *  不可分解性分析
 *
 *  ir_ratio = P(abc) / (P(ab) * P(bc))
 *
 *  P(abc)  = triplet_count / total_triplets
 *  P(ab)   = edge_weight(a→b) / sum_outgoing_weights(a)
 *  P(bc)   = edge_weight(b→c) / sum_outgoing_weights(b)
 *
 *  仅返回 ir_ratio > 1.0 的结果
 * ================================================================ */

/* 辅助: 在 ReasoningNode 的边中查找目标节点并返回边权重 */
static float find_edge_weight(ReasoningNode* node, int target_id) {
    if (!node) return 0.0f;
    for (int i = 0; i < node->edge_count; i++) {
        ReasoningNode* conn = node->edges[i].target;
        if (conn && conn->node_id == target_id) {
            return node->edges[i].weight;
        }
    }
    return 0.0f;
}

/* 辅助: 计算节点的出边权重总和 */
static float sum_outgoing_weights(ReasoningNode* node) {
    if (!node) return 0.0f;
    float sum = 0.0f;
    for (int i = 0; i < node->edge_count; i++) {
        sum += node->edges[i].weight;
    }
    return sum;
}

static int ir_result_cmp_desc(const void* a, const void* b) {
    float ra = ((const IrreducibilityResult*)a)->ir_ratio;
    float rb = ((const IrreducibilityResult*)b)->ir_ratio;
    return (ra < rb) ? 1 : (ra > rb) ? -1 : 0;
}

IrreducibilityResult* path_analyze_irreducibility(
    PathFrequencyTable* table,
    void* const* nodes,
    int node_count,
    int* result_count) {
    if (!table || !nodes || node_count <= 0 || !result_count) return NULL;

    *result_count = 0;
    if (table->entry_count == 0) return NULL;

    /* 预分配 — 最多 entry_count 个结果 */
    IrreducibilityResult* results = (IrreducibilityResult*)malloc(
        (size_t)table->entry_count * sizeof(IrreducibilityResult));
    if (!results) return NULL;

    ReasoningNode* const* rnodes = (ReasoningNode* const*)nodes;
    int rc = 0;

    int iter = 0;
    const PathTripletRecord* rec;
    while ((rec = path_freq_table_iter(table, &iter)) != NULL) {
        int a = rec->node_a;
        int b = rec->node_b;
        int c = rec->node_c;

        /* 节点范围校验 */
        if (a < 0 || a >= node_count || b < 0 || b >= node_count || c < 0 || c >= node_count)
            continue;

        ReasoningNode* na = rnodes[a];
        ReasoningNode* nb = rnodes[b];
        if (!na || !nb) continue;

        /* edge weight a→b */
        float w_ab = find_edge_weight(na, b);
        if (w_ab <= 0.0f) continue;

        /* edge weight b→c */
        float w_bc = find_edge_weight(nb, c);
        if (w_bc <= 0.0f) continue;

        /* 出边权重总和 */
        float sum_a = sum_outgoing_weights(na);
        float sum_b = sum_outgoing_weights(nb);
        if (sum_a <= 0.0f || sum_b <= 0.0f) continue;

        /* 独立边概率 */
        float p_ab = w_ab / sum_a;
        float p_bc = w_bc / sum_b;

        /* 三元组经验概率 */
        float p_abc = (float)rec->count / (float)(table->total_triplets > 0 ? table->total_triplets : 1);

        /* 不可分解性比值 */
        float expected = p_ab * p_bc + 1e-10f;
        float ir_ratio = p_abc / expected;

        /* 仅保留 ir_ratio > 1.0 的结果 */
        if (ir_ratio > 1.0f) {
            results[rc].node_a   = a;
            results[rc].node_b   = b;
            results[rc].node_c   = c;
            results[rc].ir_ratio = ir_ratio;
            results[rc].count    = rec->count;
            rc++;
        }
    }

    if (rc == 0) {
        free(results);
        return NULL;
    }

    /* 按 ir_ratio 降序排列 */
    qsort(results, rc, sizeof(IrreducibilityResult), ir_result_cmp_desc);

    *result_count = rc;
    return results;
}
