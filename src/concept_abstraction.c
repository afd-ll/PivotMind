#include "../include/concept_abstraction.h"
#include "../include/huarong_topology.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdio.h>

// ==================== 宏定义 ====================

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))

static const char* LEVEL_NAMES[CONCEPT_LEVEL_COUNT] = {
    "具体", "感觉运动", "空间", "时间", "分类", "因果", "意图"
};

// ==================== ID 生成器（线程安全改进）====================

static int global_next_concept_id = 0;

static int generate_concept_id(void) {
    return __sync_fetch_and_add(&global_next_concept_id, 1);
}

// ==================== 环路检测 ====================

// 检查将 source 设为 target 的子节点是否会形成环路
// 环路条件：target 已经是 source 的后代
static bool would_create_cycle(ConceptHierarchy* hierarchy, int source_id, int target_id) {
    if (!hierarchy || source_id < 0 || target_id < 0) return false;

    // 从 target 向上遍历到根，如果遇到 source 则形成环路
    int visited[256] = {0};
    int visited_count = 0;
    int current = target_id;

    while (current >= 0) {
        if (current == source_id) return true;  // 发现环路

        // 防止无限循环
        for (int i = 0; i < visited_count; i++) {
            if (visited[i] == current) return true;
        }
        if (visited_count < 256) visited[visited_count++] = current;

        ConceptNode* node = concept_hierarchy_get(hierarchy, current);
        if (!node) break;
        current = node->parent_id;
    }
    return false;
}

// ==================== 概念层级管理 ==========

ConceptHierarchy* concept_hierarchy_create(int capacity) {
    if (capacity <= 0) capacity = 16;

    ConceptHierarchy* hierarchy = (ConceptHierarchy*)malloc(sizeof(ConceptHierarchy));
    if (!hierarchy) return NULL;

    hierarchy->nodes = (ConceptNode**)calloc(capacity, sizeof(ConceptNode*));
    if (!hierarchy->nodes) {
        free(hierarchy);
        return NULL;
    }

    // 添加 ID 到索引的映射数组以加速查找
    hierarchy->id_index = (int*)malloc(capacity * sizeof(int));
    if (!hierarchy->id_index) {
        free(hierarchy->nodes);
        free(hierarchy);
        return NULL;
    }
    // 初始化为 -1 表示无效
    for (int i = 0; i < capacity; i++) {
        hierarchy->id_index[i] = -1;
    }

    hierarchy->node_count = 0;
    hierarchy->capacity = capacity;
    hierarchy->root_concept_id = -1;

    for (int i = 0; i < CONCEPT_LEVEL_COUNT; i++) {
        hierarchy->level_counts[i] = 0;
    }

    return hierarchy;
}

void concept_hierarchy_destroy(ConceptHierarchy* hierarchy) {
    if (!hierarchy) return;

    for (int i = 0; i < hierarchy->node_count; i++) {
        if (hierarchy->nodes[i]) {
            concept_node_destroy(hierarchy->nodes[i]);
        }
    }
    free(hierarchy->nodes);
    if (hierarchy->id_index) {
        free(hierarchy->id_index);
    }
    free(hierarchy);
}

ConceptNode* concept_node_create(const char* name, ConceptLevel level) {
    ConceptNode* node = (ConceptNode*)malloc(sizeof(ConceptNode));
    if (!node) return NULL;

    node->concept_id = generate_concept_id();
    node->name = name;
    node->level = level;

    // 使用宏进行安全的内存分配和初始化
    node->concrete_nodes = (int*)malloc(16 * sizeof(int));
    if (!node->concrete_nodes) {
        free(node);
        return NULL;
    }
    node->concrete_count = 0;
    node->concrete_capacity = 16;

    node->child_ids = (int*)malloc(16 * sizeof(int));
    if (!node->child_ids) {
        free(node->concrete_nodes);
        free(node);
        return NULL;
    }
    node->child_count = 0;
    node->child_capacity = 16;

    node->abstraction_strength = 0.5f;
    node->generalization_degree = 0.0f;
    node->typicality = 0.5f;
    node->parent_id = -1;
    node->activation = 0.0f;
    node->created_at = time(NULL);
    node->last_accessed = time(NULL);

    return node;
}

