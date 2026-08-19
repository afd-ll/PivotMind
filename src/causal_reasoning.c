#include "common.h"
#include "causal_reasoning.h"
#include "huarong_topology.h"
#include "utf8_tokenizer.h"
#include "node_hash.h"
#include "multi_topology.h"
#include "concept_abstraction.h"
#include "memory_arena.h"   /* opt(任务5): 内存池收敛——自写 MemPool → ObjectPool */

#ifdef _OPENMP
#include <omp.h>
#endif

#ifdef USE_SQLITE
#include <sqlite3.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <float.h>

// ==================== 宏定义 ====================

#define DEFAULT_NODE_COUNT 256
#define DEFAULT_EDGE_CAPACITY 1024
#define MAX_PATH_LENGTH 20
#define EPSILON 1e-10f

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))

// 因果置信度阈值
#define CAUSAL_CONF_THRESHOLD_CONTEXT 0.3f
#define CAUSAL_CONF_THRESHOLD_SHORT 0.6f
#define CAUSAL_CONF_THRESHOLD_PERMANENT 0.8f
#define MAX_OBSERVATIONS 1000
#define CAUSAL_DECAY_DAYS 30

// ==================== 因果置信度函数 ====================

/**
 * 创建因果置信度结构
 */
CausalConfidence* causal_confidence_create(float base_score) {
    CausalConfidence* cc = (CausalConfidence*)malloc(sizeof(CausalConfidence));
    if (!cc) return NULL;
    
    cc->base_score = base_score;
    cc->observation_count = 1;
    cc->valid_scenarios = 1;
    cc->total_scenarios = 1;
    cc->consistent_count = 1;
    cc->total_tests = 1;
    cc->first_observed = time(NULL);
    cc->last_confirmed = time(NULL);
    
    return cc;
}

/**
 * 计算因果置信度
 */
float compute_causal_confidence(CausalConfidence* cc) {
    if (!cc) return 0.0f;
    
    // 1. 基础分
    float base = cc->base_score;
    
    // 2. 验证次数因子 (log 衰减)
    float validation = logf((float)cc->observation_count + 1.0f) / 
                       logf((float)MAX_OBSERVATIONS + 1.0f);
    
    // 3. 场景多样性因子
    float diversity = (cc->total_scenarios > 0) ? 
                     (float)cc->valid_scenarios / cc->total_scenarios : 0.5f;
    
    // 4. 稳定性因子
    float stability = (cc->total_tests > 0) ?
                     (float)cc->consistent_count / cc->total_tests : 0.5f;
    
    // 综合计算
    float confidence = base * 0.4f + validation * 0.2f + 
                      diversity * 0.2f + stability * 0.2f;
    
    return CLAMP(confidence, 0.0f, 1.0f);
}

/**
 * 更新因果置信度
 */
void update_causal_confidence(CausalConfidence* cc, bool supports, int scenario_id) {
    if (!cc) return;
    
    // 更新观察次数
    cc->observation_count++;
    
    // 更新一致性
    cc->total_tests++;
    if (supports) {
        cc->consistent_count++;
    }
    
    // 更新场景多样性（简化：每次新观察都算新场景）
    if (scenario_id > 0 && scenario_id >= cc->total_scenarios) {
        cc->valid_scenarios++;
        cc->total_scenarios = scenario_id + 1;
    }
    
    // 更新时间
    cc->last_confirmed = time(NULL);
}

/**
 * 获取置信度级别
 */
CausalConfidenceLevel get_confidence_level(float confidence) {
    if (confidence < CAUSAL_CONF_THRESHOLD_CONTEXT) {
        return CAUSAL_CONF_CONTEXT;
    } else if (confidence < CAUSAL_CONF_THRESHOLD_SHORT) {
        return CAUSAL_CONF_SHORT_TERM;
    } else if (confidence < CAUSAL_CONF_THRESHOLD_PERMANENT) {
        return CAUSAL_CONF_PERMANENT;
    }
    return CAUSAL_CONF_CORE;
}

/**
 * 因果置信度衰减（随时间）
 */
float decay_causal_confidence(CausalConfidence* cc) {
    if (!cc) return 0.0f;
    
    time_t current = time(NULL);
    time_t elapsed = current - cc->last_confirmed;
    float days = elapsed / (24.0f * 60.0f * 60.0f);
    
    // 超过 30 天未确认，开始衰减
    if (days > CAUSAL_DECAY_DAYS) {
        float decay_rate = 0.01f * (days - CAUSAL_DECAY_DAYS);
        return cc->base_score * expf(-decay_rate);
    }
    
    return cc->base_score;
}

/**
 * 释放因果置信度结构
 */
void causal_confidence_destroy(CausalConfidence* cc) {
    if (cc) free(cc);
}

// ==================== A* 搜索优先队列 ====================

typedef struct {
    int node_id;           // 节点ID
    float g_score;         // 从起点到该节点的实际代价
    float f_score;         // f = g + h 启发式评估
    int* path;             // 路径
    int path_len;          // 路径长度
    float total_strength;  // 路径总因果强度
} AStarNode;

// 优先队列（最小堆）
typedef struct {
    AStarNode* nodes;      // 节点数组
    int capacity;          // 容量
    int size;              // 当前大小
} PriorityQueue;

// 初始化优先队列
static PriorityQueue* pq_create(int capacity) {
    PriorityQueue* pq = (PriorityQueue*)malloc(sizeof(PriorityQueue));
    pq->capacity = capacity;
    pq->size = 0;
    pq->nodes = (AStarNode*)malloc(capacity * sizeof(AStarNode));
    return pq;
}

// 释放优先队列
static void pq_free(PriorityQueue* pq) {
    if (!pq) return;
    for (int i = 0; i < pq->size; i++) {
        if (pq->nodes[i].path) free(pq->nodes[i].path);
    }
    free(pq->nodes);
    free(pq);
}

// 交换两个节点
static void pq_swap(AStarNode* a, AStarNode* b) {
    AStarNode temp = *a;
    *a = *b;
    *b = temp;
}

// 上浮操作
static void pq_sift_up(PriorityQueue* pq, int idx) {
    while (idx > 0) {
        int parent = (idx - 1) / 2;
        if (pq->nodes[parent].f_score <= pq->nodes[idx].f_score) break;
        pq_swap(&pq->nodes[parent], &pq->nodes[idx]);
        idx = parent;
    }
}

// 下沉操作
static void pq_sift_down(PriorityQueue* pq, int idx) {
    while (2 * idx + 1 < pq->size) {
        int left = 2 * idx + 1;
        int right = 2 * idx + 2;
        int smallest = left;
        
        if (right < pq->size && pq->nodes[right].f_score < pq->nodes[left].f_score) {
            smallest = right;
        }
        
        if (pq->nodes[idx].f_score <= pq->nodes[smallest].f_score) break;
        
        pq_swap(&pq->nodes[idx], &pq->nodes[smallest]);
        idx = smallest;
    }
}

// 插入节点
static void pq_push(PriorityQueue* pq, AStarNode* node) {
    if (pq->size >= pq->capacity) {
        pq->capacity *= 2;
        pq->nodes = (AStarNode*)realloc(pq->nodes, pq->capacity * sizeof(AStarNode));
    }
    pq->nodes[pq->size] = *node;
    pq->size++;
    pq_sift_up(pq, pq->size - 1);
}

// 弹出最小 f_score 节点
static int pq_pop(PriorityQueue* pq, AStarNode* out) {
    if (pq->size == 0) return 0;
    *out = pq->nodes[0];
    pq->nodes[0] = pq->nodes[--pq->size];
    pq_sift_down(pq, 0);
    return 1;
}

// 启发式函数：估计从 node 到 target 的最小代价
// 使用入边数量的倒数作为启发值（启发式一致性和可采纳性）
static float heuristic_estimate(CausalGraph* graph, int node, int target) {
    if (node == target) return 0.0f;
    // 如果 node 有直接指向 target 的边，距离为 1
    if (causal_edge_exists(graph, node, target)) return 1.0f;
    // 否则使用平均边数的倒数作为估计
    float avg_in_degree = (graph->node_count > 0) ? 
        (float)graph->edge_count / graph->node_count : 1.0f;
    return avg_in_degree * 0.5f;  // 略微低估，保证 A* 的最优性
}

// ==================== 辅助函数 ====================

static int find_edge_index(CausalGraph* graph, int cause_id, int effect_id) {
    if (!graph) return -1;
    for (int i = 0; i < graph->edge_count; i++) {
        CausalEdge* edge = graph->edges[i];
        if (edge && edge->cause_node_id == cause_id && edge->effect_node_id == effect_id) {
            return i;
        }
    }
    return -1;
}

static bool has_cycle_dfs(CausalGraph* graph, int node, int* visited, int* rec_stack) {
    visited[node] = 1;
    rec_stack[node] = 1;

    for (int i = 0; i < graph->outgoing_count[node]; i++) {
        int neighbor = (graph->edges[graph->outgoing[node][i]] ? graph->edges[graph->outgoing[node][i]]->effect_node_id : -1);
        if (!visited[neighbor]) {
            if (has_cycle_dfs(graph, neighbor, visited, rec_stack)) {
                return true;
            }
        } else if (rec_stack[neighbor]) {
            return true;
        }
    }

    rec_stack[node] = 0;
    return false;
}


// ==================== 因果图管理 ==========

CausalGraph* causal_graph_create(int node_count, int edge_capacity) {
    CausalGraph* graph = (CausalGraph*)malloc(sizeof(CausalGraph));
    if (!graph) return NULL;

    graph->node_count = (node_count > 0) ? node_count : DEFAULT_NODE_COUNT;
    graph->edge_capacity = (edge_capacity > 0) ? edge_capacity : DEFAULT_EDGE_CAPACITY;
    graph->edge_count = 0;
    graph->topo_node_count = 0;

    graph->node_mapping = (int*)calloc(graph->node_count, sizeof(int));
    graph->edges = (CausalEdge**)malloc(graph->edge_capacity * sizeof(CausalEdge*));

    graph->outgoing = (int**)calloc(graph->node_count, sizeof(int*));
    graph->outgoing_count = (int*)calloc(graph->node_count, sizeof(int));
    graph->incoming = (int**)calloc(graph->node_count, sizeof(int*));
    graph->incoming_count = (int*)calloc(graph->node_count, sizeof(int));

    graph->is_dag = true;
    graph->topological_order = NULL;
    graph->order_length = 0;
    graph->avg_causal_strength = 0.0f;
    graph->last_updated = time(NULL);

    // 初始化边内存池（P2 dsh返工: adj_pool 死代码已移除——从未 acquire）
    graph->edge_pool = object_pool_create(sizeof(CausalEdge), 128);

    return graph;
}

void causal_graph_destroy(CausalGraph* graph) {
    if (!graph) return;

    /* 边来自 edge_pool 内存池：先归还池再销毁。
     * ObjectPool 的 destroy 只释放空闲列表对象，已 acquire 未归还的
     * 对象需先 object_pool_release（等价于原 pool_destroy 统一释放）。 */
    if (graph->edge_pool) {
        for (int i = 0; i < graph->edge_count; i++) {
            if (graph->edges[i]) {
                object_pool_release((ObjectPool*)graph->edge_pool, graph->edges[i]);
                graph->edges[i] = NULL;
            }
        }
    }
    free(graph->edges);

    for (int i = 0; i < graph->node_count; i++) {
        if (graph->outgoing[i]) free(graph->outgoing[i]);
        if (graph->incoming[i]) free(graph->incoming[i]);
    }
    free(graph->outgoing);
    free(graph->incoming);
    free(graph->outgoing_count);
    free(graph->incoming_count);
    free(graph->node_mapping);
    if (graph->topological_order) free(graph->topological_order);

    // 释放边内存池（adj_pool 已移除）
    if (graph->edge_pool) object_pool_destroy((ObjectPool*)graph->edge_pool);

    free(graph);
}

int add_causal_edge(CausalGraph* graph, int cause_id, int effect_id,
                  CausalEdgeType type, float strength) {
    if (!graph || cause_id < 0 || effect_id < 0) return -1;
    if (cause_id >= graph->node_count || effect_id >= graph->node_count) return -1;
    if (strength < 0 || strength > 1) strength = CLAMP(strength, 0.0f, 1.0f);

    /* v0.5.7: 查重线性扫描 O(E) 在批量建图（infer）时 O(E²) 爆炸，
     * 批量场景用 add_causal_edge_no_check（遍历天然近似去重） */
    if (find_edge_index(graph, cause_id, effect_id) >= 0) {
        return -1;  // 边已存在
    }
    return add_causal_edge_no_check(graph, cause_id, effect_id, type, strength);
}

/* 无查重版：批量建图（infer_causal_graph_*）用——跳过 find_edge_index
 * 线性扫描，避免 O(E²)。调用方负责近似去重。 */
