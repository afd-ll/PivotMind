#include "../include/huarong_topology.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ==================== 状态管理实现 ==================== 

HuarongState* huarong_state_create(int* board, int size) {
    HuarongState* state = (HuarongState*)malloc(sizeof(HuarongState));
    if (!state) return NULL;
    
    state->state_size = size;
    state->board_state = (int*)malloc(size * sizeof(int));
    state->reasoning_path = (float*)malloc(size * sizeof(float));
    
    if (!state->board_state || !state->reasoning_path) {
        free(state->board_state);
        free(state->reasoning_path);
        free(state);
        return NULL;
    }
    
    memcpy(state->board_state, board, size * sizeof(int));
    memset(state->reasoning_path, 0, size * sizeof(float));
    
    state->prev = NULL;
    state->next = NULL;
    state->move_count = 0;
    state->confidence = 1.0f;
    state->move_direction = -1;
    state->hash = huarong_state_hash(state);
    
    return state;
}

void huarong_state_destroy(HuarongState* state) {
    if (!state) return;
    free(state->board_state);
    free(state->reasoning_path);
    free(state);
}

unsigned long huarong_state_hash(HuarongState* state) {
    if (!state || !state->board_state) return 0;
    
    unsigned long hash = 5381;
    for (int i = 0; i < state->state_size; i++) {
        hash = ((hash << 5) + hash) + state->board_state[i];
    }
    return hash;
}