void concept_node_destroy(ConceptNode* node) {
    if (!node) return;
    if (node->concrete_nodes) free(node->concrete_nodes);
    if (node->child_ids) free(node->child_ids);
    free(node);
}

int concept_hierarchy_add(ConceptHierarchy* hierarchy, ConceptNode* concept) {
    if (!hierarchy || !concept) return -1;

    // 扩展容量（包括 nodes 和 id_index）
    if (hierarchy->node_count >= hierarchy->capacity) {
        int new_cap = hierarchy->capacity * 2;

        ConceptNode** new_nodes = (ConceptNode**)realloc(
            hierarchy->nodes, new_cap * sizeof(ConceptNode*));
        if (!new_nodes) return -1;
        hierarchy->nodes = new_nodes;

        // 同时扩展 id_index 数组
        int* new_id_index = (int*)realloc(hierarchy->id_index, new_cap * sizeof(int));
        if (!new_id_index) return -1;
        hierarchy->id_index = new_id_index;

        // 初始化新分配的索引为 -1
        for (int i = hierarchy->capacity; i < new_cap; i++) {
            hierarchy->id_index[i] = -1;
        }
        hierarchy->capacity = new_cap;
    }

    int id = concept->concept_id;
    int index = hierarchy->node_count;

    // 更新 ID 到索引的映射
    if (id >= 0 && id < hierarchy->capacity) {
        hierarchy->id_index[id] = index;
    }

    hierarchy->nodes[index] = concept;
    hierarchy->node_count++;

    // 更新层级计数（边界检查）
    if (concept->level >= 0 && concept->level < CONCEPT_LEVEL_COUNT) {
        hierarchy->level_counts[concept->level]++;
    }

    return id;
}

ConceptNode* concept_hierarchy_get(ConceptHierarchy* hierarchy, int concept_id) {
    if (!hierarchy) return NULL;

    // 使用索引数组进行 O(1) 查找
    if (concept_id >= 0 && concept_id < hierarchy->capacity) {
        int index = hierarchy->id_index[concept_id];
        if (index >= 0 && index < hierarchy->node_count) {
            return hierarchy->nodes[index];
        }
    }

    // 回退到线性搜索（处理 ID 超出索引范围的情况）
    for (int i = 0; i < hierarchy->node_count; i++) {
        if (hierarchy->nodes[i] && hierarchy->nodes[i]->concept_id == concept_id) {
            return hierarchy->nodes[i];
        }
    }
    return NULL;
}

int concept_set_parent(ConceptHierarchy* hierarchy, int child_id, int parent_id) {
    if (!hierarchy) return -1;

    // 参数自身检查
    if (child_id == parent_id) return -1;

    ConceptNode* child = concept_hierarchy_get(hierarchy, child_id);
    ConceptNode* parent = concept_hierarchy_get(hierarchy, parent_id);

    if (!child || !parent) return -1;

    // 检查层级关系是否合理（父节点必须比子节点更抽象）
    if (parent->level <= child->level) return -1;

    // 检测环路：如果 parent 是 child 的后代，则会形成环路
    if (would_create_cycle(hierarchy, parent_id, child_id)) {
        return -1;  // 拒绝会造成环路的操作
    }

    // 如果已经有父节点，移除
    if (child->parent_id >= 0) {
        ConceptNode* old_parent = concept_hierarchy_get(hierarchy, child->parent_id);
        if (old_parent) {
            for (int i = 0; i < old_parent->child_count; i++) {
                if (old_parent->child_ids[i] == child_id) {
                    for (int j = i; j < old_parent->child_count - 1; j++) {
                        old_parent->child_ids[j] = old_parent->child_ids[j + 1];
                    }
                    old_parent->child_count--;
                    break;
                }
            }
        }
    }

    // 设置新父节点
    child->parent_id = parent_id;

    // 添加到父节点的子节点列表
    if (parent->child_count >= parent->child_capacity) {
        int new_cap = parent->child_capacity * 2;
        int* new_children = (int*)realloc(parent->child_ids, new_cap * sizeof(int));
        if (!new_children) return -1;  // 内存分配失败
        parent->child_ids = new_children;
        parent->child_capacity = new_cap;
    }
    parent->child_ids[parent->child_count++] = child_id;

    return 0;
}