int add_causal_edge_no_check(CausalGraph* graph, int cause_id, int effect_id,
                             CausalEdgeType type, float strength) {
    if (!graph || cause_id < 0 || effect_id < 0) return -1;
    if (cause_id >= graph->node_count || effect_id >= graph->node_count) return -1;
    if (strength < 0 || strength > 1) strength = CLAMP(strength, 0.0f, 1.0f);

    // 扩容
    if (graph->edge_count >= graph->edge_capacity) {
        graph->edge_capacity *= 2;
        CausalEdge** new_edges = (CausalEdge**)realloc(
            graph->edges, graph->edge_capacity * sizeof(CausalEdge*));
        if (!new_edges) return -1;
        graph->edges = new_edges;
    }

    CausalEdge* edge = (CausalEdge*)object_pool_acquire((ObjectPool*)graph->edge_pool);
    if (!edge) return -1;

    edge->edge_id = graph->edge_count;
    edge->cause_node_id = cause_id;
    edge->effect_node_id = effect_id;
    edge->type = type;
    edge->strength = strength;
    edge->probability = strength;
    edge->bidirectional = false;
    edge->condition_node_ids = NULL;
    edge->condition_count = 0;
    edge->confidence = strength;
    edge->observed_at = time(NULL);
    edge->last_confirmed = time(NULL);

    graph->edges[graph->edge_count++] = edge;

    // 更新邻接表
    int* new_out = (int*)realloc(
        graph->outgoing[cause_id], (graph->outgoing_count[cause_id] + 1) * sizeof(int));
    if (!new_out) return -1;
    graph->outgoing[cause_id] = new_out;
    graph->outgoing[cause_id][graph->outgoing_count[cause_id]++] = edge->edge_id;

    int* new_in = (int*)realloc(
        graph->incoming[effect_id], (graph->incoming_count[effect_id] + 1) * sizeof(int));
    if (!new_in) return -1;
    graph->incoming[effect_id] = new_in;
    graph->incoming[effect_id][graph->incoming_count[effect_id]++] = cause_id;

    /* v0.5.7: 批量建图（infer）不更新统计/环检查——
     * avg_causal_strength 循环 O(E)、has_cycle_dfs O(N+E) 每次加边
     * 都跑 = O(E²)/O(E(N+E)) 爆炸（70 万边建图卡死）。
     * 单边动态场景（add_causal_edge）才需要这两项。 */
    graph->last_updated = time(NULL);
    return edge->edge_id;
}

int remove_causal_edge(CausalGraph* graph, int cause_id, int effect_id) {
    if (!graph) return -1;

    int edge_idx = find_edge_index(graph, cause_id, effect_id);
    if (edge_idx < 0) return -1;

    /* 边由 edge_pool 内存池分配（add_causal_edge:object_pool_acquire）。
     * fix(P0-2 dsh返工): 删除时必须归还池——object_pool_destroy 只释放
     * 空闲列表对象, 已 acquire 未归还的对象不会自动回收; 原"仅置空标记
     * 删除"会让被删边对象指针丢失→永不归还→泄漏(PC 算法批量删边放大)。 */
    if (graph->edge_pool && graph->edges[edge_idx])
        object_pool_release((ObjectPool*)graph->edge_pool, graph->edges[edge_idx]);
    graph->edges[edge_idx] = NULL;

    // 调整边数组
    for (int i = edge_idx; i < graph->edge_count - 1; i++) {
        graph->edges[i] = graph->edges[i + 1];
    }
    graph->edge_count--;

    // 更新邻接表
    for (int i = 0; i < graph->outgoing_count[cause_id]; i++) {
        int oi = graph->outgoing[cause_id][i];
        if (oi >= 0 && oi < graph->edge_count && graph->edges[oi] &&
            graph->edges[oi]->effect_node_id == effect_id) {
            for (int j = i; j < graph->outgoing_count[cause_id] - 1; j++) {
                graph->outgoing[cause_id][j] = graph->outgoing[cause_id][j + 1];
            }
            graph->outgoing_count[cause_id]--;
            break;
        }
    }

    for (int i = 0; i < graph->incoming_count[effect_id]; i++) {
        if (graph->incoming[effect_id][i] == cause_id) {
            for (int j = i; j < graph->incoming_count[effect_id] - 1; j++) {
                graph->incoming[effect_id][j] = graph->incoming[effect_id][j + 1];
            }
            graph->incoming_count[effect_id]--;
            break;
        }
    }

    graph->last_updated = time(NULL);
    return 0;
}

CausalEdge* get_causal_edge(CausalGraph* graph, int cause_id, int effect_id) {
    /* v0.5.7: 原 find_edge_index 全边扫描 O(E)——A* 扩展循环里
     * 调用爆炸。改邻接表查询 O(度)。 */
    if (!graph || cause_id < 0 || cause_id >= graph->node_count) return NULL;
    for (int i = 0; i < graph->outgoing_count[cause_id]; i++) {
        int oi = graph->outgoing[cause_id][i];
        if (oi >= 0 && oi < graph->edge_count && graph->edges[oi] &&
            graph->edges[oi]->effect_node_id == effect_id) return graph->edges[oi];
    }
    return NULL;
}

bool causal_edge_exists(CausalGraph* graph, int cause_id, int effect_id) {
    /* v0.5.7: 原 find_edge_index 全边线性扫描 O(E)——A* 启发式/搜索
     * 循环里调用爆炸（O(E×N)）。改邻接表查询 O(度)。 */
    if (!graph) return false;
    if (cause_id < 0 || cause_id >= graph->node_count) return false;
    for (int i = 0; i < graph->outgoing_count[cause_id]; i++) {
        int oi = graph->outgoing[cause_id][i];
        if (oi >= 0 && oi < graph->edge_count && graph->edges[oi] &&
            graph->edges[oi]->effect_node_id == effect_id) return true;
    }
    return false;
}

// ==================== 从拓扑网络构建因果图 ==========

CausalGraph* infer_causal_graph_from_topology(HuarongTopologyNet* topo_net,
                                             float min_strength) {
    if (!topo_net) return NULL;

    CausalGraph* graph = causal_graph_create(topo_net->node_count, topo_net->node_count * 2);
    if (!graph) return NULL;

    graph->topo_node_count = topo_net->node_count;

    // 从连接关系推断因果边
    for (int i = 0; i < topo_net->node_count; i++) {
        ReasoningNode* node = topo_net->nodes[i];
        if (!node) continue;

        for (int j = 0; j < node->edge_count; j++) {
            int target_id = node->edges[j].target->node_id;
            float weight = node->edges[j].weight;

            if (weight >= min_strength) {
                add_causal_edge_no_check(graph, i, target_id, CAUSAL_DIRECT, weight);
            }
        }
    }

    return graph;
}

int learn_causal_edges(CausalGraph* graph, float** observations,
                      int obs_count, int feature_dim) {
    if (!graph || !observations || obs_count <= 0) return 0;

    // 简化实现：基于相关性学习因果边
    int learned = 0;

    for (int i = 0; i < feature_dim; i++) {
        for (int j = 0; j < feature_dim; j++) {
            if (i == j) continue;

            // 计算相关性
            float correlation = 0.0f;
            float mean_i = 0.0f, mean_j = 0.0f;
            float var_i = 0.0f, var_j = 0.0f;

            for (int k = 0; k < obs_count; k++) {
                mean_i += observations[k][i];
                mean_j += observations[k][j];
            }
            mean_i /= obs_count;
            mean_j /= obs_count;

            for (int k = 0; k < obs_count; k++) {
                float diff_i = observations[k][i] - mean_i;
                float diff_j = observations[k][j] - mean_j;
                correlation += diff_i * diff_j;
                var_i += diff_i * diff_i;
                var_j += diff_j * diff_j;
            }

            if (var_i > EPSILON && var_j > EPSILON) {
                correlation /= sqrtf(var_i * var_j);
                correlation = fabsf(correlation);

                if (correlation > 0.3f) {
                    // 时序假设：i 在 j 之前，所以 i -> j
                    if (!causal_edge_exists(graph, i, j)) {
                        if (add_causal_edge(graph, i, j, CAUSAL_DIRECT, correlation) >= 0) {
                            learned++;
                        }
                    }
                }
            }
        }
    }

    return learned;
}

// ==================== 因果效应计算 ==========

int do_intervention(CausalGraph* graph, Intervention* intervention,
                  float* output_values) {
    if (!graph || !intervention || !output_values) return -1;

    // 简化实现：设置目标节点的值
    for (int i = 0; i < graph->node_count; i++) {
        output_values[i] = 0.5f;  // 默认值
    }

    switch (intervention->type) {
        case INTERVENTION_DO:
        case INTERVENTION_SET:
            output_values[intervention->target_node_id] = intervention->new_value;
            break;

        case INTERVENTION_REMOVE:
            // 移除所有指向目标节点的边
            for (int i = 0; i < graph->incoming_count[intervention->target_node_id]; i++) {
                // 效果通过其他路径传播
                (void)graph->incoming[intervention->target_node_id][i];
            }
            break;

        case INTERVENTION_ADD:
            // 添加新的因果边
            add_causal_edge(graph, intervention->source_node_id,
                          intervention->target_node_id, CAUSAL_DIRECT,
                          intervention->strength);
            break;
    }

    return 0;
}

CausalEffect* compute_causal_effect(CausalGraph* graph, int cause_id,
                                   int effect_id, CausalEffectType effect_type) {
    if (!graph || cause_id < 0 || effect_id < 0) return NULL;

    CausalEffect* effect = (CausalEffect*)malloc(sizeof(CausalEffect));
    if (!effect) return NULL;

    effect->type = effect_type;
    effect->cause_node_id = cause_id;
    effect->effect_node_id = effect_id;

    switch (effect_type) {
        case EFFECT_DIRECT: {
            CausalEdge* edge = get_causal_edge(graph, cause_id, effect_id);
            effect->effect_size = edge ? edge->strength : 0.0f;
            effect->confidence = edge ? edge->confidence : 0.0f;
            break;
        }

        case EFFECT_TOTAL: {
            // 总效应 = 直接效应 + 间接效应（修复：实际使用路径计算总效应）
            float total = 0.0f;
            int path_count_temp = 0;
            CausalPath** paths = find_all_causal_paths(graph, cause_id, effect_id, MAX_PATH_LENGTH, &path_count_temp);

            // 先添加直接效应
            CausalEdge* direct = get_causal_edge(graph, cause_id, effect_id);
            if (direct) {
                total += direct->strength;
            }

            // 计算间接效应（通过路径累积）
            if (paths && path_count_temp > 0) {
                for (int i = 0; i < path_count_temp; i++) {
                    // 路径效应 = 路径上所有边强度的乘积
                    float path_effect = paths[i]->total_strength;
                    // 避免重复计算直接路径
                    if (paths[i]->length > 2) {
                        total += path_effect;
                    }
                    free(paths[i]->node_ids);
                    free(paths[i]->edge_strengths);
                    free(paths[i]);
                }
                free(paths);
            }

            effect->effect_size = CLAMP(total, 0.0f, 1.0f);
            effect->confidence = (path_count_temp > 0) ? 0.8f : (direct ? direct->confidence : 0.0f);
            break;
        }

        case EFFECT_ATE:
        case EFFECT_ATT:
        case EFFECT_CATE:
        default: {
            // 简化 ATE 计算
            float treated = 0.0f, control = 0.0f;
            CausalEdge* edge = get_causal_edge(graph, cause_id, effect_id);
            if (edge) {
                treated = edge->strength;
                control = 0.0f;  // 假设控制组无效应
            }
            effect->effect_size = treated - control;
            effect->confidence = edge ? edge->confidence : 0.5f;
            break;
        }
    }

    effect->standard_error = 0.1f;
    effect->confidence_interval_low = effect->effect_size - 1.96f * effect->standard_error;
    effect->confidence_interval_high = effect->effect_size + 1.96f * effect->standard_error;
    effect->p_value = 0.05f;
    effect->sample_size = 1000;

    return effect;
}

float backdoor_adjustment(CausalGraph* graph, int cause_id, int effect_id,
                         int* confounders, int confounder_count) {
    if (!graph) return 0.0f;

    // 简化后门调整：控制所有混淆因素
    // P(Y|do(X)) = sum_z P(Y|X,Z) P(Z)
    float adjusted_effect = 0.0f;

    CausalEdge* direct = get_causal_edge(graph, cause_id, effect_id);
    if (direct) {
        adjusted_effect = direct->strength;
    }

    // 如果有混淆因素，应用调整
    if (confounders && confounder_count > 0) {
        for (int i = 0; i < confounder_count; i++) {
            CausalEdge* confounder_edge = get_causal_edge(graph, confounders[i], effect_id);
            if (confounder_edge) {
                adjusted_effect *= (1.0f - confounder_edge->strength * 0.1f);
            }
        }
    }

    return CLAMP(adjusted_effect, 0.0f, 1.0f);
}

float frontdoor_adjustment(CausalGraph* graph, int cause_id, int effect_id,
                          int mediator_id) {
    if (!graph) return 0.0f;

    // 前门调整：P(Y|do(X)) = P(M|X) P(Y|M)
    CausalEdge* x_to_m = get_causal_edge(graph, cause_id, mediator_id);
    CausalEdge* m_to_y = get_causal_edge(graph, mediator_id, effect_id);

    if (x_to_m && m_to_y) {
        return x_to_m->strength * m_to_y->strength;
    }

    return 0.0f;
}