int huarong_state_equal(HuarongState* state1, HuarongState* state2) {
    if (!state1 || !state2) return 0;
    if (state1->state_size != state2->state_size) return 0;
    
    return memcmp(state1->board_state, state2->board_state, 
                  state1->state_size * sizeof(int)) == 0;
}

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
    
    // 分配特征向量
    node->features = (float*)malloc(feature_dim * sizeof(float));
    if (features) {
        memcpy(node->features, features, feature_dim * sizeof(float));
    } else {
        memset(node->features, 0, feature_dim * sizeof(float));
    }
    
    // 初始化连接数组（预分配10个连接空间）
    node->connections = (ReasoningNode**)malloc(10 * sizeof(ReasoningNode*));
    node->connection_weights = (float*)malloc(10 * sizeof(float));
    node->connection_motivational_bias = (float*)malloc(10 * sizeof(float));
    node->connection_confidences = (float*)malloc(10 * sizeof(float));
    node->connection_capacity = 10;
    
    // 初始化权重和动机倾向
    for (int i = 0; i < 10; i++) {
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

HuarongTopologyNet* huarong_net_create(int max_nodes, int max_state_history) {
    HuarongTopologyNet* net = (HuarongTopologyNet*)malloc(sizeof(HuarongTopologyNet));
    if (!net) return NULL;
    
    net->nodes = (ReasoningNode**)calloc(max_nodes, sizeof(ReasoningNode*));
    net->max_nodes = max_nodes;  // 保存最大节点数
    net->node_count = 0;
    net->current_state = NULL;
    net->initial_state = NULL;
    net->target_state = NULL;
    net->max_state_history = max_state_history;
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
    
    // 销毁状态
    if (net->current_state) huarong_state_destroy(net->current_state);
    if (net->initial_state) huarong_state_destroy(net->initial_state);
    if (net->target_state) huarong_state_destroy(net->target_state);
    
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
        from_node->connections = (ReasoningNode**)realloc(from_node->connections, new_cap * sizeof(ReasoningNode*));
        from_node->connection_weights = (float*)realloc(from_node->connection_weights, new_cap * sizeof(float));
        from_node->connection_motivational_bias = (float*)realloc(from_node->connection_motivational_bias, new_cap * sizeof(float));
        from_node->connection_confidences = (float*)realloc(from_node->connection_confidences, new_cap * sizeof(float));
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

// ==================== 前向推理实现 ==================== 

HuarongState* huarong_net_forward(HuarongTopologyNet* net, 
                                 HuarongState* current_state, 
                                 int* action) {
    if (!net || !current_state || !action) return NULL;
    
    // 创建新状态（复制当前状态）
    HuarongState* new_state = huarong_state_create(current_state->board_state, 
                                                   current_state->state_size);
    if (!new_state) return NULL;
    
    // 根据action执行状态转换
    int move_direction = action[0];
    int move_position = action[1];
    
    // 执行移动操作（这里简化实现，实际需要根据华容道规则）
    if (move_position >= 0 && move_position < new_state->state_size) {
        // 简化的状态转换逻辑
        new_state->board_state[move_position] = 
            (new_state->board_state[move_position] + move_direction) % 10;
    }
    
    // 更新状态信息
    new_state->prev = current_state;
    current_state->next = new_state;
    new_state->move_count = current_state->move_count + 1;
    new_state->move_direction = move_direction;
    
    // 计算置信度（基于推理路径）
    new_state->confidence = current_state->confidence * 0.95f; // 衰减因子
    
    // 更新推理路径权重
    for (int i = 0; i < new_state->state_size && i < current_state->state_size; i++) {
        new_state->reasoning_path[i] = current_state->reasoning_path[i] + 
                                     (action[0] * 0.1f); // 简化计算
    }
    
    return new_state;
}

// ==================== 反向推理实现 ==================== 

HuarongState* huarong_net_backward(HuarongTopologyNet* net, 
                                  HuarongState* current_state) {
    if (!net || !current_state) return NULL;
    
    // 如果已经是初始状态，无法继续回退
    if (!current_state->prev) return NULL;
    
    // 返回到前一个状态
    HuarongState* prev_state = current_state->prev;
    
    // 可选：验证反向操作的正确性
    int reverse_action[2];
    reverse_action[0] = -current_state->move_direction; // 反向移动
    reverse_action[1] = current_state->move_count;      // 位置信息
    
    // 这里可以添加反向验证逻辑
    printf("反向推理：从状态%d回退到状态%d，反向操作[%d, %d]\n", 
           current_state->move_count, prev_state->move_count,
           reverse_action[0], reverse_action[1]);
    
    return prev_state;
}

// ==================== 拓扑排序实现 ==================== 

int* huarong_net_topological_sort(HuarongTopologyNet* net, int* path_length) {
    if (!net || !path_length) return NULL;
    
    int* sorted_nodes = (int*)malloc(net->node_count * sizeof(int));
    if (!sorted_nodes) return NULL;
    
    int* in_degree = (int*)calloc(net->node_count, sizeof(int));
    int* visited = (int*)calloc(net->node_count, sizeof(int));
    
    // 计算每个节点的入度
    for (int i = 0; i < net->node_count; i++) {
        ReasoningNode* node = net->nodes[i];
        for (int j = 0; j < node->connection_count; j++) {
            for (int k = 0; k < net->node_count; k++) {
                if (net->nodes[k] == node->connections[j]) {
                    in_degree[k]++;
                    break;
                }
            }
        }
    }
    
    // Kahn算法进行拓扑排序
    int sorted_count = 0;
    while (sorted_count < net->node_count) {
        int found = 0;
        for (int i = 0; i < net->node_count; i++) {
            if (!visited[i] && in_degree[i] == 0) {
                sorted_nodes[sorted_count++] = i;
                visited[i] = 1;
                found = 1;
                
                // 更新相邻节点的入度
                ReasoningNode* node = net->nodes[i];
                for (int j = 0; j < node->connection_count; j++) {
                    for (int k = 0; k < net->node_count; k++) {
                        if (net->nodes[k] == node->connections[j]) {
                            in_degree[k]--;
                            break;
                        }
                    }
                }
                break;
            }
        }
        
        if (!found) {
            // 存在环，无法进行拓扑排序
            printf("警告：网络中存在环，无法进行拓扑排序\n");
            break;
        }
    }
    
    *path_length = sorted_count;
    
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
    
    // 移除节点并重新组织数组
    destroy_reasoning_node(net->nodes[node_id]);
    
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
                new_connection_count++;
            }
        }
        
        node->connection_count = new_connection_count;
    }
    
    printf("网络优化完成：移除了权重小于%.3f的冗余连接\n", threshold);
}

// ==================== 可视化函数 ==================== 

void huarong_net_visualize_path(HuarongState** path, int path_length) {
    if (!path || path_length <= 0) {
        printf("无效的推理路径\n");
        return;
    }
    
    printf("\n=== 推理路径可视化 ===\n");
    for (int i = 0; i < path_length; i++) {
        printf("步骤 %d: 状态[", i);
        for (int j = 0; j < path[i]->state_size && j < 10; j++) {
            printf("%d", path[i]->board_state[j]);
            if (j < path[i]->state_size - 1 && j < 9) printf(",");
        }
        printf("] 置信度: %.3f\n", path[i]->confidence);
    }
    printf("=== 路径结束 ===\n\n");
}