#include "topology_growth.h"
#include "huarong_topology.h"
#include "node_hash.h"
#include "multi_topology.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <limits.h>

// ==================== 静态变量 ====================

static TopologyGrowthConfig* g_default_config = NULL;
static GrowthStats g_global_stats = {0};

// ==================== 辅助函数 ====================

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))

// 计算拓扑密度
static float compute_topology_density(HuarongTopologyNet* net) {
    if (!net || net->node_count <= 1) return 0.0f;
    
    int max_edges = net->node_count * (net->node_count - 1) / 2;
    int actual_edges = 0;
    
    for (int i = 0; i < net->node_count; i++) {
        if (net->nodes[i]) {
            actual_edges += net->nodes[i]->edge_count;
        }
    }
    
    return (float)actual_edges / max_edges;
}

// 计算负载因子
static float compute_load_factor(HuarongTopologyNet* net, int max_nodes) {
    if (max_nodes <= 0) return 0.0f;
    return (float)net->node_count / max_nodes;
}

// 检查冷却时间
static bool check_cooldown(GrowthTrigger* trigger) {
    if (!trigger || trigger->cooldown_ticks <= 0) return true;
    
    time_t now = time(NULL);
    if (now - trigger->last_triggered < trigger->cooldown_ticks) {
        return false;
    }
    return true;
}

// 更新触发时间
static void update_trigger_time(GrowthTrigger* trigger) {
    if (trigger) {
        trigger->last_triggered = time(NULL);
    }
}

// ==================== 配置管理 ====================

TopologyGrowthConfig* topology_growth_config_create(void) {
    return topology_growth_config_create_custom(10000, 100, 100);
}

TopologyGrowthConfig* topology_growth_config_create_custom(
    int max_nodes, int max_connections, int growth_increment) {
    
    TopologyGrowthConfig* config = 
        (TopologyGrowthConfig*)malloc(sizeof(TopologyGrowthConfig));
    if (!config) return NULL;

    config->max_nodes_per_topology = max_nodes;
    config->max_connections_per_node = max_connections;
    config->growth_increment = growth_increment;
    config->min_connection_weight = 0.01f;

    // 节点数量触发器
    config->node_count_trigger.type = GROWTH_TRIGGER_NODE_COUNT;
    config->node_count_trigger.threshold = 0.6f;  // 60% 容量即扩容，给增长留余量
    config->node_count_trigger.hysteresis = 0.1f;
    config->node_count_trigger.cooldown_ticks = 60;
    config->node_count_trigger.last_triggered = 0;

    // 密度触发器
    config->density_trigger.type = GROWTH_TRIGGER_LINK_DENSITY;
    config->density_trigger.threshold = 0.7f;
    config->density_trigger.hysteresis = 0.05f;
    config->density_trigger.cooldown_ticks = 120;
    config->density_trigger.last_triggered = 0;

    // 负载触发器
    config->load_trigger.type = GROWTH_TRIGGER_LOAD_FACTOR;
    config->load_trigger.threshold = 0.9f;
    config->load_trigger.hysteresis = 0.05f;
    config->load_trigger.cooldown_ticks = 60;
    config->load_trigger.last_triggered = 0;

    // 增长限制
    config->max_growth_rate = 1000.0f;  // 1000 节点/秒
    config->max_nodes_per_hour = 10000;
    config->max_topologies = 64;

    // 自适应参数
    config->auto_shrink_enabled = true;
    config->shrink_threshold = 0.3f;  // 低于 30% 容量时收缩
    config->idle_before_shrink = 3600;  // 1小时空闲

    // 学习参数
    config->learning_rate = 0.01f;
    config->connection_decay = 0.999f;

    return config;
}

void topology_growth_config_destroy(TopologyGrowthConfig* config) {
    if (config) free(config);
}

TopologyGrowthConfig* topology_growth_get_default_config(void) {
    if (!g_default_config) {
        g_default_config = topology_growth_config_create();
    }
    return g_default_config;
}

void topology_growth_set_default_config(TopologyGrowthConfig* config) {
    if (g_default_config) {
        topology_growth_config_destroy(g_default_config);
    }
    g_default_config = config;
}