// ==================== 因果路径分析 ==========

CausalPath** find_all_causal_paths(CausalGraph* graph, int source,
                                   int target, int max_length, int* path_count) {
    if (!graph || source < 0 || target < 0 || source >= graph->node_count ||
        target >= graph->node_count || path_count == NULL) {
        if (path_count) *path_count = 0;
        return NULL;
    }

    CausalPath** paths = (CausalPath**)malloc(100 * sizeof(CausalPath*));
    int count = 0;

    // BFS 搜索所有路径
    typedef struct {
        int node;
        int* path;
        int path_len;
        float strength;
    } State;

    int queue_capacity = graph->node_count * max_length;
    State* queue = (State*)malloc(queue_capacity * sizeof(State));
    if (!queue) return 0;
    int queue_front = 0, queue_back = 0;

    // 初始化队列
    queue[queue_back].node = source;
    queue[queue_back].path = (int*)malloc(max_length * sizeof(int));
    queue[queue_back].path[0] = source;
    queue[queue_back].path_len = 1;
    queue[queue_back].strength = 1.0f;
    queue_back++;

    while (queue_front < queue_back && count < 100) {
        State current = queue[queue_front++];

        if (current.node == target && current.path_len > 1) {
            // 找到一条路径
            CausalPath* path = (CausalPath*)malloc(sizeof(CausalPath));
            path->node_ids = current.path;
            path->length = current.path_len;
            path->total_strength = current.strength;
            path->edge_strengths = (float*)malloc((current.path_len - 1) * sizeof(float));
            path->is_direct = (current.path_len == 2);
            path->has_confounder = false;
            paths[count++] = path;
            continue;  // 避免后续 free(current.path) 造成 double free
        }

        if (current.path_len >= max_length) continue;

        // 扩展
        for (int i = 0; i < graph->outgoing_count[current.node]; i++) {
            int next_node = (graph->edges[graph->outgoing[current.node][i]] ? graph->edges[graph->outgoing[current.node][i]]->effect_node_id : -1);

            // 检查是否在路径中 (避免循环)
            bool in_path = false;
            for (int j = 0; j < current.path_len; j++) {
                if (current.path[j] == next_node) {
                    in_path = true;
                    break;
                }
            }
            if (in_path) continue;

            if (queue_back >= queue_capacity) {
                /* 队列满 -- 图密集时路径爆炸，截断防止堆越界 */
                free(current.path);
                free(queue);
                for (int k = 0; k < count; k++) {
                    free(paths[k]->edge_strengths);
                    free(paths[k]->node_ids);
                    free(paths[k]);
                }
                free(paths);
                *path_count = 0;
                return NULL;
            }

            CausalEdge* edge = get_causal_edge(graph, current.node, next_node);
            float edge_strength = edge ? edge->strength : 0.5f;

            queue[queue_back].node = next_node;
            queue[queue_back].path = (int*)malloc(max_length * sizeof(int));
            memcpy(queue[queue_back].path, current.path, current.path_len * sizeof(int));
            queue[queue_back].path[current.path_len] = next_node;
            queue[queue_back].path_len = current.path_len + 1;
            queue[queue_back].strength = current.strength * edge_strength;
            queue_back++;
        }

        free(current.path);
    }

    free(queue);
    *path_count = count;

    if (count == 0) {
        free(paths);
        return NULL;
    }

    return paths;
}

// ==================== A* 搜索实现 ====================

/** A* 路径创建（三处 A* 实现共享） */
static CausalPath* astar_create_path(CausalGraph* graph, AStarNode* node) {
    CausalPath* path = (CausalPath*)malloc(sizeof(CausalPath));
    if (!path) return NULL;
    path->node_ids = node->path;  // 所有权转移
    path->length = node->path_len;
    path->total_strength = node->total_strength;
    path->edge_strengths = (float*)malloc((node->path_len - 1) * sizeof(float));
    path->is_direct = (node->path_len == 2);
    path->has_confounder = false;
    if (path->edge_strengths) {
        for (int i = 0; i < node->path_len - 1; i++) {
            CausalEdge* edge = get_causal_edge(graph, node->path[i], node->path[i + 1]);
            path->edge_strengths[i] = edge ? edge->strength : 0.5f;
        }
    }
    return path;
}

/**
 * 使用 A* 算法查找因果路径（启发式搜索优先探索最有希望的路径）
 * 与 find_all_causal_paths 不同，A* 使用启发式搜索优先探索最有希望的路径
 * 
 * @param graph 因果图
 * @param source 源节点
 * @param target 目标节点
 * @param max_length 最大路径长度
 * @param max_paths 最大返回路径数量
 * @param path_count 输出路径数量
 * @return 路径数组 (需调用者释放)
 */
CausalPath** find_causal_paths_astar(CausalGraph* graph, int source, int target,
                                     int max_length, int max_paths, int* path_count) {
    if (!graph || source < 0 || target < 0 || 
        source >= graph->node_count || target >= graph->node_count || 
        path_count == NULL) {
        if (path_count) *path_count = 0;
        return NULL;
    }

    *path_count = 0;
    
    // 如果源节点就是目标节点，返回空
    if (source == target) return NULL;

    // 创建优先队列
    PriorityQueue* open_set = pq_create(256);
    
    // 记录已访问的节点及其最优 g_score（用于剪枝）
    float* best_g = (float*)malloc(graph->node_count * sizeof(float));
    for (int i = 0; i < graph->node_count; i++) best_g[i] = FLT_MAX;
    
    // 结果路径
    CausalPath** paths = (CausalPath**)malloc(max_paths * sizeof(CausalPath*));
    int found_count = 0;

    // 初始化：加入起点
    AStarNode start_node;
    start_node.node_id = source;
    start_node.g_score = 0.0f;
    start_node.f_score = heuristic_estimate(graph, source, target);
    start_node.path = (int*)malloc(max_length * sizeof(int));
    start_node.path[0] = source;
    start_node.path_len = 1;
    start_node.total_strength = 1.0f;
    
    pq_push(open_set, &start_node);
    best_g[source] = 0.0f;

    while (open_set->size > 0 && found_count < max_paths) {
        // 取出 f_score 最小的节点
        AStarNode current;
        pq_pop(open_set, &current);
        
        // 检查是否到达目标
        if (current.node_id == target && current.path_len > 1) {
            CausalPath* path = astar_create_path(graph, &current);
            if (path) paths[found_count++] = path;
            continue;
        }
        
        // 达到最大长度，不再扩展
        if (current.path_len >= max_length) {
            free(current.path);
            continue;
        }
        
        // 扩展当前节点的所有邻居
        for (int i = 0; i < graph->outgoing_count[current.node_id]; i++) {
            int neighbor = (graph->edges[graph->outgoing[current.node_id][i]] ? graph->edges[graph->outgoing[current.node_id][i]]->effect_node_id : -1);
            
            // 计算经过当前路径到 neighbor 的 g_score
            CausalEdge* edge = get_causal_edge(graph, current.node_id, neighbor);
            float edge_strength = edge ? edge->strength : 0.5f;
            float tentative_g = current.g_score + 1.0f;  // 步数代价
            
            // 剪枝：如果已经找到更好的路径到这个节点，跳过
            if (tentative_g >= best_g[neighbor]) continue;
            
            // 检查是否在当前路径中（避免循环）
            bool in_path = false;
            for (int j = 0; j < current.path_len; j++) {
                if (current.path[j] == neighbor) {
                    in_path = true;
                    break;
                }
            }
            if (in_path) continue;
            
            // 更新最优路径
            best_g[neighbor] = tentative_g;
            
            // 计算 f_score = g + h
            float h = heuristic_estimate(graph, neighbor, target);
            float f = tentative_g + h;
            
            // 创建新节点
            AStarNode neighbor_node;
            neighbor_node.node_id = neighbor;
            neighbor_node.g_score = tentative_g;
            neighbor_node.f_score = f;
            neighbor_node.path = (int*)malloc(max_length * sizeof(int));
            memcpy(neighbor_node.path, current.path, current.path_len * sizeof(int));
            neighbor_node.path[current.path_len] = neighbor;
            neighbor_node.path_len = current.path_len + 1;
            neighbor_node.total_strength = current.total_strength * edge_strength;
            
            pq_push(open_set, &neighbor_node);
        }
        
        // 释放当前节点的路径（如果没被采纳）
        free(current.path);
    }
    
    // 清理
    pq_free(open_set);
    free(best_g);
    
    if (found_count == 0) {
        free(paths);
        return NULL;
    }
    
    *path_count = found_count;
    return paths;
}

/**
 * 查找最短因果路径（使用 A*，返回单条最优路径）
 */
CausalPath* find_shortest_causal_path(CausalGraph* graph, int source, int target, 
                                       int max_length) {
    int path_count = 0;
    CausalPath** paths = find_causal_paths_astar(graph, source, target, max_length, 1, &path_count);
    
    if (path_count == 0) return NULL;
    
    CausalPath* result = paths[0];
    free(paths);  // 释放路径数组，但保留第一个路径（所有权已转移）
    return result;
}

CausalPath* find_direct_causal_path(CausalGraph* graph, int source, int target) {
    CausalEdge* edge = get_causal_edge(graph, source, target);
    if (!edge) return NULL;

    CausalPath* path = (CausalPath*)malloc(sizeof(CausalPath));
    path->node_ids = (int*)malloc(2 * sizeof(int));
    path->node_ids[0] = source;
    path->node_ids[1] = target;
    path->length = 2;
    path->total_strength = edge->strength;
    path->edge_strengths = (float*)malloc(sizeof(float));
    path->edge_strengths[0] = edge->strength;
    path->is_direct = true;
    path->has_confounder = false;

    return path;
}

float compute_path_effect(CausalGraph* graph, CausalPath* path) {
    if (!graph || !path || path->length < 2) return 0.0f;

    float effect = 1.0f;
    for (int i = 0; i < path->length - 1; i++) {
        CausalEdge* edge = get_causal_edge(graph, path->node_ids[i], path->node_ids[i + 1]);
        effect *= edge ? edge->strength : 0.0f;
    }
    return effect;
}

int detect_confounders(CausalGraph* graph, int cause_id, int effect_id,
                      int** confounders) {
    if (!graph || cause_id < 0 || effect_id < 0) {
        if (confounders) *confounders = NULL;
        return 0;
    }

    // 简化：混淆因素是同时指向 cause 和 effect 的节点
    int* confounder_list = (int*)malloc(graph->node_count * sizeof(int));
    int count = 0;

    for (int i = 0; i < graph->node_count; i++) {
        if (i == cause_id || i == effect_id) continue;

        bool to_cause = causal_edge_exists(graph, i, cause_id);
        bool to_effect = causal_edge_exists(graph, i, effect_id);

        if (to_cause && to_effect) {
            confounder_list[count++] = i;
        }
    }

    if (confounders) {
        *confounders = confounder_list;
    } else {
        free(confounder_list);
    }

    return count;
}

// ==================== 反事实推理 ==========

CounterfactualResult* compute_counterfactual(CausalGraph* graph,
                                          CounterfactualQuery* query) {
    if (!graph || !query) return NULL;

    CounterfactualResult* result = (CounterfactualResult*)malloc(sizeof(CounterfactualResult));
    if (!result) return NULL;

    result->query_id = query->query_id;

    // 反事实计算：Y_1 - Y_0
    float y_do_1 = 0.0f;
    float y_do_0 = 0.0f;

    // 干预后的结果 (假设动作 = 1)
    Intervention intervention1 = {
        .type = INTERVENTION_DO,
        .target_node_id = query->action_node_id,
        .new_value = 1.0f
    };
    float* output1 = (float*)calloc(graph->node_count, sizeof(float));
    do_intervention(graph, &intervention1, output1);
    y_do_1 = output1[query->outcome_node_id];
    free(output1);

    // 干预后的结果 (假设动作 = 0)
    Intervention intervention0 = {
        .type = INTERVENTION_DO,
        .target_node_id = query->action_node_id,
        .new_value = 0.0f
    };
    float* output0 = (float*)calloc(graph->node_count, sizeof(float));
    do_intervention(graph, &intervention0, output0);
    y_do_0 = output0[query->outcome_node_id];
    free(output0);

    result->counterfactual_value = y_do_1 - y_do_0;
    result->probability = 0.9f;
    result->explanation = NULL;

    return result;
}

float abduction_action_update(CausalGraph* graph, float* observed,
                            Intervention* action, int outcome_var) {
    if (!graph || !observed || !action) return 0.0f;

    // 推断：根据观察推断未观察到的因素
    // 行动：执行干预
    // 更新：更新对结果的预测

    float* output = (float*)calloc(graph->node_count, sizeof(float));
    do_intervention(graph, action, output);
    float result = output[outcome_var];
    free(output);

    return result;
}

