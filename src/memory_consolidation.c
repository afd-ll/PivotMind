#include "../include/memory_consolidation.h"
#include "../include/huarong_topology.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdio.h>

// ==================== 宏定义 ====================

#define DEFAULT_SIMILARITY_THRESHOLD 0.75f
#define DEFAULT_MIN_ACCESS_COUNT 3
#define DEFAULT_DECAY_RATE 0.01f
#define DEFAULT_RECENTCY_WEIGHT 0.3f
#define DEFAULT_FREQUENCY_WEIGHT 0.3f
#define DEFAULT_UTILITY_WEIGHT 0.4f

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))

// ==================== 静态变量 ====================

static ConsolidationConfig* g_default_config = NULL;

// ==================== 巩固配置 ==========

ConsolidationConfig* consolidation_config_create(void) {
    return consolidation_config_create_custom(
        DEFAULT_SIMILARITY_THRESHOLD,
        DEFAULT_MIN_ACCESS_COUNT,
        DEFAULT_DECAY_RATE
    );
}

ConsolidationConfig* consolidation_config_create_custom(
    float similarity_threshold, int min_access_count, float decay_rate) {
    
    ConsolidationConfig* config = (ConsolidationConfig*)malloc(sizeof(ConsolidationConfig));
    if (!config) return NULL;

    config->similarity_threshold = similarity_threshold;
    config->merge_ratio = 0.5f;
    config->min_access_count = min_access_count;
    config->max_memories_per_cluster = 100;
    config->decay_rate = decay_rate;
    config->consolidation_threshold = 0.5f;
    config->recency_weight = DEFAULT_RECENTCY_WEIGHT;
    config->frequency_weight = DEFAULT_FREQUENCY_WEIGHT;
    config->utility_weight = DEFAULT_UTILITY_WEIGHT;
    config->enable_compression = true;
    config->compression_ratio = 0.7f;
    config->min_cluster_size = 2;

    return config;
}

void consolidation_config_destroy(ConsolidationConfig* config) {
    if (config) free(config);
}

ConsolidationConfig* consolidation_get_default_config(void) {
    if (!g_default_config) {
        g_default_config = consolidation_config_create();
    }
    return g_default_config;
}

void consolidation_set_default_config(ConsolidationConfig* config) {
    if (g_default_config) {
        consolidation_config_destroy(g_default_config);
    }
    g_default_config = config;
}

// ==================== 相似度计算 ==========

float compute_similarity(HuarongTopologyNet* net, int node_a_id, int node_b_id,
                        SimilarityMetric metric) {
    if (!net || node_a_id < 0 || node_b_id < 0) return 0.0f;
    if (node_a_id >= net->node_count || node_b_id >= net->node_count) return 0.0f;
    if (node_a_id == node_b_id) return 1.0f;

    ReasoningNode* node_a = net->nodes[node_a_id];
    ReasoningNode* node_b = net->nodes[node_b_id];
    if (!node_a || !node_b) return 0.0f;

    switch (metric) {
        case SIMILARITY_COSINE: {
            // 余弦相似度 - 基于激活值
            if (node_a->activation < 0.001f && node_b->activation < 0.001f) {
                return 0.5f;
            }
            float dot = node_a->activation * node_b->activation;
            float norm_a = node_a->activation;
            float norm_b = node_b->activation;
            return (norm_a > 0 && norm_b > 0) ? dot / (norm_a * norm_b) : 0.0f;
        }

        case SIMILARITY_EUCLIDEAN: {
            // 欧几里得距离
            float diff = fabsf(node_a->activation - node_b->activation);
            return 1.0f / (1.0f + diff);
        }

        case SIMILARITY_MANHATTAN: {
            // 曼哈顿距离
            float diff = fabsf(node_a->activation - node_b->activation);
            return 1.0f / (1.0f + diff);
        }

        case SIMILARITY_JACCARD: {
            // Jaccard 相似度 - 基于连接
            int shared = 0;
            int total = MIN(node_a->connection_count, node_b->connection_count);
            if (total == 0) return 0.0f;
            
            for (int i = 0; i < node_a->connection_count; i++) {
                for (int j = 0; j < node_b->connection_count; j++) {
                    if (node_a->connections[i]->node_id == node_b->connections[j]->node_id) {
                        shared++;
                        break;
                    }
                }
            }
            return (float)shared / (node_a->connection_count + node_b->connection_count - shared);
        }

        case SIMILARITY_TVERSIFY:
        default: {
            // Tversky 相似度 - 综合方法
            float alpha = 1.0f, beta = 1.0f;
            int shared = 0;
            for (int i = 0; i < node_a->connection_count; i++) {
                for (int j = 0; j < node_b->connection_count; j++) {
                    if (node_a->connections[i]->node_id == node_b->connections[j]->node_id) {
                        shared++;
                        break;
                    }
                }
            }
            float diff_a = node_a->connection_count - shared;
            float diff_b = node_b->connection_count - shared;
            float tversky = (float)shared / (shared + alpha * diff_a + beta * diff_b);
            return CLAMP(tversky, 0.0f, 1.0f);
        }
    }
}