// ==================== 模式提取 ==========

int find_common_patterns(HuarongTopologyNet* net, int* node_ids, int count,
                        char*** output_patterns) {
    if (!net || !node_ids || count < 2) return 0;

    // 统计每个节点与其他节点的连接
    int* connection_counts = (int*)calloc(net->node_count, sizeof(int));
    if (!connection_counts) return 0;

    for (int i = 0; i < count; i++) {
        int node_id = node_ids[i];
        if (node_id < 0 || node_id >= net->node_count) continue;

        ReasoningNode* node = net->nodes[node_id];
        if (!node) continue;

        for (int j = 0; j < node->connection_count; j++) {
            int target_id = node->connections[j]->node_id;
            if (target_id >= 0 && target_id < net->node_count) {
                connection_counts[target_id]++;
            }
        }
    }

    // 找出被多个节点连接的共同目标
    int* pattern_node_ids = (int*)malloc(net->node_count * sizeof(int));
    if (!pattern_node_ids) {
        free(connection_counts);
        return 0;
    }

    int pattern_count = 0;
    for (int i = 0; i < net->node_count; i++) {
        if (connection_counts[i] >= 2) {  // 被至少两个节点连接
            pattern_node_ids[pattern_count++] = i;
        }
    }

    free(connection_counts);

    if (output_patterns && pattern_count > 0) {
        // 限制输出数量避免内存问题
        int output_count = MIN(pattern_count, 10);
        *output_patterns = (char**)malloc(output_count * sizeof(char*));
        if (!*output_patterns) {
            free(pattern_node_ids);
            return 0;
        }

        for (int i = 0; i < output_count; i++) {
            (*output_patterns)[i] = (char*)malloc(32);
            if ((*output_patterns)[i]) {
                snprintf((*output_patterns)[i], 32, "pattern_%d", pattern_node_ids[i]);
            } else {
                // 分配失败，清理已分配的
                for (int j = 0; j < i; j++) {
                    free((*output_patterns)[j]);
                }
                free(*output_patterns);
                *output_patterns = NULL;
                free(pattern_node_ids);
                return 0;
            }
        }
    }

    free(pattern_node_ids);
    return pattern_count;
}

int extract_common_features(HuarongTopologyNet* net, int* node_ids, int count,
                           float* feature_weights) {
    if (!net || !node_ids || count < 2 || !feature_weights) return 0;

    // 简化实现：基于激活值的共同特征
    float total_activation = 0.0f;
    int valid_count = 0;

    for (int i = 0; i < count; i++) {
        int node_id = node_ids[i];
        if (node_id < 0 || node_id >= net->node_count) continue;

        ReasoningNode* node = net->nodes[node_id];
        if (!node) continue;

        total_activation += node->activation;
        valid_count++;
    }

    if (valid_count == 0) return 0;

    float avg_activation = total_activation / valid_count;

    // 只有一个特征：平均激活值
    feature_weights[0] = avg_activation;
    feature_weights[1] = 1.0f - avg_activation;  // 方差代理

    return 2;  // 返回两个特征
}

int recognize_hierarchical_patterns(HuarongTopologyNet* net, ConceptLevel /*level*/,
                                   char*** patterns) {
    if (!net) return 0;

    // 按层级分类现有节点
    int level_node_count[CONCEPT_LEVEL_COUNT] = {0};

    for (int i = 0; i < net->node_count; i++) {
        ReasoningNode* node = net->nodes[i];
        if (!node) continue;

        // 根据连接数大致分类
        if (node->connection_count < 3) {
            level_node_count[CONCRETE]++;
        } else if (node->connection_count < 6) {
            level_node_count[CATEGORICAL]++;
        } else {
            level_node_count[CAUSAL]++;
        }
    }

    if (patterns) {
        *patterns = (char**)malloc(3 * sizeof(char*));
        if (!*patterns) return 0;

        for (int i = 0; i < 3 && i < CONCEPT_LEVEL_COUNT; i++) {
            (*patterns)[i] = (char*)malloc(64);
            if ((*patterns)[i]) {
                snprintf((*patterns)[i], 64, "level_%s: %d nodes",
                        LEVEL_NAMES[i], level_node_count[i]);
            }
        }
    }

    return 3;
}