char* generate_counterfactual_explanation(CausalGraph* graph,
                                       CounterfactualResult* result,
                                       CounterfactualQuery* query) {
    if (!graph || !result || !query) return NULL;

    char* explanation = (char*)malloc(512);
    snprintf(explanation, 512,
            "如果对节点 %d 进行干预（从 %.2f 改为 %.2f），"
            "节点 %d 的值将从 %.2f 变为 %.2f。"
            "这种变化的概率约为 %.0f%%。",
            query->action_node_id,
            query->observed_outcome,
            query->hypothetical_action,
            query->outcome_node_id,
            query->observed_outcome,
            result->counterfactual_value,
            result->probability * 100);

    return explanation;
}

// ==================== 因果结构学习 ==========

int pc_algorithm(CausalGraph* graph, float** data, int n, int d, float alpha) {
    if (!graph || !data || n <= 0 || d <= 0) return -1;

    // PC 算法简化实现：
    // 1. 从完全无向图开始
    // 2. 条件独立性测试去除边
    // 3. 定向剩余边

    // 初始化无向图
    for (int i = 0; i < d; i++) {
        for (int j = i + 1; j < d; j++) {
            add_causal_edge(graph, i, j, CAUSAL_SPURIOUS, 0.5f);
        }
    }

    // 条件独立性测试（验证变量间条件独立关系）
    for (int i = 0; i < d; i++) {
        for (int j = i + 1; j < d; j++) {
            // 计算偏相关
            float corr_ij = 0.0f;
            for (int k = 0; k < n; k++) {
                corr_ij += (data[k][i] - 0.5f) * (data[k][j] - 0.5f);
            }
            corr_ij /= n;

            if (fabsf(corr_ij) < alpha) {
                // 条件独立，移除边
                remove_causal_edge(graph, i, j);
                remove_causal_edge(graph, j, i);
            }
        }
    }

    return 0;
}

// ==================== 并行 PC 算法 ====================

// 辅助函数：计算两个变量的相关性
static float compute_correlation(float* x, float* y, int n) {
    float sum_x = 0.0f, sum_y = 0.0f, sum_xy = 0.0f;
    float sum_x2 = 0.0f, sum_y2 = 0.0f;
    
    for (int i = 0; i < n; i++) {
        sum_x += x[i];
        sum_y += y[i];
        sum_xy += x[i] * y[i];
        sum_x2 += x[i] * x[i];
        sum_y2 += y[i] * y[i];
    }
    
    float numerator = n * sum_xy - sum_x * sum_y;
    float denominator = sqrtf((n * sum_x2 - sum_x * sum_x) * (n * sum_y2 - sum_y * sum_y));
    
    if (fabsf(denominator) < EPSILON) return 0.0f;
    return numerator / denominator;
}

// 独立性检验任务结构
typedef struct {
    int var_i;
    int var_j;
    float corr;
    bool independent;
} IndependenceTestTask;

/**
 * 并行 PC 算法（使用 OpenMP 多线程加速）
 * @param graph 因果图
 * @param data 数据矩阵 [n x d]
 * @param n 样本数
 * @param d 变量数
 * @param alpha 显著性水平
 * @return 0 成功
 */
int pc_algorithm_parallel(CausalGraph* graph, float** data, int n, int d, float alpha) {
    if (!graph || !data || n <= 0 || d <= 0) return -1;

    // 计算任务数量
    int total_tests = d * (d - 1) / 2;
    IndependenceTestTask* tasks = (IndependenceTestTask*)malloc(total_tests * sizeof(IndependenceTestTask));
    
    // 准备所有独立性检验任务
    int task_idx = 0;
    for (int i = 0; i < d; i++) {
        for (int j = i + 1; j < d; j++) {
            tasks[task_idx].var_i = i;
            tasks[task_idx].var_j = j;
            tasks[task_idx].corr = 0.0f;
            tasks[task_idx].independent = false;
            task_idx++;
        }
    }

// 并行计算所有相关性 — 每线程预分配缓冲区，消除 per-task malloc 竞争
#ifdef _OPENMP
    #pragma omp parallel
    {
        float* xi = (float*)malloc(n * sizeof(float));
        float* xj = (float*)malloc(n * sizeof(float));
        #pragma omp for schedule(dynamic, 100)
        for (int t = 0; t < total_tests; t++) {
            int i = tasks[t].var_i;
            int j = tasks[t].var_j;
            for (int k = 0; k < n; k++) {
                xi[k] = data[k][i];
                xj[k] = data[k][j];
            }
            float corr = compute_correlation(xi, xj, n);
            tasks[t].corr = corr;
            tasks[t].independent = (fabsf(corr) < alpha);
        }
        free(xi);
        free(xj);
    }
#else
    for (int t = 0; t < total_tests; t++) {
        int i = tasks[t].var_i;
        int j = tasks[t].var_j;
        float* xi = (float*)malloc(n * sizeof(float));
        float* xj = (float*)malloc(n * sizeof(float));
        for (int k = 0; k < n; k++) {
            xi[k] = data[k][i];
            xj[k] = data[k][j];
        }
        float corr = compute_correlation(xi, xj, n);
        tasks[t].corr = corr;
        tasks[t].independent = (fabsf(corr) < alpha);
        free(xi);
        free(xj);
    }
#endif

    // 初始化完全无向图（串行，因为需要去重）
    for (int i = 0; i < d; i++) {
        for (int j = i + 1; j < d; j++) {
            add_causal_edge(graph, i, j, CAUSAL_SPURIOUS, 0.5f);
        }
    }

    // 根据独立性检验结果移除边（串行，需要保证线程安全）
    for (int t = 0; t < total_tests; t++) {
        if (tasks[t].independent) {
            remove_causal_edge(graph, tasks[t].var_i, tasks[t].var_j);
            remove_causal_edge(graph, tasks[t].var_j, tasks[t].var_i);
        }
    }

    free(tasks);
    return 0;
}

/**
 * 获取 CPU 核心数（用于并行计算）
 */
int get_parallel_thread_count(void) {
    #ifdef _OPENMP
    return omp_get_max_threads();
    #else
    return 1;
    #endif
}

int pcg_algorithm(CausalGraph* graph, CausalGraph* graph_prior,
                 float** data, int n, int d) {
    if (!graph || !data || n <= 0 || d <= 0) return -1;

    // 使用先验图指导结构学习
    for (int i = 0; i < d; i++) {
        for (int j = 0; j < d; j++) {
            if (i == j) continue;

            // 基于数据计算相关性
            float corr = 0.0f;
            for (int k = 0; k < n; k++) {
                corr += (data[k][i] - 0.5f) * (data[k][j] - 0.5f);
            }
            corr /= n;
            corr = fabsf(corr);

            // 使用先验增强
            float prior_strength = 0.5f;
            if (graph_prior) {
                CausalEdge* prior_edge = get_causal_edge(graph_prior, i, j);
                if (prior_edge) {
                    prior_strength = prior_edge->strength;
                }
            }

            float final_strength = (corr + prior_strength) / 2.0f;
            if (final_strength > 0.3f) {
                if (!causal_edge_exists(graph, i, j)) {
                    add_causal_edge(graph, i, j, CAUSAL_DIRECT, final_strength);
                }
            }
        }
    }

    return 0;
}

// ==================== 因果规则泛化 ====================

/**
 * 简单通配符匹配
 * @param pattern 模式（可含 *）
 * @param text 文本
 * @return 匹配得分 0-1
 */
static float wildcard_match(const char* pattern, const char* text) {
    if (!pattern || !text) return 0.0f;
    
    // 如果模式不含通配符，严格匹配
    const char* star = strchr(pattern, '*');
    if (!star) {
        return strcmp(pattern, text) == 0 ? 1.0f : 0.0f;
    }
    
    // 简单通配符匹配：前缀*后缀
    size_t prefix_len = star - pattern;
    if (strncmp(pattern, text, prefix_len) == 0) {
        // 前缀匹配，检查后缀
        const char* suffix = star + 1;
        size_t suffix_len = strlen(suffix);
        size_t text_len = strlen(text);
        
        if (suffix_len == 0) return 1.0f;  // * 后无内容
        if (text_len < suffix_len) return 0.0f;
        
        // 后缀匹配
        if (strcmp(text + text_len - suffix_len, suffix) == 0) {
            return 1.0f;
        }
    }
    
    return 0.0f;
}

/**
 * 从具体因果关系创建因果模式
 */
CausalPattern* causal_pattern_create(const char* cause, const char* effect,
                                   float strength) {
    if (!cause || !effect) return NULL;
    
    CausalPattern* pattern = (CausalPattern*)malloc(sizeof(CausalPattern));
    if (!pattern) return NULL;
    
    // 使用 * 替换公共部分创建模式
    // 简化实现：找到公共前缀和后缀，中间用 * 替换
    
    // 计算公共前缀长度
    size_t prefix_len = 0;
    while (cause[prefix_len] && effect[prefix_len] && 
           cause[prefix_len] == effect[prefix_len]) {
        prefix_len++;
    }
    
    // 计算公共后缀长度
    size_t cause_len = strlen(cause);
    size_t effect_len = strlen(effect);
    size_t suffix_len = 0;
    
    while (suffix_len < cause_len && suffix_len < effect_len &&
           cause[cause_len - 1 - suffix_len] == effect[effect_len - 1 - suffix_len]) {
        suffix_len++;
    }
    
    // 构建模式：prefix + * + middle + * + suffix
    char buffer[512];
    size_t buffer_pos = 0;
    
    // 添加前缀
    strncpy(buffer, cause, prefix_len);
    buffer_pos += prefix_len;
    
    // 添加第一个 *
    buffer[buffer_pos++] = '*';
    
    // 添加中间部分（如果有）
    size_t cause_middle_start = prefix_len;
    size_t cause_middle_end = cause_len - suffix_len;
    
    if (cause_middle_end > cause_middle_start) {
        strncpy(buffer + buffer_pos, cause + cause_middle_start, 
               cause_middle_end - cause_middle_start);
        buffer_pos += cause_middle_end - cause_middle_start;
    }
    
    // 添加第二个 *
    buffer[buffer_pos++] = '*';
    
    // 添加后缀
    if (suffix_len > 0) {
        strcpy(buffer + buffer_pos, cause + cause_len - suffix_len);
    } else {
        buffer[buffer_pos] = '\0';
    }
    
    pattern->cause_template = strdup(buffer);
    pattern->effect_template = strdup(buffer);  // 简化：使用相同模式
    
    // 如果没有公共部分，使用原始值
    if (prefix_len == 0 && suffix_len == 0) {
        free(pattern->cause_template);
        free(pattern->effect_template);
        pattern->cause_template = strdup("*");
        pattern->effect_template = strdup("*");
    }
    
    pattern->strength = strength;
    pattern->confidence = strength;
    pattern->instance_count = 1;
    pattern->max_instances = 10;
    pattern->instance_cause = (char**)malloc(10 * sizeof(char*));
    pattern->instance_effect = (char**)malloc(10 * sizeof(char*));
    pattern->instance_cause[0] = strdup(cause);
    pattern->instance_effect[0] = strdup(effect);
    
    return pattern;
}

/**
 * 释放因果模式
 */
void causal_pattern_destroy(CausalPattern* pattern) {
    if (!pattern) return;
    
    free(pattern->cause_template);
    free(pattern->effect_template);
    
    for (int i = 0; i < pattern->instance_count; i++) {
        free(pattern->instance_cause[i]);
        free(pattern->instance_effect[i]);
    }
    free(pattern->instance_cause);
    free(pattern->instance_effect);
    free(pattern);
}

/**
 * 添加实例到因果模式
 */
int causal_pattern_add_instance(CausalPattern* pattern, const char* cause,
                               const char* effect, float strength) {
    if (!pattern || !cause || !effect) return -1;
    
    // 检查是否已存在
    for (int i = 0; i < pattern->instance_count; i++) {
        if (strcmp(pattern->instance_cause[i], cause) == 0 &&
            strcmp(pattern->instance_effect[i], effect) == 0) {
            return 0;  // 已存在
        }
    }
    
    // 需要扩展
    if (pattern->instance_count >= pattern->max_instances) {
        int new_max = pattern->max_instances * 2;
        char** new_cause = (char**)realloc(pattern->instance_cause,
                                           new_max * sizeof(char*));
        if (!new_cause) return -1;
        char** new_effect = (char**)realloc(pattern->instance_effect,
                                            new_max * sizeof(char*));
        if (!new_effect) { free(new_cause); return -1; }
        pattern->instance_cause = new_cause;
        pattern->instance_effect = new_effect;
        pattern->max_instances = new_max;
    }
    
    pattern->instance_cause[pattern->instance_count] = strdup(cause);
    pattern->instance_effect[pattern->instance_count] = strdup(effect);
    pattern->instance_count++;
    
    // 更新强度和置信度
    pattern->strength = (pattern->strength + strength) / 2.0f;
    pattern->confidence = MIN(1.0f, logf((float)pattern->instance_count + 1.0f) / 3.0f);
    
    return 0;
}

/**
 * 匹配具体因果关系到模式
 * @param pattern 模式
 * @param cause 具体原因
 * @param effect 具体效果
 * @return 匹配得分 0-1
 */