int compute_similarity_batch(HuarongTopologyNet* net, int* node_ids, int count,
                           SimilarityMetric metric, SimilarityResult* results) {
    if (!net || !node_ids || !results || count <= 0) return 0;

    int result_count = 0;
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            float sim = compute_similarity(net, node_ids[i], node_ids[j], metric);
            results[result_count].node_a_id = node_ids[i];
            results[result_count].node_b_id = node_ids[j];
            results[result_count].score = sim;
            results[result_count].confidence = 1.0f;
            result_count++;
        }
    }
    return result_count;
}

int* find_similar_nodes(HuarongTopologyNet* net, int node_id,
                      SimilarityMetric metric, float threshold,
                      int max_results, int* output_count) {
    if (!net || node_id < 0 || !output_count) {
        if (output_count) *output_count = 0;
        return NULL;
    }

    int* results = (int*)malloc(net->node_count * sizeof(int));
    if (!results) {
        *output_count = 0;
        return NULL;
    }

    int found = 0;
    for (int i = 0; i < net->node_count && found < max_results; i++) {
        if (i == node_id) continue;
        float sim = compute_similarity(net, node_id, i, metric);
        if (sim >= threshold) {
            results[found++] = i;
        }
    }

    *output_count = found;
    return results;
}

// ==================== 多维度重要性评估 ==========

float compute_recency_score(ReasoningNode* node, time_t current_time, float half_life, time_t last_used_field) {
    if (!node) return 0.0f;

    // 使用传入的 last_used_field 或使用 created_time 作为近似
    time_t last_used = last_used_field > 0 ? last_used_field : node->activation * 86400;
    time_t time_diff = current_time - last_used;
    if (time_diff < 0) time_diff = 0;

    // 指数衰减
    float decay_factor = expf(-time_diff * logf(2.0f) / half_life);
    return CLAMP(decay_factor, 0.0f, 1.0f);
}

float compute_frequency_score(ReasoningNode* node, int max_count) {
    if (!node) return 0.0f;

    // 频率评分 - 对数尺度避免极端值
    float freq = logf(1.0f + node->connection_count) / logf(1.0f + MAX(max_count, 1));
    return CLAMP(freq, 0.0f, 1.0f);
}

float compute_utility_score(ReasoningNode* node, float avg_activation) {
    if (!node) return 0.0f;

    // 实用性基于激活值与平均值的比较
    float diff = node->activation - avg_activation;
    float utility = 0.5f + diff;
    return CLAMP(utility, 0.0f, 1.0f);
}