// ==================== 动态节点操作 ====================

int insert_node_dynamic(MasterTopology* master, int topo_id,
                       const char* concept, float* features, int feature_dim) {
    if (!master || !concept) return -1;

    SubTopology* sub = master_get_sub_topology(master, topo_id);
    if (!sub || !sub->net) return -1;

    // 检查容量（用实际容量，不是全局上限）
    if ((size_t)sub->net->node_count >= sub->net->max_nodes) {
        // 尝试自动扩展
        if (check_growth_needed(master, topo_id)) {
            auto_extend_topology(master, topo_id);
        }
        if ((size_t)sub->net->node_count >= sub->net->max_nodes) {
            return -1;
        }
    }

    // 插入节点
    ReasoningNode* new_node = huarong_net_add_node(sub->net, concept, features, feature_dim);
    if (!new_node) return -1;

    // 更新哈希表
    if (sub->node_hash) {
        node_hash_add(sub->node_hash, new_node);
    }

    // 更新统计
    g_global_stats.total_node_insertions++;
    g_global_stats.current_node_count++;
    if (g_global_stats.current_node_count > g_global_stats.peak_node_count) {
        g_global_stats.peak_node_count = g_global_stats.current_node_count;
    }

    return new_node->node_id;
}

int remove_node_dynamic(MasterTopology* master, int topo_id,
                      int node_id, bool force) {
    if (!master || node_id < 0) return -1;

    SubTopology* sub = master_get_sub_topology(master, topo_id);
    if (!sub || !sub->net || node_id >= sub->net->node_count) return -1;

    ReasoningNode* node = sub->net->nodes[node_id];
    if (!node) return -1;

    // 检查连接
    if (!force && node->edge_count > 0) {
        return -1;  // 有连接，不能删除
    }

    /* v0.5.7-A: 全图反向清理——其他节点指向本节点的边（入边）也必须清除，
     * 否则删除后 target 悬垂（保存时解引用会写坏状态文件，加载时边恢复失败） */
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* st = master->sub_topologies[t];
        if (!st || !st->net) continue;
        for (int ni = 0; ni < st->net->node_count; ni++) {
            ReasoningNode* other = st->net->nodes[ni];
            if (!other || other == node || other->edge_count == 0) continue;
            int changed = 0;
            for (int j = 0; j < other->edge_count; ) {
                if (other->edges[j].target == node) {
                    for (int k = j; k < other->edge_count - 1; k++)
                        other->edges[k] = other->edges[k + 1];
                    other->edge_count--;
                    changed = 1;
                } else {
                    j++;
                }
            }
            /* 边数组变了——连接哈希重建（防查错） */
            if (changed && other->conn_hash) {
                /* 简单重建：清除后全量重加（删除不频繁，可接受） */
                for (int hi = 0; hi <= other->conn_hash_mask; hi++) {
                    other->conn_hash[hi].target = NULL;
                    other->conn_hash[hi].is_deleted = 0;
                }
                other->conn_hash_entries = 0;
                for (int e2 = 0; e2 < other->edge_count; e2++) {
                    if (!other->edges[e2].target) continue;
                    int idx = e2;
                    unsigned int h = (unsigned int)(uintptr_t)other->edges[e2].target & (unsigned int)other->conn_hash_mask;
                    while (other->conn_hash[h].target != NULL && !other->conn_hash[h].is_deleted) {
                        h = (h + 1) & (unsigned int)other->conn_hash_mask;
                    }
                    other->conn_hash[h].target = other->edges[e2].target;
                    other->conn_hash[h].index = idx;
                    other->conn_hash[h].is_deleted = 0;
                    other->conn_hash_entries++;
                }
            }
        }
    }

    // 移除连接
    if (node->edge_count > 0) {
        for (int i = 0; i < node->edge_count; i++) {
            if (node->edges[i].target) {
                // 从目标节点移除反向连接
                ReasoningNode* target = node->edges[i].target;
                for (int j = 0; j < target->edge_count; j++) {
                    if (target->edges[j].target && target->edges[j].target->node_id == node_id) {
                        // 移除
                        for (int k = j; k < target->edge_count - 1; k++) {
                            target->edges[k].target = target->edges[k + 1].target;
                            target->edges[k].weight = target->edges[k + 1].weight;
                            target->edges[k].motivational_bias = target->edges[k + 1].motivational_bias;
                            target->edges[k].confidence = target->edges[k + 1].confidence;
                        }
                        target->edge_count--;
                        break;
                    }
                }
            }
        }
        free(node->edges);
        node->edges = NULL;
        node->edge_count = 0;
    }

    // 从哈希表移除
    if (sub->node_hash && node->concept) {
        node_hash_remove(sub->node_hash, node->concept);
    }

    // 释放节点
    if (node->concept) free(node->concept);
    if (node->features) free(node->features);
    sub->net->nodes[node_id] = NULL;
    free(node);

    // 更新统计
    g_global_stats.total_node_removals++;
    g_global_stats.current_node_count--;

    return 0;
}