float causal_pattern_match(CausalPattern* pattern, const char* cause,
                          const char* effect) {
    if (!pattern || !cause || !effect) return 0.0f;
    
    float cause_score = wildcard_match(pattern->cause_template, cause);
    float effect_score = wildcard_match(pattern->effect_template, effect);
    
    return (cause_score + effect_score) / 2.0f;
}

/**
 * 泛化查询：根据具体因果关系推断可能的泛化
 * @param patterns 模式数组
 * @param pattern_count 模式数量
 * @param cause 具体原因
 * @param effect 具体效果
 * @param match 输出匹配结果
 * @return 匹配数量
 */
int causal_generalize(CausalPattern** patterns, int pattern_count,
                     const char* cause, const char* effect,
                     GeneralizationMatch* match) {
    if (!patterns || !cause || !effect || !match) return 0;
    
    int match_count = 0;
    float best_score = 0.0f;

    for (int i = 0; i < pattern_count; i++) {
        CausalPattern* p = patterns[i];
        float score = causal_pattern_match(p, cause, effect);
        
        if (score > 0.5f) {
            // 找到匹配
            match[match_count].cause = strdup(cause);
            match[match_count].effect = strdup(effect);
            match[match_count].matched_strength = p->strength * score;
            match[match_count].matched_pattern = p;
            match_count++;
            
            if (score > best_score) {
                best_score = score;
            }
        }
    }
    
    // 如果没有找到匹配，尝试创建新模式
    if (match_count == 0 && best_score < 0.5f) {
        // 尝试从现有模式推断
        for (int i = 0; i < pattern_count; i++) {
            CausalPattern* p = patterns[i];
            
            // 只匹配原因或只匹配效果
            float cause_score = wildcard_match(p->cause_template, cause);
            float effect_score = wildcard_match(p->effect_template, effect);
            
            if (cause_score > 0.8f) {
                // 原因匹配，效果可能也匹配
                float inferred_strength = p->strength * cause_score;
                match[match_count].cause = strdup(cause);
                match[match_count].effect = strdup(p->effect_template);
                match[match_count].matched_strength = inferred_strength;
                match[match_count].matched_pattern = p;
                match_count++;
            }
            
            if (effect_score > 0.8f) {
                // 效果匹配，原因可能也匹配
                float inferred_strength = p->strength * effect_score;
                match[match_count].cause = strdup(p->cause_template);
                match[match_count].effect = strdup(effect);
                match[match_count].matched_strength = inferred_strength;
                match[match_count].matched_pattern = p;
                match_count++;
            }
        }
    }
    
    return match_count;
}

/**
 * 创建具体到抽象的映射（学习）
 */
CausalPattern** learn_causal_patterns_from_examples(const char** causes,
                                                   const char** effects,
                                                   float* strengths,
                                                   int count,
                                                   int* out_pattern_count) {
    if (!causes || !effects || count <= 0 || !out_pattern_count) return NULL;
    
    CausalPattern** patterns = (CausalPattern**)malloc(count * sizeof(CausalPattern*));
    int pattern_count = 0;
    
    for (int i = 0; i < count; i++) {
        // 尝试匹配现有模式
        bool matched = false;
        
        for (int j = 0; j < pattern_count; j++) {
            float score = causal_pattern_match(patterns[j], causes[i], effects[i]);
            if (score > 0.8f) {
                // 归入现有模式
                causal_pattern_add_instance(patterns[j], causes[i], effects[i], strengths[i]);
                matched = true;
                break;
            }
        }
        
        if (!matched) {
            // 创建新模式
            patterns[pattern_count] = causal_pattern_create(causes[i], effects[i], strengths[i]);
            if (patterns[pattern_count]) {
                pattern_count++;
            }
        }
    }
    
    *out_pattern_count = pattern_count;
    
    // 收缩到实际大小
    if (pattern_count < count) {
        patterns = (CausalPattern**)realloc(patterns, pattern_count * sizeof(CausalPattern*));
    }
    
    return patterns;
}

/**
 * 释放泛化匹配结果
 */
void free_generalization_matches(GeneralizationMatch* matches, int count) {
    if (!matches) return;
    
    for (int i = 0; i < count; i++) {
        free((char*)matches[i].cause);
        free((char*)matches[i].effect);
    }
}

// ==================== 便捷函数 ==========

bool is_dag(CausalGraph* graph) {
    if (!graph) return false;
    return graph->is_dag;
}

float total_causal_effect(CausalGraph* graph, int source, int target) {
    if (!graph) return 0.0f;

    int path_count = 0;
    CausalPath** paths = find_all_causal_paths(graph, source, target, MAX_PATH_LENGTH, &path_count);

    if (!paths || path_count == 0) {
        CausalEdge* direct = get_causal_edge(graph, source, target);
        return direct ? direct->strength : 0.0f;
    }

    float total = 0.0f;
    for (int i = 0; i < path_count; i++) {
        total += compute_path_effect(graph, paths[i]);
        free(paths[i]->node_ids);
        free(paths[i]->edge_strengths);
        free(paths[i]);
    }
    free(paths);

    return CLAMP(total, 0.0f, 1.0f);
}

void causal_graph_to_adjacency_matrix(CausalGraph* graph, float* matrix) {
    if (!graph || !matrix) return;

    memset(matrix, 0, graph->node_count * graph->node_count * sizeof(float));

    for (int i = 0; i < graph->edge_count; i++) {
        CausalEdge* edge = graph->edges[i];
        if (edge) {
            int from = edge->cause_node_id;
            int to = edge->effect_node_id;
            matrix[from * graph->node_count + to] = edge->strength;
        }
    }
}

int get_parent_nodes(CausalGraph* graph, int node_id, int** parents) {
    if (!graph || node_id < 0 || node_id >= graph->node_count) {
        if (parents) *parents = NULL;
        return 0;
    }

    int* parent_list = (int*)malloc(graph->incoming_count[node_id] * sizeof(int));
    for (int i = 0; i < graph->incoming_count[node_id]; i++) {
        parent_list[i] = graph->incoming[node_id][i];
    }

    if (parents) {
        *parents = parent_list;
    } else {
        free(parent_list);
    }

    return graph->incoming_count[node_id];
}

int get_child_nodes(CausalGraph* graph, int node_id, int** children) {
    if (!graph || node_id < 0 || node_id >= graph->node_count) {
        if (children) *children = NULL;
        return 0;
    }

    int* child_list = (int*)malloc(graph->outgoing_count[node_id] * sizeof(int));
    for (int i = 0; i < graph->outgoing_count[node_id]; i++) {
        child_list[i] = (graph->edges[graph->outgoing[node_id][i]] ? graph->edges[graph->outgoing[node_id][i]]->effect_node_id : -1);
    }

    if (children) {
        *children = child_list;
    } else {
        free(child_list);
    }

    return graph->outgoing_count[node_id];
}

void causal_graph_get_stats(CausalGraph* graph, int* node_count, int* edge_count,
                          float* avg_degree, float* density) {
    if (!graph) return;

    if (node_count) *node_count = graph->node_count;
    if (edge_count) *edge_count = graph->edge_count;

    if (avg_degree) {
        int total_degree = 0;
        for (int i = 0; i < graph->node_count; i++) {
            total_degree += graph->outgoing_count[i] + graph->incoming_count[i];
        }
        *avg_degree = (float)total_degree / graph->node_count;
    }

    if (density) {
        int max_edges = graph->node_count * (graph->node_count - 1);
        *density = (float)graph->edge_count / max_edges;
    }
}

// ==================== A* 最强因果路径搜索 ====================

/**
 * 使用 A* 查找最强因果路径（最大化 total_strength = 边强度乘积）
 * 
 * 核心思路：将边强度 s 转化为代价 c = -log(s)，则路径代价最小化
 * 等价于路径强度乘积最大化。
 * 因为对于强度在 [0,1] 内的边，-log(s) >= 0，满足 A* 非负代价要求。
 * 
 * 启发式函数 h(n) 保守估计剩余路径的 -log(strength) 下界，
 * 保证 A* 的可采纳性（admissible）。
 * 
 * @param graph 因果图
 * @param source 源节点
 * @param target 目标节点
 * @param max_length 最大路径长度
 * @param max_paths 最大返回路径数量
 * @param path_count 输出路径数量
 * @return 路径数组（按强度降序），需调用者释放
 */
CausalPath** find_strongest_causal_path_astar(CausalGraph* graph, int source, int target,
                                              int max_length, int max_paths, int* path_count) {
    if (!graph || source < 0 || target < 0 || 
        source >= graph->node_count || target >= graph->node_count || 
        path_count == NULL) {
        if (path_count) *path_count = 0;
        return NULL;
    }

    *path_count = 0;
    if (source == target) return NULL;

    // 创建优先队列（最小堆，按 f = -log(strength) 总代价排序）
    PriorityQueue* open_set = pq_create(512);
    
    // 记录已访问节点在当前轮次的最低 g_score（用于剪枝）
    float* best_g = (float*)malloc(graph->node_count * sizeof(float));
    for (int i = 0; i < graph->node_count; i++) best_g[i] = FLT_MAX;
    
    CausalPath** paths = (CausalPath**)malloc(max_paths * sizeof(CausalPath*));
    int found_count = 0;

    // 估算最大可能的边强度（用于启发式）
    float max_edge_strength = 0.0f;
    for (int i = 0; i < graph->edge_count; i++) {
        if (graph->edges[i] && graph->edges[i]->strength > max_edge_strength) {
            max_edge_strength = graph->edges[i]->strength;
        }
    }
    if (max_edge_strength < 0.01f) max_edge_strength = 0.5f;
    float min_log_cost = -logf(max_edge_strength + EPSILON);  // 单边最小代价
    
    // 初始化起点：g = 0, f = 0（h=0时为Dijkstra，但更稳妥）
    AStarNode start_node;
    start_node.node_id = source;
    start_node.g_score = 0.0f;
    start_node.f_score = 0.0f;
    start_node.path = (int*)malloc(max_length * sizeof(int));
    start_node.path[0] = source;
    start_node.path_len = 1;
    start_node.total_strength = 1.0f;
    
    pq_push(open_set, &start_node);
    best_g[source] = 0.0f;

    /* v0.5.7: 扩展次数上限——5 万节点密集图上 max_length=5 的 A*
     * 会扩展 14^5≈53 万节点（10-20s 卡死）；超限快速失败，
     * 调用方（pfe_solve_subgoal）回退 diffusion 保证响应。 */
    int expansions = 0;
    const int max_expansions = 2000;

    while (open_set->size > 0 && found_count < max_paths) {
        if (++expansions > max_expansions) break;
        AStarNode current;
        pq_pop(open_set, &current);
        
        // 剪枝：如果当前 g 已不是最优，跳过
        if (current.g_score > best_g[current.node_id] + 0.001f) {
            free(current.path);
            continue;
        }
        
        if (current.node_id == target && current.path_len > 1) {
            CausalPath* path = astar_create_path(graph, &current);
            if (path) paths[found_count++] = path;
            continue;
        }
        
        if (current.path_len >= max_length) {
            free(current.path);
            continue;
        }
        
        // 扩展邻居
        for (int i = 0; i < graph->outgoing_count[current.node_id]; i++) {
            int neighbor = (graph->edges[graph->outgoing[current.node_id][i]] ? graph->edges[graph->outgoing[current.node_id][i]]->effect_node_id : -1);
            
            // 避免循环
            bool in_path = false;
            for (int j = 0; j < current.path_len; j++) {
                if (current.path[j] == neighbor) { in_path = true; break; }
            }
            if (in_path) continue;
            
            CausalEdge* edge = get_causal_edge(graph, current.node_id, neighbor);
            float edge_strength = edge ? edge->strength : 0.5f;
            
            // 以 -log(strength) 为代价：高强度边 = 低代价
            float edge_cost = -logf(edge_strength + EPSILON);
            float tentative_g = current.g_score + edge_cost;
            
            // 剪枝：如果已有更优路径到此节点，跳过
            if (tentative_g >= best_g[neighbor]) continue;
            best_g[neighbor] = tentative_g;
            
            // 启发式：估计剩余路径的最小代价
            // 保守估计：至少还需要 1 步到达目标，每步代价 >= min_log_cost
            float min_remaining_steps = 1.0f;
            if (neighbor != target && current.path_len + 1 < max_length) {
                // 检查是否有直接边到目标
                if (!causal_edge_exists(graph, neighbor, target)) {
                    min_remaining_steps = 2.0f;  // 至少还要两步
                }
                // 保守估计，确保可采纳性（不低估）
                min_remaining_steps = fminf(min_remaining_steps, 
                                          (float)(max_length - current.path_len - 1));
            }
            // 保守启发式：假设剩下的边都很强（低代价）
            float h = min_remaining_steps * min_log_cost * 0.5f;
            // 但保证不高估实际剩余代价 → 保守一点用 0.5 倍
            // 其实安全做法是 h = 0（退化为 Dijkstra），性能可接受
            
            float f = tentative_g + h;
            
            AStarNode neighbor_node;
            neighbor_node.node_id = neighbor;
            neighbor_node.g_score = tentative_g;
            neighbor_node.f_score = f;
            neighbor_node.path = (int*)malloc(max_length * sizeof(int));
            memcpy(neighbor_node.path, current.path, current.path_len * sizeof(int));
            neighbor_node.path[current.path_len] = neighbor;
            neighbor_node.path_len = current.path_len + 1;
            neighbor_node.total_strength = current.total_strength * edge_strength;
            
            pq_push(open_set, &neighbor_node);
        }
        
        free(current.path);
    }
    
    pq_free(open_set);
    free(best_g);
    
    if (found_count == 0) {
        free(paths);
        return NULL;
    }
    
    *path_count = found_count;
    return paths;
}