float compute_salience_score(ReasoningNode* node, float historical_avg, float std_dev) {
    if (!node || std_dev < 0.001f) return 0.0f;

    // 显著性基于偏离历史平均的程度
    float z_score = (node->activation - historical_avg) / std_dev;
    float salience = fabsf(z_score) / (1.0f + fabsf(z_score));
    return CLAMP(salience, 0.0f, 1.0f);
}

float compute_forgetting_priority(ReasoningNode* node, time_t current_time,
                                  ConsolidationConfig* config) {
    if (!node || !config) return 0.0f;

    // 综合遗忘优先级
    float recency = compute_recency_score(node, current_time, 86400.0f, 0);  // 1天半衰期
    float frequency = compute_frequency_score(node, 100);
    float utility = compute_utility_score(node, 0.5f);

    // 低重要性 + 旧 + 低频率 = 高遗忘优先级
    float priority = 
        (1.0f - config->recency_weight) * (1.0f - recency) +
        (1.0f - config->frequency_weight) * (1.0f - frequency) +
        (1.0f - config->utility_weight) * (1.0f - utility);

    return CLAMP(priority, 0.0f, 1.0f);
}

// ==================== 记忆簇管理 ==========

MemoryCluster* memory_cluster_create(int cluster_id, int initial_capacity) {
    MemoryCluster* cluster = (MemoryCluster*)malloc(sizeof(MemoryCluster));
    if (!cluster) return NULL;

    cluster->cluster_id = cluster_id;
    cluster->node_ids = (int*)malloc(initial_capacity * sizeof(int));
    cluster->node_count = 0;
    cluster->capacity = initial_capacity;
    cluster->centroid = NULL;
    cluster->centroid_dim = 0;
    cluster->avg_importance = 0.0f;
    cluster->avg_activation = 0.0f;
    cluster->total_access_count = 0;
    cluster->first_created = time(NULL);
    cluster->last_updated = time(NULL);
    cluster->level = 0;

    return cluster;
}

void memory_cluster_destroy(MemoryCluster* cluster) {
    if (!cluster) return;
    if (cluster->node_ids) free(cluster->node_ids);
    if (cluster->centroid) free(cluster->centroid);
    free(cluster);
}

int memory_cluster_add_node(MemoryCluster* cluster, int node_id) {
    if (!cluster || node_id < 0) return -1;

    // 检查是否已存在
    for (int i = 0; i < cluster->node_count; i++) {
        if (cluster->node_ids[i] == node_id) return 0;
    }

    // 扩容
    if (cluster->node_count >= cluster->capacity) {
        int new_cap = cluster->capacity * 2;
        int* new_ids = (int*)realloc(cluster->node_ids, new_cap * sizeof(int));
        if (!new_ids) return -1;
        cluster->node_ids = new_ids;
        cluster->capacity = new_cap;
    }

    cluster->node_ids[cluster->node_count++] = node_id;
    cluster->last_updated = time(NULL);
    return 0;
}

int memory_cluster_remove_node(MemoryCluster* cluster, int node_id) {
    if (!cluster) return -1;

    for (int i = 0; i < cluster->node_count; i++) {
        if (cluster->node_ids[i] == node_id) {
            for (int j = i; j < cluster->node_count - 1; j++) {
                cluster->node_ids[j] = cluster->node_ids[j + 1];
            }
            cluster->node_count--;
            cluster->last_updated = time(NULL);
            return 0;
        }
    }
    return -1;
}

void memory_cluster_update_centroid(MemoryCluster* cluster, HuarongTopologyNet* net) {
    if (!cluster || !net || cluster->node_count == 0) return;

    // 计算平均激活值作为简化的质心
    float total_activation = 0.0f;
    int total_connections = 0;

    for (int i = 0; i < cluster->node_count; i++) {
        int node_id = cluster->node_ids[i];
        if (node_id >= 0 && node_id < net->node_count) {
            ReasoningNode* node = net->nodes[node_id];
            if (node) {
                total_activation += node->activation;
                total_connections += node->connection_count;
            }
        }
    }

    cluster->avg_activation = total_activation / cluster->node_count;
    cluster->avg_importance = (float)total_connections / cluster->node_count / 10.0f;
}