int insert_nodes_batch(MasterTopology* master, int topo_id,
                     const char** concepts, int count) {
    if (!master || !concepts || count <= 0) return 0;

    int success_count = 0;
    for (int i = 0; i < count; i++) {
        if (insert_node_dynamic(master, topo_id, concepts[i], NULL, 0) >= 0) {
            success_count++;
        }
    }
    return success_count;
}

// ==================== 动态边操作 ====================

int add_edge_dynamic(MasterTopology* master, int topo_id,
                    int from_node_id, int to_node_id, float weight) {
    if (!master || from_node_id < 0 || to_node_id < 0) return -1;

    SubTopology* sub = master_get_sub_topology(master, topo_id);
    if (!sub || !sub->net) return -1;

    if (from_node_id >= sub->net->node_count || to_node_id >= sub->net->node_count) {
        return -1;
    }

    // 添加连接
    int result = huarong_net_add_connection(sub->net, from_node_id, to_node_id, weight);

    if (result == 0) {
        g_global_stats.total_edge_insertions++;
    }

    return result;
}

int remove_edge_dynamic(MasterTopology* master, int topo_id,
                      int from_node_id, int to_node_id) {
    if (!master || from_node_id < 0 || to_node_id < 0) return -1;

    SubTopology* sub = master_get_sub_topology(master, topo_id);
    if (!sub || !sub->net) return -1;

    ReasoningNode* from_node = sub->net->nodes[from_node_id];
    if (!from_node) return -1;

    // 查找并移除连接
    for (int i = 0; i < from_node->edge_count; i++) {
        if (from_node->edges[i].target->node_id == to_node_id) {
            // 找到，移除
            for (int j = i; j < from_node->edge_count - 1; j++) {
                from_node->edges[j].target = from_node->edges[j + 1].target;
                from_node->edges[j].weight = from_node->edges[j + 1].weight;
                from_node->edges[j].motivational_bias = from_node->edges[j + 1].motivational_bias;
                from_node->edges[j].confidence = from_node->edges[j + 1].confidence;
            }
            from_node->edge_count--;

            // 从目标节点也移除反向连接
            ReasoningNode* to_node = sub->net->nodes[to_node_id];
            if (to_node) {
                for (int j = 0; j < to_node->edge_count; j++) {
                    if (to_node->edges[j].target->node_id == from_node_id) {
                        for (int k = j; k < to_node->edge_count - 1; k++) {
                            to_node->edges[k].target = to_node->edges[k + 1].target;
                            to_node->edges[k].weight = to_node->edges[k + 1].weight;
                            to_node->edges[k].motivational_bias = to_node->edges[k + 1].motivational_bias;
                            to_node->edges[k].confidence = to_node->edges[k + 1].confidence;
                        }
                        to_node->edge_count--;
                        break;
                    }
                }
            }

            g_global_stats.total_edge_removals++;
            return 0;
        }
    }

    return -1;  // 未找到连接
}