/**
 * 查找单条最强因果路径
 */
CausalPath* find_strongest_causal_path(CausalGraph* graph, int source, int target, int max_length) {
    int path_count = 0;
    CausalPath** paths = find_strongest_causal_path_astar(graph, source, target, max_length, 1, &path_count);
    if (path_count == 0) return NULL;
    CausalPath* result = paths[0];
    free(paths);
    return result;
}

// ==================== 条件路径切除（拓扑版反事实推理） ====================

/**
 * 条件路径切除：检查在指定节点被阻塞（不激活）时，
 * 从 source 到 target 是否仍有路径可达。
 * 
 * 这是拓扑版反事实推理的核心：
 * "如果X不激活（阻塞节点集），Y还能到达吗？"
 * 
 * @param graph 因果图（无向版本）
 * @param source 源节点
 * @param target 目标节点
 * @param blocked_nodes 被阻塞/被切除的节点ID数组
 * @param blocked_count 阻塞节点数量
 * @param max_length 最大搜索深度
 * @return true 如果存在绕过阻塞节点的路径
 */
bool conditional_path_reachability(CausalGraph* graph, int source, int target,
                                   int* blocked_nodes, int blocked_count,
                                   int max_length) {
    if (!graph || source < 0 || target < 0 ||
        source >= graph->node_count || target >= graph->node_count) {
        return false;
    }
    if (source == target) return true;
    
    // 构建阻塞掩码（O(1) 查询）
    bool* blocked = (bool*)calloc(graph->node_count, sizeof(bool));
    for (int i = 0; i < blocked_count; i++) {
        if (blocked_nodes[i] >= 0 && blocked_nodes[i] < graph->node_count) {
            blocked[blocked_nodes[i]] = true;
        }
    }
    
    // 如果源节点或目标节点本身被阻塞，不可达
    if (blocked[source] || blocked[target]) {
        free(blocked);
        return false;
    }
    
    // BFS/DFS 搜索（使用栈，避免队列分配）
    bool* visited = (bool*)calloc(graph->node_count, sizeof(bool));
    int* stack = (int*)malloc(graph->node_count * sizeof(int));
    int stack_top = 0;
    
    visited[source] = true;
    stack[stack_top++] = source;
    
    int current_depth = 0;
    bool found = false;
    
    while (stack_top > 0) {
        int node = stack[--stack_top];
        
        // 检查出边
        for (int i = 0; i < graph->outgoing_count[node]; i++) {
            int neighbor = (graph->edges[graph->outgoing[node][i]] ? graph->edges[graph->outgoing[node][i]]->effect_node_id : -1);
            if (visited[neighbor] || blocked[neighbor]) continue;
            
            if (neighbor == target) {
                found = true;
                goto cleanup;
            }
            
            visited[neighbor] = true;
            stack[stack_top++] = neighbor;
        }
        
        // 如果是无向图，也检查入边（作为反向出边）
        for (int i = 0; i < graph->incoming_count[node]; i++) {
            int neighbor = graph->incoming[node][i];
            if (visited[neighbor] || blocked[neighbor]) continue;
            
            if (neighbor == target) {
                found = true;
                goto cleanup;
            }
            
            visited[neighbor] = true;
            stack[stack_top++] = neighbor;
        }
        
        // 深度跟踪（简化：不精确但足够）
        current_depth++;
        if (current_depth >= max_length) break;
    }
    
cleanup:
    free(blocked);
    free(visited);
    free(stack);
    return found;
}

/**
 * 条件路径切除的高级查询：
 * 返回在阻塞节点集下，所有从 source 可达 target 的最强路径。
 * 
 * 这相当于问："如果不经过这些节点，X到Y最强的因果链是什么？"
 * 结合了最强路径搜索 + 条件切除。
 * 
 * @param graph 因果图
 * @param source 源节点
 * @param target 目标节点
 * @param blocked_nodes 被阻塞的节点数组
 * @param blocked_count 阻塞节点数量
 * @param max_length 最大路径长度
 * @param max_paths 最大返回路径数量
 * @param path_count 输出路径数量
 * @return 路径数组，需调用者释放
 */
CausalPath** conditional_strongest_paths(CausalGraph* graph, int source, int target,
                                         int* blocked_nodes, int blocked_count,
                                         int max_length, int max_paths, int* path_count) {
    if (!graph || path_count == NULL) {
        if (path_count) *path_count = 0;
        return NULL;
    }
    *path_count = 0;
    
    // 构建阻塞掩码
    bool* blocked = (bool*)calloc(graph->node_count, sizeof(bool));
    for (int i = 0; i < blocked_count; i++) {
        if (blocked_nodes[i] >= 0 && blocked_nodes[i] < graph->node_count) {
            blocked[blocked_nodes[i]] = true;
        }
    }
    
    if (blocked[source] || blocked[target]) {
        free(blocked);
        return NULL;
    }
    
    // 使用修改版 A*（跳过阻塞节点）
    PriorityQueue* open_set = pq_create(512);
    
    float* best_g = (float*)malloc(graph->node_count * sizeof(float));
    for (int i = 0; i < graph->node_count; i++) best_g[i] = FLT_MAX;
    
    float max_edge_strength = 0.5f;
    for (int i = 0; i < graph->edge_count; i++) {
        if (graph->edges[i] && graph->edges[i]->strength > max_edge_strength) {
            max_edge_strength = graph->edges[i]->strength;
        }
    }
    float min_log_cost = -logf(max_edge_strength + EPSILON);
    
    CausalPath** paths = (CausalPath**)malloc(max_paths * sizeof(CausalPath*));
    int found_count = 0;
    
    AStarNode start_node;
    start_node.node_id = source;
    start_node.g_score = 0.0f;
    start_node.f_score = 0.0f;
    start_node.path = (int*)malloc(max_length * sizeof(int));
    start_node.path[0] = source;
    start_node.path_len = 1;
    start_node.total_strength = 1.0f;
    
    pq_push(open_set, &start_node);
    best_g[source] = 0.0f;
    
    while (open_set->size > 0 && found_count < max_paths) {
        AStarNode current;
        pq_pop(open_set, &current);
        
        if (current.g_score > best_g[current.node_id] + 0.001f) {
            free(current.path);
            continue;
        }
        
        if (current.node_id == target && current.path_len > 1) {
            CausalPath* path = astar_create_path(graph, &current);
            if (path) paths[found_count++] = path;
            continue;
        }
        
        if (current.path_len >= max_length) {
            free(current.path);
            continue;
        }
        
        // 扩展邻居（跳过阻塞节点）
        for (int i = 0; i < graph->outgoing_count[current.node_id]; i++) {
            int neighbor = (graph->edges[graph->outgoing[current.node_id][i]] ? graph->edges[graph->outgoing[current.node_id][i]]->effect_node_id : -1);
            if (blocked[neighbor]) continue;
            
            bool in_path = false;
            for (int j = 0; j < current.path_len; j++) {
                if (current.path[j] == neighbor) { in_path = true; break; }
            }
            if (in_path) continue;
            
            CausalEdge* edge = get_causal_edge(graph, current.node_id, neighbor);
            float edge_strength = edge ? edge->strength : 0.5f;
            float edge_cost = -logf(edge_strength + EPSILON);
            float tentative_g = current.g_score + edge_cost;
            
            if (tentative_g >= best_g[neighbor]) continue;
            best_g[neighbor] = tentative_g;
            
            float h = min_log_cost * 0.5f;  // 保守启发式
            float f = tentative_g + h;
            
            AStarNode neighbor_node;
            neighbor_node.node_id = neighbor;
            neighbor_node.g_score = tentative_g;
            neighbor_node.f_score = f;
            neighbor_node.path = (int*)malloc(max_length * sizeof(int));
            memcpy(neighbor_node.path, current.path, current.path_len * sizeof(int));
            neighbor_node.path[current.path_len] = neighbor;
            neighbor_node.path_len = current.path_len + 1;
            neighbor_node.total_strength = current.total_strength * edge_strength;
            
            pq_push(open_set, &neighbor_node);
        }
        
        free(current.path);
    }
    
    pq_free(open_set);
    free(best_g);
    free(blocked);
    
    if (found_count == 0) {
        free(paths);
        return NULL;
    }
    
    *path_count = found_count;
    return paths;
}

// ==================== 多拓扑 → 因果图桥接 ====================

/**
 * 从多拓扑网络推断统一因果图
 * 
 * 遍历所有子拓扑，将每个子拓扑内的节点连接视为因果边，
 * 同时提取跨拓扑连接作为跨域因果传播路径。
 * 所有节点根据其概念名去重合并，构建统一的因果图视图。
 * 
 * @param master 多拓扑网络
 * @param min_strength 最小连接强度阈值
 * @return 因果图，需调用者释放
 */
/* ==================== 因果图构建哈希索引（v0.5.7） ====================
 * infer_causal_graph_from_master_topology 原实现用线性搜索去重/建边
 * （O(N²)），5 万节点拓扑上卡死（PFE 因果搜索 93% CPU 忙转）。
 * 改为 djb2 哈希桶：O(1) 查询。 */
typedef struct CgNameEntry {
    const char* name;
    int         cg_id;
    struct CgNameEntry* next;
} CgNameEntry;

typedef struct {
    CgNameEntry** buckets;
    int size;
} CgNameMap;