ClusterManager* cluster_manager_create(int capacity, SimilarityMetric metric,
                                     float threshold) {
    ClusterManager* manager = (ClusterManager*)malloc(sizeof(ClusterManager));
    if (!manager) return NULL;

    manager->clusters = (MemoryCluster**)malloc(capacity * sizeof(MemoryCluster*));
    if (!manager->clusters) {
        free(manager);
        return NULL;
    }

    manager->cluster_count = 0;
    manager->capacity = capacity;
    manager->metric = metric;
    manager->similarity_threshold = threshold;

    return manager;
}

void cluster_manager_destroy(ClusterManager* manager) {
    if (!manager) return;

    for (int i = 0; i < manager->cluster_count; i++) {
        if (manager->clusters[i]) {
            memory_cluster_destroy(manager->clusters[i]);
        }
    }
    free(manager->clusters);
    free(manager);
}

int memory_cluster_merge(ClusterManager* manager, int cluster_a_id, int cluster_b_id) {
    if (!manager) return -1;

    MemoryCluster* cluster_a = NULL;
    MemoryCluster* cluster_b = NULL;

    for (int i = 0; i < manager->cluster_count; i++) {
        if (manager->clusters[i]->cluster_id == cluster_a_id) {
            cluster_a = manager->clusters[i];
        }
        if (manager->clusters[i]->cluster_id == cluster_b_id) {
            cluster_b = manager->clusters[i];
        }
    }

    if (!cluster_a || !cluster_b) return -1;

    // 找到 cluster_b 的索引
    int idx_b = -1;
    for (int i = 0; i < manager->cluster_count; i++) {
        if (manager->clusters[i]->cluster_id == cluster_b_id) {
            idx_b = i;
            break;
        }
    }

    // 合并到 cluster_a
    for (int i = 0; i < cluster_b->node_count; i++) {
        memory_cluster_add_node(cluster_a, cluster_b->node_ids[i]);
    }

    // 移除 cluster_b
    memory_cluster_destroy(cluster_b);
    if (idx_b >= 0) {
        for (int i = idx_b; i < manager->cluster_count - 1; i++) {
            manager->clusters[i] = manager->clusters[i + 1];
        }
        manager->cluster_count--;
    }

    cluster_a->last_updated = time(NULL);
    return cluster_a->cluster_id;
}

int memory_cluster_split(ClusterManager* manager, int cluster_id,
                        int* split_indices, int split_count) {
    if (!manager || !split_indices || split_count <= 0) return -1;

    MemoryCluster* original = NULL;

    for (int i = 0; i < manager->cluster_count; i++) {
        if (manager->clusters[i]->cluster_id == cluster_id) {
            original = manager->clusters[i];
            break;
        }
    }

    if (!original) return -1;

    // 创建新簇
    static int next_cluster_id = 1;
    MemoryCluster* new_cluster = memory_cluster_create(next_cluster_id++, original->capacity);
    if (!new_cluster) return -1;

    // 分配节点到新簇
    for (int i = 0; i < split_count && i < original->node_count; i++) {
        memory_cluster_add_node(new_cluster, original->node_ids[split_indices[i]]);
        memory_cluster_remove_node(original, original->node_ids[split_indices[i]]);
    }

    // 添加新簇
    if (manager->cluster_count >= manager->capacity) {
        manager->clusters = (MemoryCluster**)realloc(
            manager->clusters, manager->capacity * 2 * sizeof(MemoryCluster*));
        manager->capacity *= 2;
    }
    manager->clusters[manager->cluster_count++] = new_cluster;

    return new_cluster->cluster_id;
}

// ==================== 记忆巩固 ==========