int update_edge_weight(MasterTopology* master, int topo_id,
                      int from_node_id, int to_node_id, float new_weight) {
    if (!master || from_node_id < 0 || to_node_id < 0) return -1;

    SubTopology* sub = master_get_sub_topology(master, topo_id);
    if (!sub || !sub->net) return -1;

    ReasoningNode* from_node = sub->net->nodes[from_node_id];
    if (!from_node) return -1;

    // 查找连接
    for (int i = 0; i < from_node->edge_count; i++) {
        if (from_node->edges[i].target->node_id == to_node_id) {
            from_node->edges[i].weight = CLAMP(new_weight, 0.0f, 1.0f);
            return 0;
        }
    }

    return -1;  // 未找到连接
}

// ==================== 自动拓扑增长 ====================

bool check_growth_needed(MasterTopology* master, int topo_id) {
    if (!master) return false;

    TopologyGrowthConfig* config = topology_growth_get_default_config();
    SubTopology* sub = (topo_id >= 0) ? master_get_sub_topology(master, topo_id) : NULL;

    // 如果指定了拓扑，检查该拓扑
    if (sub && sub->net) {
        HuarongTopologyNet* net = sub->net;

        // 检查节点数量（用实际容量而非全局上限，否则动态扩容形同虚设）
        if (check_cooldown(&config->node_count_trigger)) {
            float usage = (float)net->node_count / net->max_nodes;
            if (usage > config->node_count_trigger.threshold) {
                return true;
            }
        }

        // 检查密度
        if (check_cooldown(&config->density_trigger)) {
            float density = compute_topology_density(net);
            if (density > config->density_trigger.threshold) {
                return true;
            }
        }

        // 检查负载（用实际容量）
        if (check_cooldown(&config->load_trigger)) {
            float load = compute_load_factor(net, net->max_nodes);
            if (load > config->load_trigger.threshold) {
                return true;
            }
        }
    }

    return false;
}

int auto_extend_topology(MasterTopology* master, int topo_id) {
    if (!master) return -1;

    TopologyGrowthConfig* config = topology_growth_get_default_config();
    SubTopology* sub = master_get_sub_topology(master, topo_id);

    if (!sub || !sub->net) return -1;

    HuarongTopologyNet* net = sub->net;

    // 计算需要扩展的容量 (v0.5.1: 取消硬天花板, huarong_net_add_node 已自动扩容)
    size_t current_capacity = net->max_nodes;
    size_t new_capacity = current_capacity + (size_t)config->growth_increment;

    if (new_capacity <= current_capacity) {
        return 0;  // 无需扩展
    }

    // 扩容
    ReasoningNode** new_nodes = (ReasoningNode**)realloc(
        net->nodes, new_capacity * sizeof(ReasoningNode*));
    if (!new_nodes) return -1;

    // 初始化新空间
    for (size_t i = current_capacity; i < new_capacity; i++) {
        new_nodes[i] = NULL;
    }

    net->nodes = new_nodes;
    net->max_nodes = new_capacity;

    // 更新统计
    g_global_stats.total_growth_events++;
    g_global_stats.last_growth = time(NULL);

    if (config->node_count_trigger.cooldown_ticks > 0) {
        update_trigger_time(&config->node_count_trigger);
    }

    return new_capacity;
}

int auto_shrink_topology(MasterTopology* master, int topo_id) {
    if (!master || master_load_protected(master)) return -1;  /* 加载保护期：不收缩（v0.6） */
    TopologyGrowthConfig* config = topology_growth_get_default_config();
    if (!config->auto_shrink_enabled) return -1;

    SubTopology* sub = master_get_sub_topology(master, topo_id);
    if (!sub || !sub->net) return -1;

    HuarongTopologyNet* net = sub->net;

    // 检查使用率
    float usage = (float)net->node_count / net->max_nodes;
    if (usage > config->shrink_threshold) {
        return 0;  // 不需要收缩
    }

    // 计算新的容量
    size_t new_capacity = net->max_nodes - (size_t)config->growth_increment;
    if (new_capacity < (size_t)net->node_count) {
        new_capacity = (size_t)net->node_count + 10;  // 保留一些余量
    }

    // 不收缩太多
    if (new_capacity < net->max_nodes * 0.5f) {
        new_capacity = net->max_nodes * 0.5f;
    }

    if (new_capacity >= net->max_nodes) {
        return 0;
    }

    // 扩容 (收缩后可能需要扩容来处理现有节点)
    ReasoningNode** new_nodes = (ReasoningNode**)realloc(
        net->nodes, new_capacity * sizeof(ReasoningNode*));
    if (!new_nodes) return -1;

    net->nodes = new_nodes;
    net->max_nodes = new_capacity;

    // 更新统计
    g_global_stats.total_shrink_events++;
    g_global_stats.last_shrink = time(NULL);

    return new_capacity;
}