// ==================== 概念层次构建 ==========

// 综合多维度指标判定概念层级
// 考虑因素：连接数、激活值、连接的目标多样性、节点是否有 concept 名称
static ConceptLevel determine_concept_level(ReasoningNode* node, int net_node_count) {
    if (!node) return CONCRETE;

    float score = 0.0f;

    // 因素1：连接数占总节点的比例（0-30分）
    float connection_ratio = (net_node_count > 0)
        ? (float)node->connection_count / (float)net_node_count
        : 0.0f;
    score += CLAMP(connection_ratio * 30.0f, 0.0f, 30.0f);

    // 因素2：绝对连接数（0-25分）
    // 0连接=0分, 1-2连接=5分, 3-5=12分, 6-9=20分, 10+=25分
    if (node->connection_count >= 10) score += 25.0f;
    else if (node->connection_count >= 6) score += 20.0f;
    else if (node->connection_count >= 3) score += 12.0f;
    else if (node->connection_count >= 1) score += 5.0f;

    // 因素3：激活值（0-25分）
    // 高激活值说明节点频繁被访问，可能是重要概念
    score += CLAMP(node->activation * 25.0f, 0.0f, 25.0f);

    // 因素4：连接目标多样性（0-20分）
    // 连接到不同节点的比例越高，抽象程度越高
    int unique_targets = 0;
    if (node->connection_count > 0) {
        // 简化：连接数本身就是目标数量的代理
        unique_targets = node->connection_count;
    }
    float diversity = (node->connection_count > 0)
        ? (float)unique_targets / (float)node->connection_count
        : 0.0f;
    score += diversity * 20.0f;

    // 映射到层级（总分 0-100）
    if (score >= 70) return INTENTIONAL;
    if (score >= 55) return CAUSAL;
    if (score >= 40) return CATEGORICAL;
    if (score >= 28) return TEMPORAL;
    if (score >= 18) return SPATIAL;
    if (score >= 8)  return SENSORIMTOR;
    return CONCRETE;
}

int build_concept_hierarchy(HuarongTopologyNet* net, ConceptHierarchy* hierarchy,
                           void* /*config*/) {
    if (!net || !hierarchy) return -1;

    for (int i = 0; i < net->node_count; i++) {
        ReasoningNode* node = net->nodes[i];
        if (!node) continue;

        // 使用多维度评分判定层级
        ConceptLevel level = determine_concept_level(node, net->node_count);

        // 创建概念（ID 由 concept_node_create 自动生成）
        ConceptNode* concept = concept_node_create(node->concept, level);
        if (!concept) continue;

        concept->activation = node->activation;
        concept->concrete_nodes[concept->concrete_count++] = i;

        // 添加到层次
        concept_hierarchy_add(hierarchy, concept);

        // 如果是具体节点，查找更抽象的父概念候选
        if (level < CATEGORICAL && node->connection_count > 0) {
            // 选择连接最多且层级比自己高的节点作为父概念
            float best_score = -1.0f;
            int parent_node_id = -1;
            for (int j = 0; j < node->connection_count; j++) {
                int target_id = node->connections[j]->node_id;
                if (target_id >= 0 && target_id < net->node_count) {
                    ReasoningNode* target = net->nodes[target_id];
                    if (!target) continue;

                    // 只考虑比自己连接数更多的目标作为父概念
                    if (target->connection_count > node->connection_count) {
                        float target_score = target->connection_count + target->activation * 5.0f;
                        if (target_score > best_score) {
                            best_score = target_score;
                            parent_node_id = target_id;
                        }
                    }
                }
            }
            concept->parent_id = parent_node_id;
        }
    }

    return 0;
}