SimilarityResult* find_similar_memories(HuarongTopologyNet* net,
                                       ClusterManager* manager,
                                       float threshold,
                                       int* output_count) {
    if (!net || !manager || !output_count) {
        if (output_count) *output_count = 0;
        return NULL;
    }

    int max_results = net->node_count * (net->node_count - 1) / 2;
    SimilarityResult* results = (SimilarityResult*)malloc(max_results * sizeof(SimilarityResult));
    if (!results) {
        *output_count = 0;
        return NULL;
    }

    int count = 0;
    for (int i = 0; i < net->node_count; i++) {
        for (int j = i + 1; j < net->node_count; j++) {
            float sim = compute_similarity(net, i, j, manager->metric);
            if (sim >= threshold) {
                results[count].node_a_id = i;
                results[count].node_b_id = j;
                results[count].score = sim;
                results[count].confidence = sim;
                count++;
            }
        }
    }

    *output_count = count;
    return results;
}

int merge_memories(HuarongTopologyNet* net, int node_a_id, int node_b_id,
                  float merge_ratio) {
    if (!net || node_a_id < 0 || node_b_id < 0) return -1;
    if (node_a_id >= net->node_count || node_b_id >= net->node_count) return -1;
    if (node_a_id == node_b_id) return node_a_id;

    ReasoningNode* node_a = net->nodes[node_a_id];
    ReasoningNode* node_b = net->nodes[node_b_id];
    if (!node_a || !node_b) return -1;

    // 合并激活值
    float new_activation = merge_ratio * node_a->activation + 
                          (1.0f - merge_ratio) * node_b->activation;
    node_a->activation = new_activation;

    // 保留 node_a 作为合并后的节点
    // node_b 将被标记为待删除 (connection_count = -1 表示无效)
    node_b->connection_count = -1;

    return node_a_id;
}

int compress_memory_cluster(HuarongTopologyNet* net, ClusterManager* manager,
                           int cluster_id, float compression_ratio) {
    if (!net || !manager || cluster_id < 0) return 0;

    MemoryCluster* cluster = NULL;
    for (int i = 0; i < manager->cluster_count; i++) {
        if (manager->clusters[i]->cluster_id == cluster_id) {
            cluster = manager->clusters[i];
            break;
        }
    }

    if (!cluster || cluster->node_count <= 1) return 0;

    // 计算需要保留的节点数
    int keep_count = MAX(1, (int)(cluster->node_count * compression_ratio));

    // 按重要性排序 (简化版：按激活值)
    // 保留激活值高的节点
    int removed = 0;
    while (cluster->node_count > keep_count) {
        int min_idx = 0;
        float min_activation = 1.0f;

        for (int i = 0; i < cluster->node_count; i++) {
            int node_id = cluster->node_ids[i];
            if (node_id >= 0 && node_id < net->node_count) {
                ReasoningNode* node = net->nodes[node_id];
                if (node && node->activation < min_activation) {
                    min_activation = node->activation;
                    min_idx = i;
                }
            }
        }

        int removed_node_id = cluster->node_ids[min_idx];
        memory_cluster_remove_node(cluster, removed_node_id);
        // 标记节点为无效
        if (removed_node_id >= 0 && removed_node_id < net->node_count) {
            net->nodes[removed_node_id]->connection_count = -1;
        }
        removed++;
    }

    return removed;
}