int topology_load_balancing(MasterTopology* master) {
    if (!master) return -1;

    // 简单负载均衡：将节点从繁忙拓扑迁移到空闲拓扑
    int total_nodes = 0;
    int active_sub_count = 0;

    // 计算平均负载
    for (int i = 0; i < master->sub_topo_count; i++) {
        SubTopology* sub = master->sub_topologies[i];
        if (sub && sub->net && sub->is_active) {
            total_nodes += sub->net->node_count;
            active_sub_count++;
        }
    }

    if (active_sub_count <= 1) return 0;

    int avg_nodes = total_nodes / active_sub_count;
    int migrated = 0;

    // 迁移节点
    for (int i = 0; i < master->sub_topo_count; i++) {
        SubTopology* src = master->sub_topologies[i];
        if (!src || !src->net || !src->is_active) continue;

        // 找出过载的拓扑
        if (src->net->node_count <= avg_nodes * 1.2f) continue;

        // 找出欠载的拓扑
        SubTopology* dst = NULL;
        int min_load = INT_MAX;
        int dst_idx = -1;
        for (int j = 0; j < master->sub_topo_count; j++) {
            if (i == j) continue;
            SubTopology* sub = master->sub_topologies[j];
            if (!sub || !sub->net || !sub->is_active) continue;
            if (sub->net->node_count < min_load) {
                min_load = sub->net->node_count;
                dst = sub;
                dst_idx = j;
            }
        }

        if (!dst || dst_idx < 0) continue;

        // 迁移几个节点
        int to_migrate = MIN(
            (src->net->node_count - avg_nodes) / 2,
            (avg_nodes - dst->net->node_count) / 2 + 1
        );
        to_migrate = MIN(to_migrate, 10);  // 每次最多迁移10个

        for (int k = 0; k < to_migrate && src->net->node_count > avg_nodes; k++) {
            // 找最小度节点迁移（连接少的节点迁移成本低）
            int min_degree_node = -1;
            int min_degree = INT_MAX;
            for (int n = 0; n < src->net->node_count; n++) {
                ReasoningNode* node = src->net->nodes[n];
                if (node && node->edge_count < min_degree) {
                    min_degree = node->edge_count;
                    min_degree_node = n;
                }
            }

            if (min_degree_node < 0) break;

            // 执行实际迁移
            ReasoningNode* node_to_move = src->net->nodes[min_degree_node];
            if (!node_to_move) break;

            // 检查目标拓扑是否有空间
            if ((size_t)dst->net->node_count >= dst->net->max_nodes) {
                // 尝试扩展目标拓扑
                int new_cap = dst->net->max_nodes + 100;
                ReasoningNode** new_nodes = (ReasoningNode**)realloc(
                    dst->net->nodes, new_cap * sizeof(ReasoningNode*));
                if (!new_nodes) break; // 扩容失败
                for (int n = dst->net->max_nodes; n < new_cap; n++) {
                    new_nodes[n] = NULL;
                }
                dst->net->nodes = new_nodes;
                dst->net->max_nodes = new_cap;
            }

            // 1. 从源拓扑移除
            src->net->nodes[min_degree_node] = NULL;
            src->net->node_count--;

            // 更新源拓扑的哈希表（如果存在）
            if (src->node_hash && node_to_move->concept) {
                node_hash_remove(src->node_hash, node_to_move->concept);
            }

            // 2. 添加到目标拓扑
            int new_node_id = dst->net->node_count;
            node_to_move->node_id = new_node_id;
            dst->net->nodes[new_node_id] = node_to_move;
            dst->net->node_count++;

            // 更新目标拓扑的哈希表（如果存在）
            if (dst->node_hash && node_to_move->concept) {
                node_hash_add(dst->node_hash, node_to_move);
            }

            // 3. 更新跨拓扑链接（如果有）
            // 更新所有跨链接中的节点ID引用
            for (int l = 0; l < master->cross_link_count; l++) {
                CrossTopologyLink* link = master->cross_links[l];
                if (link) {
                    // 更新源引用
                    if (link->from_topo_id == i && link->from_node_id == min_degree_node) {
                        link->from_topo_id = dst_idx;
                        link->from_node_id = new_node_id;
                    }
                    // 更新目标引用
                    if (link->to_topo_id == i && link->to_node_id == min_degree_node) {
                        link->to_topo_id = dst_idx;
                        link->to_node_id = new_node_id;
                    }
                }
            }

            migrated++;
        }
    }

    return migrated;
}