int abstract_to_higher_level(HuarongTopologyNet* net, ConceptHierarchy* hierarchy,
                            int* concrete_node_ids, int count,
                            ConceptLevel target_level) {
    if (!net || !hierarchy || !concrete_node_ids || count <= 0) return -1;
    if (target_level <= CONCRETE) return -1;

    // 创建新概念（ID 自动生成）
    char name[64];
    snprintf(name, sizeof(name), "abstract_%d_%d", target_level, count);

    ConceptNode* new_concept = concept_node_create(name, target_level);
    if (!new_concept) return -1;

    // 确保有足够容量存放具体节点
    if (count > new_concept->concrete_capacity) {
        int new_cap = count + 16;
        int* new_nodes = (int*)realloc(new_concept->concrete_nodes, new_cap * sizeof(int));
        if (!new_nodes) {
            concept_node_destroy(new_concept);
            return -1;
        }
        new_concept->concrete_nodes = new_nodes;
        new_concept->concrete_capacity = new_cap;
    }

    // 设置关联的具体节点
    for (int i = 0; i < count; i++) {
        new_concept->concrete_nodes[new_concept->concrete_count++] = concrete_node_ids[i];
    }

    // 计算抽象强度
    new_concept->abstraction_strength = 0.5f + (float)(count - 1) / 20.0f;
    new_concept->abstraction_strength = CLAMP(new_concept->abstraction_strength, 0.0f, 1.0f);

    // 设置父概念：逐层向上查找已有概念
    int parent_id = -1;
    for (ConceptLevel l = CONCRETE + 1; l <= target_level; l++) {
        for (int i = 0; i < hierarchy->node_count; i++) {
            ConceptNode* existing = hierarchy->nodes[i];
            if (existing && existing->level == (ConceptLevel)(l - 1) && parent_id < 0) {
                parent_id = existing->concept_id;
                break;
            }
        }
    }
    new_concept->parent_id = parent_id;

    // 添加到层次
    int concept_id = concept_hierarchy_add(hierarchy, new_concept);

    return concept_id;
}

int instantiate_concept(HuarongTopologyNet* net, ConceptHierarchy* hierarchy,
                      int concept_id, int** output_node_ids) {
    if (!net || !hierarchy || !output_node_ids) return 0;

    ConceptNode* concept = concept_hierarchy_get(hierarchy, concept_id);
    if (!concept) return 0;

    // 返回关联的具体节点
    *output_node_ids = (int*)malloc(concept->concrete_count * sizeof(int));
    if (!*output_node_ids) return 0;

    for (int i = 0; i < concept->concrete_count; i++) {
        (*output_node_ids)[i] = concept->concrete_nodes[i];
    }

    concept->last_accessed = time(NULL);
    return concept->concrete_count;
}

int specialize_concept(ConceptHierarchy* hierarchy, int concept_id, float specificity) {
    if (!hierarchy) return -1;

    ConceptNode* concept = concept_hierarchy_get(hierarchy, concept_id);
    if (!concept) return -1;

    // 降低抽象强度，增加特异性
    concept->abstraction_strength *= (1.0f - specificity);
    concept->typicality += specificity * 0.1f;
    concept->typicality = CLAMP(concept->typicality, 0.0f, 1.0f);

    return 0;
}

// ==================== 抽象推理 ==========

// 向下查找后代中匹配目标层级的最佳候选（深度优先，激活值最高优先）
static int find_descendant_at_level(ConceptHierarchy* hierarchy,
                                     int concept_id, ConceptLevel target_level) {
    ConceptNode* node = concept_hierarchy_get(hierarchy, concept_id);
    if (!node) return -1;

    // 如果当前就是目标层级，直接返回
    if (node->level == target_level) return concept_id;

    // 如果当前层级不高于目标（无法再向下），失败
    if (node->level < target_level) return -1;

    // 遍历子节点，选激活值最高的匹配后代
    int best_id = -1;
    float best_activation = -1.0f;

    for (int i = 0; i < node->child_count; i++) {
        int child_id = node->child_ids[i];
        int result = find_descendant_at_level(hierarchy, child_id, target_level);

        if (result >= 0) {
            ConceptNode* result_node = concept_hierarchy_get(hierarchy, result);
            if (result_node && result_node->activation > best_activation) {
                best_activation = result_node->activation;
                best_id = result;
            }
        }
    }

    return best_id;
}