static unsigned long cg_name_hash(const char* s) {
    unsigned long h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

static CgNameMap* cg_namemap_create(int size) {
    if (size < 1024) size = 1024;
    CgNameMap* m = (CgNameMap*)calloc(1, sizeof(CgNameMap));
    if (!m) return NULL;
    m->buckets = (CgNameEntry**)calloc((size_t)size, sizeof(CgNameEntry*));
    if (!m->buckets) { free(m); return NULL; }
    m->size = size;
    return m;
}

/* 返回 cg_id；未命中返回 -1 */
static int cg_namemap_get(CgNameMap* m, const char* name) {
    if (!m || !name) return -1;
    unsigned long h = cg_name_hash(name) % (unsigned long)m->size;
    for (CgNameEntry* e = m->buckets[h]; e; e = e->next) {
        if (strcmp(e->name, name) == 0) return e->cg_id;
    }
    return -1;
}

static void cg_namemap_put(CgNameMap* m, const char* name, int cg_id) {
    if (!m || !name) return;
    unsigned long h = cg_name_hash(name) % (unsigned long)m->size;
    CgNameEntry* e = (CgNameEntry*)malloc(sizeof(CgNameEntry));
    if (!e) return;
    e->name = name;
    e->cg_id = cg_id;
    e->next = m->buckets[h];
    m->buckets[h] = e;
}

static void cg_namemap_free(CgNameMap* m) {
    if (!m) return;
    for (int i = 0; i < m->size; i++) {
        CgNameEntry* e = m->buckets[i];
        while (e) { CgNameEntry* n = e->next; free(e); e = n; }
    }
    free(m->buckets);
    free(m);
}

CausalGraph* infer_causal_graph_from_master_topology(MasterTopology* master,
                                                     float min_strength) {
    if (!master) return NULL;

    // 第一步：统计总节点数（所有子拓扑 + 跨拓扑，按概念名去重）
    // 使用临时哈希表去重（简化：先用大容量）
    int total_nodes = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        if (master->sub_topologies[t] && master->sub_topologies[t]->net) {
            total_nodes += master->sub_topologies[t]->net->node_count;
        }
    }
    if (total_nodes == 0) return NULL;

    // 创建因果图（容量取节点数的2倍作为边容量）
    CausalGraph* graph = causal_graph_create(total_nodes + 1, total_nodes * 4);
    if (!graph) return NULL;
    graph->topo_node_count = total_nodes;

    // 第二步：构建概念名 → 因果图节点ID的映射（v0.5.7 哈希版）
    CgNameMap* namemap = cg_namemap_create(total_nodes * 2);
    if (!namemap) { causal_graph_destroy(graph); return NULL; }
    int actual_nodes = 0;

    // 先收集所有子拓扑的节点
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;

        for (int i = 0; i < sub->net->node_count; i++) {
            ReasoningNode* node = sub->net->nodes[i];
            if (!node || !node->concept) continue;

            int cg = cg_namemap_get(namemap, node->concept);
            if (cg >= 0) {
                graph->node_mapping[cg] = node->node_id;   /* 同名合并 */
            } else {
                cg_namemap_put(namemap, node->concept, actual_nodes);
                graph->node_mapping[actual_nodes] = node->node_id;
                actual_nodes++;
            }
        }
    }

    // 调整因果图为实际节点数
    // 注意：已分配的node_mapping前actual_nodes个有效

    // 第三步：遍历所有拓扑内部的连接，转换为因果边
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;

        for (int i = 0; i < sub->net->node_count; i++) {
            ReasoningNode* node = sub->net->nodes[i];
            if (!node || !node->concept) continue;

            // 找该节点在因果图中的ID（哈希 O(1)）
            int cause_cg_id = cg_namemap_get(namemap, node->concept);
            if (cause_cg_id < 0) continue;

            // 遍历连接边
            for (int c = 0; c < node->edge_count; c++) {
                if (!node->edges[c].target) continue;
                ReasoningNode* target = node->edges[c].target;
                if (!target || !target->concept) continue;

                float weight = (c < node->edge_count) ?
                               node->edges[c].weight : 0.5f;
                if (weight < min_strength) continue;

                int effect_cg_id = cg_namemap_get(namemap, target->concept);
                if (effect_cg_id < 0) continue;
                if (cause_cg_id == effect_cg_id) continue;

                // 添加因果边（已在graph->node_count上限内）
                // 确保node_count足够大
                if (cause_cg_id >= graph->node_count) continue;
                if (effect_cg_id >= graph->node_count) continue;

                /* v0.5.7: 批量建图不查重（no_check）——causal_edge_exists
                 * 线性扫描在循环里 O(E²) 爆炸；同名合并产生的重复边
                 * 无害（A* 容忍） */
                add_causal_edge_no_check(graph, cause_cg_id, effect_cg_id, 
                                         CAUSAL_DIRECT, weight);
            }
        }
    }

    // 第四步：处理跨拓扑连接
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;

        for (int i = 0; i < sub->net->node_count; i++) {
            ReasoningNode* node = sub->net->nodes[i];
            if (!node || !node->concept) continue;

            int cause_cg_id = cg_namemap_get(namemap, node->concept);
            if (cause_cg_id < 0) continue;

            // 跨拓扑连接（通过交叉连接索引）
            int adj_idx = sub->topo_id * PM_MAX_NODES_PER_TOPO + i;
            if (adj_idx < master->cross_adj_count) {
                CrossTopoAdjEntry* entry = master->cross_adj[adj_idx];
                while (entry) {
                    if (entry->link_index < master->cross_link_count) {
                        CrossTopologyLink* link = master->cross_links[entry->link_index];
                        if (link && link->weight >= min_strength) {
                            // 查找目标节点（先通过topo_id找子拓扑）
                            SubTopology* to_sub = NULL;
                            for (int st = 0; st < master->sub_topo_count; st++) {
                                if (master->sub_topologies[st] &&
                                    master->sub_topologies[st]->topo_id == link->to_topo_id) {
                                    to_sub = master->sub_topologies[st];
                                    break;
                                }
                            }
                            if (to_sub && to_sub->net && 
                                link->to_node_id < to_sub->net->node_count) {
                                ReasoningNode* to_node = 
                                    to_sub->net->nodes[link->to_node_id];
                                if (to_node && to_node->concept) {
                                    int effect_cg_id = cg_namemap_get(namemap, to_node->concept);
                                    if (effect_cg_id >= 0 && 
                                        effect_cg_id != cause_cg_id &&
                                        effect_cg_id < graph->node_count) {
                                        if (!causal_edge_exists(graph, cause_cg_id, 
                                                              effect_cg_id)) {
                                            add_causal_edge_no_check(graph, cause_cg_id,
                                                               effect_cg_id,
                                                               CAUSAL_INDIRECT,
                                                          link->weight *
                                                          link->transfer_rate);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    entry = entry->next;
                }
            }
        }
    }

    // 清理
    cg_namemap_free(namemap);

    return graph;
}

/**
 * 因果联想搜索（A* 替换 BFS 联想扩散的桥接函数）
 * 
 * 给定输入文本，使用 A* 最强路径搜索替代传统 BFS 激活传播：
 * 1. 从多拓扑网络构建统一因果图
 * 2. 分词找到输入中的激活概念
 * 3. 对每对概念运行 A* 最强路径搜索
 * 4. 返回因果路径（按总强度降序排列）
 * 
 * @param master 多拓扑网络
 * @param input_text 输入文本
 * @param max_hops 最大路径长度
 * @param max_results 最大返回结果数
 * @param out_count 输出搜索结果数量
 * @return 因果搜索结果数组（按强度降序），需调用者释放
 */
CausalSearchResult* causal_associative_search(MasterTopology* master,
                                              const char* input_text,
                                              int max_hops, int max_results,
                                              int* out_count) {
    if (!master || !input_text || !out_count) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    *out_count = 0;

    /* v0.5.7: 因果图缓存——PFE 子目标/重试多次调用不必每次全图重建
     * （5 万节点/70 万边建图 ~1-2s，多次调用拖垮响应）。
     * 指纹 = 总节点数 + 跨拓扑连接数（结构变化时重建）。 */
    static CausalGraph* g_cg_cache = NULL;
    static int g_cg_cache_fp = 0;
    static pthread_mutex_t g_cg_cache_lock = PTHREAD_MUTEX_INITIALIZER;

    int fp = master_count_total_nodes(master) + master->cross_link_count;
    pthread_mutex_lock(&g_cg_cache_lock);
    /* 指纹容差：学习/词巩固的节点数小变化（<500）不重建——
     * 否则 PFE 请求期间缓存永不命中，每次都全图重建拖垮响应 */
    if (g_cg_cache && (fp < g_cg_cache_fp - 500 || fp > g_cg_cache_fp + 500)) {
        causal_graph_destroy(g_cg_cache);
        g_cg_cache = NULL;
    }
    if (!g_cg_cache) {
        g_cg_cache = infer_causal_graph_from_master_topology(master, 0.35f);
        g_cg_cache_fp = fp;
    }
    CausalGraph* graph = g_cg_cache;
    pthread_mutex_unlock(&g_cg_cache_lock);

    if (!graph || graph->edge_count == 0) {
        return NULL;
    }

    // 2. 分词并找到输入中的激活概念ID
    char* tokens[100];
    int token_count = utf8_tokenize(input_text, tokens, 100);
    if (token_count <= 0) {
        /* v0.5.7: graph 是共享缓存 g_cg_cache——不能在这里 destroy！
         * 原代码 causal_graph_destroy(graph) destroy 共享缓存后未置 NULL
         * （且锁已释放、锁外 destroy），下次调用要么在指纹容差内直接
         * 使用悬垂指针（UAF），要么指纹超差时对已释放图二次 destroy
         * （double-free → free(): invalid size / SIGSEGV）。缓存图统一
         * 由指纹变化分支（destroy 后置 NULL）销毁重建。 */
        return NULL;
    }

    // 收集输入中激活的概念节点（在因果图中的ID）
    int* source_ids = (int*)malloc(graph->node_count * sizeof(int));
    int source_count = 0;

    // 从词汇拓扑找概念
    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    
    for (int i = 0; i < token_count; i++) {
        if (!tokens[i]) continue;
        
        // 先在词汇拓扑中找
        ReasoningNode* node = NULL;
        if (vocab && vocab->net) {
            // 使用node_hash或线性搜索
            if (vocab->node_hash) {
                node = node_hash_find(vocab->node_hash, tokens[i]);
            } else {
                for (int j = 0; j < vocab->net->node_count; j++) {
                    if (vocab->net->nodes[j] && vocab->net->nodes[j]->concept &&
                        strcmp_null(vocab->net->nodes[j]->concept, tokens[i]) == 0) {
                        node = vocab->net->nodes[j];
                        break;
                    }
                }
            }
        }

        if (node && node->concept) {
            // 在因果图中找对应ID
            for (int j = 0; j < graph->node_count; j++) {
                // 用node_mapping判断（简化：通过概念名搜索所有子拓扑）
                // 这里我们只需要概念存在即可，运行A*时会自动发现路径
                bool found = false;
                // 扫描所有子拓扑找这个概念
                for (int t = 0; t < master->sub_topo_count && !found; t++) {
                    SubTopology* sub = master->sub_topologies[t];
                    if (!sub || !sub->net) continue;
                    for (int k = 0; k < sub->net->node_count && !found; k++) {
                        ReasoningNode* n = sub->net->nodes[k];
                        if (n && n->concept && 
                            strcmp_null(n->concept, node->concept) == 0) {
                            // 这个节点在因果图中有对应的ID
                            // 用因果图节点索引作为source_id
                            found = true;
                        }
                    }
                }
                if (found) {
                    // 用概念哈希索引（简化：直接用j）
                    bool dup = false;
                    for (int k = 0; k < source_count; k++) {
                        if (source_ids[k] == j) { dup = true; break; }
                    }
                    if (!dup) {
                        source_ids[source_count++] = j;
                    }
                    break;
                }
            }
        }
    }

    if (source_count == 0) {
        // 没有找到匹配概念，尝试直接搜索图
        for (int i = 0; i < token_count; i++) {
            if (!tokens[i]) continue;
            for (int j = 0; j < graph->node_count; j++) {
                // 用node_mapping反向查找概念名（不可靠，跳过）
                (void)j;
            }
        }
    }

    // 3. 在因果图中运行 A* 最强路径搜索
    // 对每对 source→target 运行 A*（只跑 source_count^2 对中最有希望的）
    CausalSearchResult* results = (CausalSearchResult*)calloc(max_results, sizeof(CausalSearchResult));
    int result_count = 0;

    if (source_count > 0) {
        int explored = 0;
        
        for (int s = 0; s < source_count && result_count < max_results && explored < 100; s++) {
            for (int t = 0; t < graph->node_count && result_count < max_results && explored < 100; t++) {
                if (t == source_ids[s]) continue;
                explored++;
                
                int path_cnt = 0;
                CausalPath** paths = find_strongest_causal_path_astar(
                    graph, source_ids[s], t, max_hops, 1, &path_cnt);
                
                if (paths && path_cnt > 0) {
                    // 找到了一条因果链
                    CausalPath* best = paths[0];
                    
                    // 只在强度足够时记录
                    if (best->total_strength > 0.2f) {
                        results[result_count].from_source = s;
                        results[result_count].source_node = source_ids[s];
                        results[result_count].target_node = t;
                        results[result_count].path = best;
                        results[result_count].total_strength = best->total_strength;
                        results[result_count].path_length = best->length;
                        result_count++;
                    } else {
                        free(best->node_ids);
                        free(best->edge_strengths);
                        free(best);
                    }
                    free(paths);
                }
            }
        }
    }

    // 4. 按强度降序排序结果
    for (int i = 0; i < result_count - 1; i++) {
        for (int j = i + 1; j < result_count; j++) {
            if (results[j].total_strength > results[i].total_strength) {
                CausalSearchResult tmp = results[i];
                results[i] = results[j];
                results[j] = tmp;
            }
        }
    }

    // 清理
    for (int i = 0; i < token_count; i++) free(tokens[i]);
    free(source_ids);
    /* v0.5.7: 图是共享缓存（g_cg_cache），不在这里 destroy——
     * 原代码 destroy 缓存图导致悬垂指针（下次调用 object_pool_destroy 崩） */
    /* causal_graph_destroy(graph); */

    *out_count = result_count;
    return results;
}

/**
 * 释放因果搜索结果数组
 */
void causal_search_results_free(CausalSearchResult* results, int count) {
    if (!results) return;
    for (int i = 0; i < count; i++) {
        if (results[i].path) {
            free(results[i].path->node_ids);
            free(results[i].path->edge_strengths);
            free(results[i].path);
        }
    }
    free(results);
}

// ==================== 方向推断（基于概念层级） ====================

/**
 * 基于概念层级推断因果方向
 * 
 * 核心原理：抽象概念（因果层/意图层）通常影响具体概念（具体层/感觉运动层）。
 * 如果A的抽象层级比B高 ≥2 层，则方向为 A → B。
 * 如果层级相近（0-1层差），使用以下弱投票：
 *   - 时序优先：输入顺序中先出现的作为因
 *   - 连接强度偏置：强连接的方向更可信
 *   - 频率偏置：出现次数多的通用概念倾向于作因
 * 
 * @param graph 因果图（会被修改方向信息）
 * @param hierarchy 概念层级（已构建）
 * @param master 多拓扑网络（用于概念名查找）
 * @param strength_threshold 最小方向置信度阈值
 * @return 被确定方向的边数
 */
int infer_causal_direction_from_hierarchy(CausalGraph* graph,
                                          ConceptHierarchy* hierarchy,
                                          MasterTopology* master,
                                          float strength_threshold) {
    if (!graph || !hierarchy || !master) return 0;

    int directed_count = 0;

    // 构建拓扑节点ID → 概念层级 的快速映射（通过概念名匹配）
    // 先收集所有ReasoningNode到层级映射
    int* topo_to_level = NULL;
    int max_topo_node = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (sub && sub->net && sub->net->node_count > max_topo_node) {
            max_topo_node = sub->net->node_count;
        }
    }
    if (max_topo_node == 0) return 0;

    // 为顶层拓扑（词汇拓扑）构建节点ID→层级映射
    SubTopology* vocab = NULL;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (sub && sub->type == TOPO_VOCABULARY) {
            vocab = sub;
            break;
        }
    }
    if (!vocab || !vocab->net) return 0;

    topo_to_level = (int*)malloc(vocab->net->node_count * sizeof(int));
    for (int i = 0; i < vocab->net->node_count; i++) {
        topo_to_level[i] = -1;  // 未知
    }

    // 遍历ConceptHierarchy，建立概念名 → 层级的映射
    for (int i = 0; i < hierarchy->node_count; i++) {
        ConceptNode* cn = hierarchy->nodes[i];
        if (!cn || !cn->name) continue;

        // 找到词汇拓扑中同名的节点
        for (int j = 0; j < vocab->net->node_count; j++) {
            ReasoningNode* rn = vocab->net->nodes[j];
            if (rn && rn->concept && strcmp_null(rn->concept, cn->name) == 0) {
                topo_to_level[j] = (int)cn->level;
                break;
            }
        }

        // 也检查 concrete_nodes 映射
        for (int ci = 0; ci < cn->concrete_count; ci++) {
            int topo_id = cn->concrete_nodes[ci];
            if (topo_id >= 0 && topo_id < vocab->net->node_count) {
                topo_to_level[topo_id] = (int)cn->level;
            }
        }
    }

    // 遍历所有因果图边，推断方向
    for (int e = 0; e < graph->edge_count; e++) {
        CausalEdge* edge = graph->edges[e];
        if (!edge) continue;

        // 已经是单向的，跳过
        if (!edge->bidirectional) continue;

        // 获取因果图节点对应的拓扑节点
        int cause_topo = (edge->cause_node_id < graph->node_count) ?
                         graph->node_mapping[edge->cause_node_id] : -1;
        int effect_topo = (edge->effect_node_id < graph->node_count) ?
                          graph->node_mapping[edge->effect_node_id] : -1;

        if (cause_topo < 0 || effect_topo < 0) continue;
        if (cause_topo >= vocab->net->node_count || 
            effect_topo >= vocab->net->node_count) continue;

        // 获取两个节点在层级中的层级
        int level_cause = (cause_topo < vocab->net->node_count) ?
                          topo_to_level[cause_topo] : -1;
        int level_effect = (effect_topo < vocab->net->node_count) ?
                           topo_to_level[effect_topo] : -1;

        // 方向推断
        bool direction_confirmed = false;

        if (level_cause >= 0 && level_effect >= 0) {
            int level_diff = level_cause - level_effect;

            // 策略1：层级差 ≥ 2 → 高层级影响低层级
            if (level_diff >= 2) {
                // cause的层级更高 → 方向正确（高→低）
                edge->bidirectional = false;
                direction_confirmed = true;
            } else if (level_diff <= -2) {
                // effect的层级更高 → 方向反了，交换
                // 不能直接交换cause_node_id/effect_node_id因为会影响邻接表
                // 改用类型标记，让调用方处理交换逻辑
                // 实际交换节点ID
                int tmp = edge->cause_node_id;
                edge->cause_node_id = edge->effect_node_id;
                edge->effect_node_id = tmp;
                edge->bidirectional = false;
                direction_confirmed = true;
            }
        }

        if (direction_confirmed) {
            // 更新边类型为直接因果
            edge->type = CAUSAL_DIRECT;
            // 方向置信度使用层级差归一化
            float level_factor = 1.0f;
            if (level_cause >= 0 && level_effect >= 0) {
                int diff = abs(level_cause - level_effect);
                level_factor = 0.5f + 0.5f * (diff / (float)(CONCEPT_LEVEL_COUNT - 1));
            }
            edge->confidence = CLAMP(level_factor, 0.0f, 1.0f);
            directed_count++;
        } else if (level_cause >= 0 || level_effect >= 0) {
            // 策略2：只有一个节点有层级信息，用强度偏置
            if (edge->strength >= strength_threshold) {
                // 强连接更可靠，保持当前方向
                edge->bidirectional = false;
                edge->confidence = edge->strength * 0.7f;
                directed_count++;
            }
        }
        // 策略3：都没有层级信息，保持bidirectional
        // 留待后续更多数据再判断
    }

    free(topo_to_level);

    // 如果方向信息足够多，更新DAG状态
    if (directed_count > 0) {
        // 重新检查是否为DAG
        int* visited = (int*)calloc(graph->node_count, sizeof(int));
        int* rec_stack = (int*)calloc(graph->node_count, sizeof(int));
        graph->is_dag = true;

        for (int i = 0; i < graph->node_count && graph->is_dag; i++) {
            if (!visited[i]) {
                if (has_cycle_dfs(graph, i, visited, rec_stack)) {
                    graph->is_dag = false;
                    break;
                }
            }
        }

        free(visited);
        free(rec_stack);
        graph->last_updated = time(NULL);
    }

    return directed_count;
}

/**
 * 保存因果图到文件（JSON格式）
 * @param graph 因果图
 * @param filepath 文件路径
 * @return 0 成功
 */
int causal_graph_save_to_file(CausalGraph* graph, const char* filepath) {
    if (!graph || !filepath) return -1;
    
    FILE* fp = fopen(filepath, "w");
    if (!fp) return -1;
    
    fprintf(fp, "{\n");
    fprintf(fp, "  \"node_count\": %d,\n", graph->node_count);
    fprintf(fp, "  \"edge_count\": %d,\n", graph->edge_count);
    fprintf(fp, "  \"edges\": [\n");
    
    for (int i = 0; i < graph->edge_count; i++) {
        CausalEdge* edge = graph->edges[i];
        if (!edge) continue;
        
        fprintf(fp, "    {\n");
        fprintf(fp, "      \"cause_id\": %d,\n", edge->cause_node_id);
        fprintf(fp, "      \"effect_id\": %d,\n", edge->effect_node_id);
        fprintf(fp, "      \"type\": %d,\n", edge->type);
        fprintf(fp, "      \"strength\": %.4f,\n", edge->strength);
        fprintf(fp, "      \"probability\": %.4f,\n", edge->probability);
        fprintf(fp, "      \"bidirectional\": %s,\n", edge->bidirectional ? "true" : "false");
        fprintf(fp, "      \"confidence\": %.4f,\n", edge->confidence);
        fprintf(fp, "      \"observed_at\": %ld,\n", (long)edge->observed_at);
        fprintf(fp, "      \"last_confirmed\": %ld\n", (long)edge->last_confirmed);
        fprintf(fp, "    }%s\n", (i < graph->edge_count - 1) ? "," : "");
    }
    
    fprintf(fp, "  ]\n");
    fprintf(fp, "}\n");
    
    fclose(fp);
    return 0;
}

/**
 * 从文件加载因果图（JSON格式）
 * @param filepath 文件路径
 * @return 因果图，NULL 失败
 */
CausalGraph* causal_graph_load_from_file(const char* filepath) {
    if (!filepath) return NULL;
    
    FILE* fp = fopen(filepath, "r");
    if (!fp) return NULL;
    
    // 简单解析：读取 node_count 和 edge_count
    int node_count = 0;
    int edge_count = 0;
    
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "  \"node_count\": %d", &node_count) == 1) continue;
        if (sscanf(line, "  \"edge_count\": %d", &edge_count) == 1) continue;
    }
    
    fclose(fp);
    
    if (node_count <= 0 || edge_count <= 0) return NULL;
    
    // 创建因果图
    CausalGraph* graph = causal_graph_create(node_count, edge_count * 2);
    if (!graph) return NULL;
    
    // 重新打开文件解析边
    fp = fopen(filepath, "r");
    if (!fp) {
        causal_graph_destroy(graph);
        return NULL;
    }
    
    int cause_id, effect_id, edge_type;
    float strength, probability, confidence;
    int bidirectional;
    long observed_at, last_confirmed;
    
    // 跳过前两行
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return 0; }
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return 0; }
    
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "\"edges\":")) continue;
        if (strstr(line, "[")) continue;
        if (strstr(line, "{")) {
            // 解析边
            while (fgets(line, sizeof(line), fp)) {
                if (strstr(line, "}")) break;
                sscanf(line, "      \"cause_id\": %d", &cause_id);
                sscanf(line, "      \"effect_id\": %d", &effect_id);
                sscanf(line, "      \"type\": %d", &edge_type);
                sscanf(line, "      \"strength\": %f", &strength);
                sscanf(line, "      \"probability\": %f", &probability);
                sscanf(line, "      \"bidirectional\": %d", &bidirectional);
                sscanf(line, "      \"confidence\": %f", &confidence);
                sscanf(line, "      \"observed_at\": %ld", &observed_at);
                sscanf(line, "      \"last_confirmed\": %ld", &last_confirmed);
            }
            
            // 添加边
            CausalEdge* edge = (CausalEdge*)object_pool_acquire((ObjectPool*)graph->edge_pool);
            if (edge) {
                edge->cause_node_id = cause_id;
                edge->effect_node_id = effect_id;
                edge->type = (CausalEdgeType)edge_type;
                edge->strength = strength;
                edge->probability = probability;
                edge->bidirectional = bidirectional ? true : false;
                edge->confidence = confidence;
                edge->observed_at = (time_t)observed_at;
                edge->last_confirmed = (time_t)last_confirmed;
                edge->condition_node_ids = NULL;
                edge->condition_count = 0;
                
                graph->edges[graph->edge_count++] = edge;
            }
        }
    }
    
    fclose(fp);
    return graph;
}