// ==================== 重要性剪枝 ====================

int prune_node_importance(MasterTopology* master, int topo_id,
                         float min_importance, bool dry_run) {
    if (!master || min_importance < 0) return 0;

    SubTopology* sub = master_get_sub_topology(master, topo_id);
    if (!sub || !sub->net) return 0;

    NodeImportanceEvaluator* evaluator = node_importance_create(0.85f, 50);
    if (!evaluator) return 0;

    int count = 0;
    ImportanceMetrics** metrics = evaluate_all_nodes(evaluator, sub->net, &count);

    int removed = 0;
    for (int i = 0; i < count; i++) {
        if (metrics[i] && metrics[i]->composite_score < min_importance) {
            if (!dry_run) {
                // 只移除无连接的节点
                ReasoningNode* node = sub->net->nodes[metrics[i]->node_id];
                if (node && node->edge_count == 0) {
                    if (remove_node_dynamic(master, topo_id, metrics[i]->node_id, false) == 0) {
                        removed++;
                    }
                }
            } else {
                removed++;
            }
        }
    }

    // 清理
    for (int i = 0; i < count; i++) {
        if (metrics[i]) free(metrics[i]);
    }
    free(metrics);
    node_importance_destroy(evaluator);

    return removed;
}

/* v0.5.7: 垃圾概念（//家 类 URL/路径残留）——无条件视为孤立删除。
 * 喂料时 http:// 过滤了但 "//xxx" 漏网（状态里 442 个），感知皮层
 * 随机抽到会搜索空转。这些节点无语义价值，有边也删。 */
static int is_junk_concept(const char* c) {
    if (!c || !c[0]) return 1;
    if (strstr(c, "//") || strstr(c, "http") || strstr(c, "www.")) return 1;
    char ch = c[0];
    if (ch == '/' || ch == '.' || ch == '-' || ch == '_' || ch == '\\' || ch == '#') return 1;
    return 0;
}

int prune_low_connectivity(MasterTopology* master, int topo_id,
                          int min_connections) {
    if (!master || min_connections < 0) return 0;

    SubTopology* sub = master_get_sub_topology(master, topo_id);
    if (!sub || !sub->net) return 0;

    int removed = 0;
    for (int i = 0; i < sub->net->node_count; i++) {
        ReasoningNode* node = sub->net->nodes[i];
        /* 跳过冻结节点（is_cooled）：冻结是 lazy memory 缓存释放（边数据
         * 已存盘可恢复），不是孤立垃圾——RED 修剪不该删它们（v0.6） */
        if (node && node->is_cooled) continue;
        /* v0.5.7: 垃圾概念（//家 类）无条件删——无语义价值，感知皮层抽到会空转 */
        if (node && is_junk_concept(node->concept)) {
            if (remove_node_dynamic(master, topo_id, i, false) == 0) {
                removed++;
            }
            continue;
        }
        if (node && node->edge_count < min_connections) {
            if (remove_node_dynamic(master, topo_id, i, false) == 0) {
                removed++;
            }
        }
    }

    return removed;
}

int prune_isolated_nodes(MasterTopology* master, int topo_id) {
    /* 加载保护期：状态加载后 30 分钟内不清理孤立节点（按时间，不受
     * tick 速率影响）。新喂知识刚加载时边恢复/自主学习尚未完成，
     * 立即清理会把知识当"孤立节点"连锁删光。 */
    if (master_load_protected(master)) return 0;
    return prune_low_connectivity(master, topo_id, 1);
}