int reason_via_abstraction(ConceptHierarchy* hierarchy, int source_concept_id,
                          ConceptLevel target_level) {
    if (!hierarchy) return -1;

    ConceptNode* source = concept_hierarchy_get(hierarchy, source_concept_id);
    if (!source) return -1;

    // 如果已经在目标层级，返回自己
    if (source->level == target_level) return source_concept_id;

    // 向上推理：目标层级更抽象（层级值更高）
    if (source->level < target_level) {
        ConceptNode* current = source;
        while (current && current->level < target_level) {
            if (current->parent_id < 0) break;
            current = concept_hierarchy_get(hierarchy, current->parent_id);
        }
        return current ? current->concept_id : -1;
    }

    // 向下推理：目标层级更具体（层级值更低）
    // 在子树中查找目标层级中激活值最高的概念
    return find_descendant_at_level(hierarchy, source_concept_id, target_level);
}

bool concept_contains(ConceptHierarchy* hierarchy, int container_id, int contained_id) {
    if (!hierarchy) return false;

    ConceptNode* container = concept_hierarchy_get(hierarchy, container_id);
    ConceptNode* contained = concept_hierarchy_get(hierarchy, contained_id);

    if (!container || !contained) return false;

    // 检查 container 是否是 contained 的祖先
    ConceptNode* current = contained;
    while (current && current->parent_id >= 0) {
        if (current->parent_id == container_id) return true;
        current = concept_hierarchy_get(hierarchy, current->parent_id);
    }

    return false;
}

int find_lowest_common_ancestor(ConceptHierarchy* hierarchy,
                               int concept_a_id, int concept_b_id) {
    if (!hierarchy) return -1;

    // 使用栈上小数组优化，避免频繁动态分配
    #define MAX_TREE_DEPTH 256
    int path_a[MAX_TREE_DEPTH];
    int path_b[MAX_TREE_DEPTH];
    int depth_a = 0, depth_b = 0;

    // 路径 A：从节点到根
    ConceptNode* node = concept_hierarchy_get(hierarchy, concept_a_id);
    while (node && depth_a < MAX_TREE_DEPTH) {
        path_a[depth_a++] = node->concept_id;
        if (node->parent_id < 0) break;
        node = concept_hierarchy_get(hierarchy, node->parent_id);
    }

    // 路径 B：从节点到根
    node = concept_hierarchy_get(hierarchy, concept_b_id);
    while (node && depth_b < MAX_TREE_DEPTH) {
        path_b[depth_b++] = node->concept_id;
        if (node->parent_id < 0) break;
        node = concept_hierarchy_get(hierarchy, node->parent_id);
    }

    // 从根开始找最后一个共同节点
    int lca = -1;
    int min_depth = MIN(depth_a, depth_b);

    for (int i = 0; i < min_depth; i++) {
        if (path_a[depth_a - 1 - i] == path_b[depth_b - 1 - i]) {
            lca = path_a[depth_a - 1 - i];
        } else {
            break;
        }
    }

    return lca;
    #undef MAX_TREE_DEPTH
}

// ==================== 抽象规则引擎 ==========

AbstractionRule* abstraction_rule_create(AbstractionRuleType type,
                                       float threshold, float strength) {
    AbstractionRule* rule = (AbstractionRule*)malloc(sizeof(AbstractionRule));
    if (!rule) return NULL;

    rule->type = type;
    rule->threshold = threshold;
    rule->strength = strength;
    rule->enabled = true;

    return rule;
}

void abstraction_rule_destroy(AbstractionRule* rule) {
    if (rule) free(rule);
}