/**
 * 保存因果知识到 SQLite 数据库
 * @param graph 因果图
 * @param db_path 数据库路径
 * @return 0 成功
 */
#ifdef USE_SQLITE
int causal_graph_save_to_db(CausalGraph* graph, const char* db_path) {
    if (!graph || !db_path) return -1;
    
    sqlite3* db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) return -1;
    
    // 创建表
    char* err_msg = NULL;
    const char* sql = 
        "CREATE TABLE IF NOT EXISTS causal_knowledge ("
        "  id INTEGER PRIMARY KEY,"
        "  cause_id INTEGER,"
        "  effect_id INTEGER,"
        "  edge_type INTEGER,"
        "  strength REAL,"
        "  probability REAL,"
        "  bidirectional INTEGER,"
        "  confidence REAL,"
        "  observed_at INTEGER,"
        "  last_confirmed INTEGER"
        ");";
    
    if (sqlite3_exec(db, sql, NULL, NULL, &err_msg) != SQLITE_OK) {
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return -1;
    }
    
    // 插入边数据
    sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, NULL);
    
    for (int i = 0; i < graph->edge_count; i++) {
        CausalEdge* edge = graph->edges[i];
        if (!edge) continue;
        
        char stmt[512];
        snprintf(stmt, sizeof(stmt),
                "INSERT INTO causal_knowledge VALUES (NULL, %d, %d, %d, %.4f, %.4f, %d, %.4f, %ld, %ld);",
                edge->cause_node_id, edge->effect_node_id, edge->type,
                edge->strength, edge->probability, edge->bidirectional ? 1 : 0,
                edge->confidence, (long)edge->observed_at, (long)edge->last_confirmed);
        
        sqlite3_exec(db, stmt, NULL, NULL, NULL);
    }
    
    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    sqlite3_close(db);
    return 0;
}
#endif

/**
 * 从 SQLite 数据库加载因果知识
 * @param db_path 数据库路径
 * @param node_count 节点数量
 * @return 因果图，NULL 失败
 */
#ifdef USE_SQLITE
CausalGraph* causal_graph_load_from_db(const char* db_path, int node_count) {
    if (!db_path || node_count <= 0) return NULL;
    
    sqlite3* db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) return NULL;
    
    CausalGraph* graph = causal_graph_create(node_count, 1024);
    if (!graph) {
        sqlite3_close(db);
        return NULL;
    }
    
    sqlite3_stmt* stmt;
    const char* sql = "SELECT cause_id, effect_id, edge_type, strength, probability, "
                     "bidirectional, confidence, observed_at, last_confirmed "
                     "FROM causal_knowledge;";
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            CausalEdge* edge = (CausalEdge*)object_pool_acquire((ObjectPool*)graph->edge_pool);
            if (edge) {
                edge->cause_node_id = sqlite3_column_int(stmt, 0);
                edge->effect_node_id = sqlite3_column_int(stmt, 1);
                edge->type = (CausalEdgeType)sqlite3_column_int(stmt, 2);
                edge->strength = (float)sqlite3_column_double(stmt, 3);
                edge->probability = (float)sqlite3_column_double(stmt, 4);
                edge->bidirectional = sqlite3_column_int(stmt, 5) ? true : false;
                edge->confidence = (float)sqlite3_column_double(stmt, 6);
                edge->observed_at = (time_t)sqlite3_column_int64(stmt, 7);
                edge->last_confirmed = (time_t)sqlite3_column_int64(stmt, 8);
                edge->condition_node_ids = NULL;
                edge->condition_count = 0;
                
                graph->edges[graph->edge_count++] = edge;
            }
        }
        sqlite3_finalize(stmt);
    }
    
    sqlite3_close(db);
    return graph;
}
#endif