// ==================== 动态权重更新 ====================

int dynamic_weight_update(MasterTopology* master, int topo_id,
                         WeightUpdatePolicy policy) {
    if (!master) return -1;

    SubTopology* sub = master_get_sub_topology(master, topo_id);
    if (!sub || !sub->net) return -1;

    TopologyGrowthConfig* config = topology_growth_get_default_config();

    switch (policy) {
        case WEIGHT_UPDATE_HEBBIAN: {
            // 赫布规则：一起激活的节点连接增强
            for (int i = 0; i < sub->net->node_count; i++) {
                ReasoningNode* node = sub->net->nodes[i];
                if (!node) continue;

                for (int j = 0; j < node->edge_count; j++) {
                    float delta = config->learning_rate * node->activation * 
                                  node->edges[j].target->activation;
                    node->edges[j].weight += delta;
                    node->edges[j].weight = CLAMP(node->edges[j].weight, 0.0f, 1.0f);
                }
            }
            break;
        }

        case WEIGHT_UPDATE_GRADIENT: {
            // 梯度下降更新
            for (int i = 0; i < sub->net->node_count; i++) {
                ReasoningNode* node = sub->net->nodes[i];
                if (!node) continue;

                for (int j = 0; j < node->edge_count; j++) {
                    // 简化梯度更新
                    float gradient = node->activation - node->edges[j].weight;
                    node->edges[j].weight -= config->learning_rate * gradient;
                    node->edges[j].weight = CLAMP(node->edges[j].weight, 0.0f, 1.0f);
                }
            }
            break;
        }

        case WEIGHT_UPDATE_RULE_BASED: {
            // 基于规则的更新
            for (int i = 0; i < sub->net->node_count; i++) {
                ReasoningNode* node = sub->net->nodes[i];
                if (!node) continue;

                for (int j = 0; j < node->edge_count; j++) {
                    // 激活高的连接增强，低的衰减
                    float target_weight = node->activation * 
                                         node->edges[j].target->activation;
                    float diff = target_weight - node->edges[j].weight;
                    node->edges[j].weight += config->learning_rate * diff;
                    node->edges[j].weight = CLAMP(node->edges[j].weight, 0.0f, 1.0f);
                }
            }
            break;
        }

        case WEIGHT_UPDATE_HYBRID:
        default: {
            // 混合策略
            float alpha = 0.5f;
            for (int i = 0; i < sub->net->node_count; i++) {
                ReasoningNode* node = sub->net->nodes[i];
                if (!node) continue;

                for (int j = 0; j < node->edge_count; j++) {
                    float hebbian = config->learning_rate * node->activation * 
                                   node->edges[j].target->activation;
                    float gradient = node->activation - node->edges[j].weight;
                    float update = alpha * hebbian + (1 - alpha) * gradient;
                    node->edges[j].weight -= config->learning_rate * update;
                    node->edges[j].weight = CLAMP(node->edges[j].weight, 0.0f, 1.0f);
                }
            }
            break;
        }
    }

    return 0;
}

float adaptive_learning_rate(MasterTopology* master, int topo_id) {
    if (!master) return 0.01f;

    TopologyGrowthConfig* config = topology_growth_get_default_config();

    SubTopology* sub = master_get_sub_topology(master, topo_id);
    if (!sub || !sub->net) return config->learning_rate;

    // 根据负载自适应调整学习率
    float load = (float)sub->net->node_count / config->max_nodes_per_topology;

    // 负载高时降低学习率，负载低时提高
    float adaptive_lr = config->learning_rate * (1.0f - load * 0.5f);

    return CLAMP(adaptive_lr, 0.001f, config->learning_rate);
}