int apply_abstraction_rule(HuarongTopologyNet* net, ConceptHierarchy* hierarchy,
                          AbstractionRule* rule, int* node_ids, int count) {
    if (!net || !hierarchy || !rule || !node_ids || count <= 0) return -1;
    if (!rule->enabled) return 0;

    int new_concepts = 0;

    switch (rule->type) {
        case RULE_CLUSTERING: {
            // 聚类抽象：找相似节点，创建高层概念
            // 简化：如果节点数超过阈值，创建抽象概念
            if (count >= 3) {
                ConceptLevel target_level = CATEGORICAL;
                if (abstract_to_higher_level(net, hierarchy, node_ids, count,
                                            target_level) >= 0) {
                    new_concepts++;
                }
            }
            break;
        }

        case RULE_GENERALIZATION: {
            // 泛化抽象：合并相似特征
            // 简化：提高现有概念的泛化程度
            for (int i = 0; i < count; i++) {
                ConceptNode* concept = concept_hierarchy_get(hierarchy, node_ids[i]);
                if (concept) {
                    concept->generalization_degree += rule->strength * 0.1f;
                    concept->generalization_degree = 
                        CLAMP(concept->generalization_degree, 0.0f, 1.0f);
                }
            }
            break;
        }

        case RULE_ONTOLOGICAL: {
            // 本体抽象：建立分类层次
            // 简化：设置父子关系
            for (int i = 1; i < count; i++) {
                concept_set_parent(hierarchy, node_ids[i], node_ids[0]);
            }
            break;
        }

        case RULE_META_ABSTRACTION: {
            // 元抽象：创建关于抽象的抽象
            // 简化：提高所有涉及概念的抽象程度
            for (int i = 0; i < count; i++) {
                ConceptNode* concept = concept_hierarchy_get(hierarchy, node_ids[i]);
                if (concept) {
                    concept->abstraction_strength += rule->strength * 0.05f;
                    concept->abstraction_strength =
                        CLAMP(concept->abstraction_strength, 0.0f, 1.0f);
                }
            }
            break;
        }
    }

    return new_concepts;
}

// ==================== 便捷函数 ==========

const char* concept_level_name(ConceptLevel level) {
    if (level < 0 || level >= CONCEPT_LEVEL_COUNT) return "Unknown";
    return LEVEL_NAMES[level];
}

ConceptLevel get_node_concept_level(ConceptHierarchy* hierarchy, int node_id) {
    if (!hierarchy) return CONCRETE;

    for (int i = 0; i < hierarchy->node_count; i++) {
        ConceptNode* concept = hierarchy->nodes[i];
        if (!concept) continue;

        for (int j = 0; j < concept->concrete_count; j++) {
            if (concept->concrete_nodes[j] == node_id) {
                return concept->level;
            }
        }
    }

    return CONCRETE;  // 默认
}

float compute_abstraction_degree(ConceptHierarchy* hierarchy, int concept_id) {
    if (!hierarchy) return 0.0f;

    ConceptNode* concept = concept_hierarchy_get(hierarchy, concept_id);
    if (!concept) return 0.0f;

    // 综合考虑：层级 + 抽象强度 + 子概念数量
    float level_factor = (float)concept->level / (float)(CONCEPT_LEVEL_COUNT - 1);  // 0-1
    float strength_factor = concept->abstraction_strength;
    float child_factor = 1.0f - 1.0f / (1.0f + concept->child_count);

    return (level_factor * 0.4f + strength_factor * 0.4f + child_factor * 0.2f);
}

void concept_hierarchy_get_stats(ConceptHierarchy* hierarchy, int* level_counts,
                                float* avg_abstraction) {
    if (!hierarchy) return;

    for (int i = 0; i < CONCEPT_LEVEL_COUNT; i++) {
        level_counts[i] = hierarchy->level_counts[i];
    }

    if (avg_abstraction) {
        float total_abstraction = 0.0f;
        int count = 0;
        for (int i = 0; i < hierarchy->node_count; i++) {
            ConceptNode* node = hierarchy->nodes[i];
            if (node) {
                total_abstraction += compute_abstraction_degree(hierarchy, node->concept_id);
                count++;
            }
        }
        *avg_abstraction = (count > 0) ? total_abstraction / count : 0.0f;
    }
}
