#include "../include/topology_growth.h"
#include "../include/huarong_topology.h"
#include "../include/node_hash.h"
#include "../include/multi_topology.h"
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
            actual_edges += net->nodes[i]->connection_count;
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
    config->node_count_trigger.threshold = 0.8f;  // 80% 容量
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

    TopologyGrowthConfig* config = topology_growth_get_default_config();

    // 检查容量
    if (sub->net->node_count >= config->max_nodes_per_topology) {
        // 尝试自动扩展
        if (check_growth_needed(master, topo_id)) {
            auto_extend_topology(master, topo_id);
        }
        if (sub->net->node_count >= sub->net->max_nodes) {
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
    if (!force && node->connection_count > 0) {
        return -1;  // 有连接，不能删除
    }

    // 移除连接
    if (node->connection_count > 0) {
        for (int i = 0; i < node->connection_count; i++) {
            if (node->connections[i]) {
                // 从目标节点移除反向连接
                ReasoningNode* target = node->connections[i];
                for (int j = 0; j < target->connection_count; j++) {
                    if (target->connections[j] && target->connections[j]->node_id == node_id) {
                        // 移除
                        for (int k = j; k < target->connection_count - 1; k++) {
                            target->connections[k] = target->connections[k + 1];
                            target->connection_weights[k] = target->connection_weights[k + 1];
                        }
                        target->connection_count--;
                        break;
                    }
                }
            }
        }
        free(node->connections);
        free(node->connection_weights);
        node->connections = NULL;
        node->connection_weights = NULL;
        node->connection_count = 0;
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
    for (int i = 0; i < from_node->connection_count; i++) {
        if (from_node->connections[i]->node_id == to_node_id) {
            // 找到，移除
            for (int j = i; j < from_node->connection_count - 1; j++) {
                from_node->connections[j] = from_node->connections[j + 1];
                from_node->connection_weights[j] = from_node->connection_weights[j + 1];
            }
            from_node->connection_count--;

            // 从目标节点也移除反向连接
            ReasoningNode* to_node = sub->net->nodes[to_node_id];
            if (to_node) {
                for (int j = 0; j < to_node->connection_count; j++) {
                    if (to_node->connections[j]->node_id == from_node_id) {
                        for (int k = j; k < to_node->connection_count - 1; k++) {
                            to_node->connections[k] = to_node->connections[k + 1];
                            to_node->connection_weights[k] = to_node->connection_weights[k + 1];
                        }
                        to_node->connection_count--;
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
    for (int i = 0; i < from_node->connection_count; i++) {
        if (from_node->connections[i]->node_id == to_node_id) {
            from_node->connection_weights[i] = CLAMP(new_weight, 0.0f, 1.0f);
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

        // 检查节点数量
        if (check_cooldown(&config->node_count_trigger)) {
            float usage = (float)net->node_count / config->max_nodes_per_topology;
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

        // 检查负载
        if (check_cooldown(&config->load_trigger)) {
            float load = compute_load_factor(net, config->max_nodes_per_topology);
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

    // 计算需要扩展的容量
    int current_capacity = net->max_nodes;
    int new_capacity = current_capacity + config->growth_increment;

    // 检查限制
    if (new_capacity > config->max_nodes_per_topology) {
        new_capacity = config->max_nodes_per_topology;
    }

    if (new_capacity <= current_capacity) {
        return -1;  // 无法再扩展
    }

    // 扩容
    ReasoningNode** new_nodes = (ReasoningNode**)realloc(
        net->nodes, new_capacity * sizeof(ReasoningNode*));
    if (!new_nodes) return -1;

    // 初始化新空间
    for (int i = current_capacity; i < new_capacity; i++) {
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
    if (!master) return -1;

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
    int new_capacity = net->max_nodes - config->growth_increment;
    if (new_capacity < net->node_count) {
        new_capacity = net->node_count + 10;  // 保留一些余量
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
                if (node && node->connection_count < min_degree) {
                    min_degree = node->connection_count;
                    min_degree_node = n;
                }
            }

            if (min_degree_node < 0) break;

            // 执行实际迁移
            ReasoningNode* node_to_move = src->net->nodes[min_degree_node];
            if (!node_to_move) break;

            // 检查目标拓扑是否有空间
            if (dst->net->node_count >= dst->net->max_nodes) {
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
                if (node && node->connection_count == 0) {
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

int prune_low_connectivity(MasterTopology* master, int topo_id,
                          int min_connections) {
    if (!master || min_connections < 0) return 0;

    SubTopology* sub = master_get_sub_topology(master, topo_id);
    if (!sub || !sub->net) return 0;

    int removed = 0;
    for (int i = 0; i < sub->net->node_count; i++) {
        ReasoningNode* node = sub->net->nodes[i];
        if (node && node->connection_count < min_connections) {
            if (remove_node_dynamic(master, topo_id, i, false) == 0) {
                removed++;
            }
        }
    }

    return removed;
}

int prune_isolated_nodes(MasterTopology* master, int topo_id) {
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

                for (int j = 0; j < node->connection_count; j++) {
                    float delta = config->learning_rate * node->activation * 
                                  node->connections[j]->activation;
                    node->connection_weights[j] += delta;
                    node->connection_weights[j] = CLAMP(node->connection_weights[j], 0.0f, 1.0f);
                }
            }
            break;
        }

        case WEIGHT_UPDATE_GRADIENT: {
            // 梯度下降更新
            for (int i = 0; i < sub->net->node_count; i++) {
                ReasoningNode* node = sub->net->nodes[i];
                if (!node) continue;

                for (int j = 0; j < node->connection_count; j++) {
                    // 简化梯度更新
                    float gradient = node->activation - node->connection_weights[j];
                    node->connection_weights[j] -= config->learning_rate * gradient;
                    node->connection_weights[j] = CLAMP(node->connection_weights[j], 0.0f, 1.0f);
                }
            }
            break;
        }

        case WEIGHT_UPDATE_RULE_BASED: {
            // 基于规则的更新
            for (int i = 0; i < sub->net->node_count; i++) {
                ReasoningNode* node = sub->net->nodes[i];
                if (!node) continue;

                for (int j = 0; j < node->connection_count; j++) {
                    // 激活高的连接增强，低的衰减
                    float target_weight = node->activation * 
                                         node->connections[j]->activation;
                    float diff = target_weight - node->connection_weights[j];
                    node->connection_weights[j] += config->learning_rate * diff;
                    node->connection_weights[j] = CLAMP(node->connection_weights[j], 0.0f, 1.0f);
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

                for (int j = 0; j < node->connection_count; j++) {
                    float hebbian = config->learning_rate * node->activation * 
                                   node->connections[j]->activation;
                    float gradient = node->activation - node->connection_weights[j];
                    float update = alpha * hebbian + (1 - alpha) * gradient;
                    node->connection_weights[j] -= config->learning_rate * update;
                    node->connection_weights[j] = CLAMP(node->connection_weights[j], 0.0f, 1.0f);
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
    int connection_count = 0;

    for (int i = 0; i < sub->net->node_count; i++) {
        ReasoningNode* node = sub->net->nodes[i];
        if (!node) continue;

        for (int j = 0; j < node->connection_count; j++) {
            node->connection_weights[j] *= decay_factor;
            if (node->connection_weights[j] < config->min_connection_weight) {
                node->connection_weights[j] = 0.0f;
            }
            total_strength += node->connection_weights[j];
            connection_count++;
        }
    }

    return (connection_count > 0) ? total_strength / connection_count : 0.0f;
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
            return 0;
        }
    }

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