float connection_strength_decay(MasterTopology* master, int topo_id,
                               float decay_factor) {
    if (!master || decay_factor <= 0 || decay_factor >= 1) return 0.0f;

    SubTopology* sub = master_get_sub_topology(master, topo_id);
    if (!sub || !sub->net) return 0.0f;

    TopologyGrowthConfig* config = topology_growth_get_default_config();
    float total_strength = 0.0f;
    int edge_count = 0;

    for (int i = 0; i < sub->net->node_count; i++) {
        ReasoningNode* node = sub->net->nodes[i];
        if (!node) continue;

        for (int j = 0; j < node->edge_count; j++) {
            node->edges[j].weight *= decay_factor;
            if (node->edges[j].weight < config->min_connection_weight) {
                node->edges[j].weight = 0.0f;
            }
            total_strength += node->edges[j].weight;
            edge_count++;
        }
    }

    return (edge_count > 0) ? total_strength / edge_count : 0.0f;
}

// ==================== 跨拓扑动态操作 ====================

int insert_cross_topology_link(MasterTopology* master,
                             int from_topo_id, int from_node_id,
                             int to_topo_id, int to_node_id,
                             float weight, const char* relation) {
    if (!master) return -1;

    return master_add_cross_link(master, from_topo_id, from_node_id,
                                 to_topo_id, to_node_id, weight, relation);
}

int remove_cross_topology_link(MasterTopology* master,
                              int from_topo_id, int from_node_id,
                              int to_topo_id, int to_node_id) {
    if (!master) return -1;

    pthread_rwlock_wrlock(&master->rwlock);

    // 查找跨拓扑链接
    for (int i = 0; i < master->cross_link_count; i++) {
        CrossTopologyLink* link = master->cross_links[i];
        if (link && 
            link->from_topo_id == from_topo_id &&
            link->from_node_id == from_node_id &&
            link->to_topo_id == to_topo_id &&
            link->to_node_id == to_node_id) {
            
            // 找到，移除
            free(link);
            for (int j = i; j < master->cross_link_count - 1; j++) {
                master->cross_links[j] = master->cross_links[j + 1];
            }
            master->cross_link_count--;
            pthread_rwlock_unlock(&master->rwlock);
            return 0;
        }
    }

    pthread_rwlock_unlock(&master->rwlock);
    return -1;  // 未找到
}

// ==================== 统计与监控 ====================

const GrowthStats* topology_growth_get_stats(MasterTopology* master) {
    (void)master;  // 未使用
    return &g_global_stats;
}

void topology_growth_reset_stats(MasterTopology* master) {
    (void)master;
    memset(&g_global_stats, 0, sizeof(GrowthStats));
}

float topology_health_score(MasterTopology* master, int topo_id) {
    if (!master) return 0.0f;

    float score = 1.0f;

    // 容量使用率评分
    SubTopology* sub = master_get_sub_topology(master, topo_id);
    if (sub && sub->net) {
        float usage = (float)sub->net->node_count / sub->net->max_nodes;
        if (usage > 0.9f) {
            score -= 0.3f;  // 接近容量
        } else if (usage < 0.3f) {
            score -= 0.2f;  // 利用率低
        }

        // 密度评分
        float density = compute_topology_density(sub->net);
        if (density > 0.8f) {
            score -= 0.2f;  // 太密集
        } else if (density < 0.1f) {
            score -= 0.2f;  // 太稀疏
        }

        // 连接性评分
        int isolated = prune_isolated_nodes(master, topo_id);
        (void)isolated;  // 忽略结果，只为计算
    } else {
        score = 0.0f;
    }

    return CLAMP(score, 0.0f, 1.0f);
}

int diagnose_topology(MasterTopology* master, int topo_id, char* report) {
    if (!master) return 3;

    SubTopology* sub = master_get_sub_topology(master, topo_id);
    if (!sub || !sub->net) return 3;

    TopologyGrowthConfig* config = topology_growth_get_default_config();

    float usage = (float)sub->net->node_count / config->max_nodes_per_topology;
    float density = compute_topology_density(sub->net);

    int status = 0;  // 0=健康

    if (usage > 0.9f || density > 0.8f) {
        status = 1;  // 需增长
    } else if (usage < 0.3f || density < 0.1f) {
        status = 2;  // 需收缩
    }

    if (report) {
        snprintf(report, 256,
                "Topology %d: usage=%.2f, density=%.3f, status=%s",
                topo_id, usage, density,
                status == 0 ? "healthy" : (status == 1 ? "needs_growth" : 
                  (status == 2 ? "needs_shrink" : "abnormal")));
    }

    return status;
}