int execute_consolidation(HuarongTopologyNet* net, ClusterManager* manager,
                         ConsolidationConfig* config) {
    if (!net || !manager || !config) return 0;

    ConsolidationConfig* def_config = consolidation_get_default_config();
    if (!def_config) def_config = config;

    // 1. 找到相似的记忆对
    int similar_count = 0;
    SimilarityResult* similar = find_similar_memories(net, manager,
                                                       def_config->similarity_threshold,
                                                       &similar_count);
    if (!similar || similar_count == 0) {
        if (similar) free(similar);
        return 0;
    }

    int consolidated_clusters = 0;

    // 2. 合并高相似度的记忆
    for (int i = 0; i < similar_count; i++) {
        if (similar[i].score >= def_config->similarity_threshold) {
            merge_memories(net, similar[i].node_a_id, similar[i].node_b_id,
                          def_config->merge_ratio);
            consolidated_clusters++;
        }
    }

    // 3. 压缩大型簇
    if (def_config->enable_compression) {
        for (int i = 0; i < manager->cluster_count; i++) {
            MemoryCluster* cluster = manager->clusters[i];
            if (cluster->node_count > def_config->max_memories_per_cluster) {
                compress_memory_cluster(net, manager, cluster->cluster_id,
                                       def_config->compression_ratio);
            }
        }
    }

    free(similar);
    return consolidated_clusters;
}

// ==================== 遗忘机制 ==========

int* select_forgetting_candidates(HuarongTopologyNet* net,
                                 ConsolidationConfig* config,
                                 int* count) {
    if (!net || !config || !count) {
        if (count) *count = 0;
        return NULL;
    }

    ConsolidationConfig* def_config = consolidation_get_default_config();
    if (!def_config) def_config = config;

    int* candidates = (int*)malloc(net->node_count * sizeof(int));
    if (!candidates) {
        *count = 0;
        return NULL;
    }

    time_t now = time(NULL);
    int found = 0;

    for (int i = 0; i < net->node_count; i++) {
        ReasoningNode* node = net->nodes[i];
        if (!node || node->connection_count < 0) continue;

        float priority = compute_forgetting_priority(node, now, def_config);
        if (priority > 0.5f) {  // 高遗忘优先级
            candidates[found++] = i;
        }
    }

    *count = found;
    return candidates;
}

int execute_forgetting(HuarongTopologyNet* net, int node_id, float forgetting_strength) {
    if (!net || node_id < 0 || node_id >= net->node_count) return -1;

    ReasoningNode* node = net->nodes[node_id];
    if (!node) return -1;

    // 遗忘：降低激活值和连接权重
    node->activation *= (1.0f - forgetting_strength);

    for (int i = 0; i < node->connection_count; i++) {
        node->connection_weights[i] *= (1.0f - forgetting_strength * 0.5f);
        if (node->connection_weights[i] < 0.01f) {
            node->connection_weights[i] = 0.0f;
        }
    }

    return 0;
}

float adaptive_decay_schedule(float base_decay, float importance, float time_delta) {
    // 重要性高的衰减慢，重要性低的衰减快
    float importance_factor = 1.0f - importance * 0.5f;
    float time_factor = 1.0f + time_delta / 86400.0f;  // 按天增长
    return base_decay * importance_factor * time_factor;
}

// ==================== 便捷函数 ==========

void memory_cluster_get_stats(MemoryCluster* cluster, float* avg_importance,
                             float* avg_activation, float* density) {
    if (!cluster) return;

    if (avg_importance) *avg_importance = cluster->avg_importance;
    if (avg_activation) *avg_activation = cluster->avg_activation;
    if (density) {
        if (cluster->node_count <= 1) {
            *density = 0.0f;
        } else {
            // 简化密度：实际连接数 / 最大可能连接数
            *density = (float)cluster->total_access_count / 
                      (cluster->node_count * (cluster->node_count - 1) / 2);
        }
    }
}

void cluster_manager_get_stats(ClusterManager* manager, int* total_clusters,
                             int* total_nodes, float* avg_cluster_size) {
    if (!manager) return;

    if (total_clusters) *total_clusters = manager->cluster_count;

    int nodes = 0;
    for (int i = 0; i < manager->cluster_count; i++) {
        nodes += manager->clusters[i]->node_count;
    }
    if (total_nodes) *total_nodes = nodes;

    if (avg_cluster_size) {
        *avg_cluster_size = (manager->cluster_count > 0) ?
            (float)nodes / manager->cluster_count : 0.0f;
    }
}
