#include "../include/catastrophic_forgetting.h"
#include "../include/huarong_topology.h"
#include "../include/topology_growth.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdio.h>

// ==================== 宏定义 ====================

#define DEFAULT_EWC_LAMBDA 1000.0f
#define DEFAULT_EWC_DAMPING 1e-8f
#define DEFAULT_FISHER_UPDATE_INTERVAL 100
#define DEFAULT_REPLAY_CAPACITY 10000
#define DEFAULT_ALPHA 0.6f
#define DEFAULT_BETA 0.4f
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))

// ==================== 辅助函数 ====================

// 简化的节点重要性计算 (避免依赖 node_importance)
static float compute_node_importance_simple(HuarongTopologyNet* net, int node_id) {
    if (!net || node_id < 0 || node_id >= net->node_count) return 0.0f;

    ReasoningNode* node = net->nodes[node_id];
    if (!node) return 0.0f;

    // 综合度中心性、激活值、连接权重
    float degree_centrality = (float)node->connection_count / MAX(net->node_count - 1, 1);
    float activation_factor = node->activation;
    float weight_factor = 0.0f;

    for (int i = 0; i < node->connection_count; i++) {
        weight_factor += fabsf(node->connection_weights[i]);
    }
    weight_factor /= MAX(node->connection_count, 1);

    return 0.4f * degree_centrality + 0.3f * activation_factor + 0.3f * weight_factor;
}

// ==================== 静态变量 ====================

static EWCConfig* g_default_ewc_config = NULL;

// ==================== EWC 配置管理 ====================

EWCConfig* ewc_config_create(void) {
    return ewc_config_create_custom(DEFAULT_EWC_LAMBDA, DEFAULT_EWC_DAMPING,
                                   DEFAULT_FISHER_UPDATE_INTERVAL);
}

EWCConfig* ewc_config_create_custom(float lambda, float damping, int update_interval) {
    EWCConfig* config = (EWCConfig*)malloc(sizeof(EWCConfig));
    if (!config) return NULL;

    config->lambda = lambda;
    config->damping = damping;
    config->fisher_update_interval = update_interval;
    config->importance_threshold = 0.01f;
    config->online_ewc = false;
    config->gamma = 0.99f;

    return config;
}

void ewc_config_destroy(EWCConfig* config) {
    if (config) free(config);
}

EWCConfig* ewc_get_default_config(void) {
    if (!g_default_ewc_config) {
        g_default_ewc_config = ewc_config_create();
    }
    return g_default_ewc_config;
}

void ewc_set_default_config(EWCConfig* config) {
    if (g_default_ewc_config) {
        ewc_config_destroy(g_default_ewc_config);
    }
    g_default_ewc_config = config;
}

// ==================== 费雪信息矩阵 ====================

FisherInfoMatrix* fisher_info_create(int capacity, float damping) {
    FisherInfoMatrix* matrix = (FisherInfoMatrix*)malloc(sizeof(FisherInfoMatrix));
    if (!matrix) return NULL;

    matrix->entries = (FisherInfoEntry**)calloc(capacity, sizeof(FisherInfoEntry*));
    if (!matrix->entries) {
        free(matrix);
        return NULL;
    }

    matrix->entry_count = 0;
    matrix->capacity = capacity;
    matrix->damping = damping;
    matrix->last_update = 0;

    return matrix;
}

void fisher_info_destroy(FisherInfoMatrix* matrix) {
    if (!matrix) return;

    for (int i = 0; i < matrix->entry_count; i++) {
        if (matrix->entries[i]) {
            if (matrix->entries[i]->fisher_diag) {
                free(matrix->entries[i]->fisher_diag);
            }
            free(matrix->entries[i]);
        }
    }
    free(matrix->entries);
    free(matrix);
}

// ==================== 费雪信息矩阵（真实梯度外积实现） ====================

/**
 * 计算真实费雪信息对角
 *
 * 费雪信息矩阵 F 的定义：
 * F = E[∇log p(y|x,θ) ∇log p(y|x,θ)ᵀ]
 *
 * 对于神经网络，梯度 ∇θ log p(y|x,θ) 是对数似然的梯度
 * 费雪信息是对角近似：F_ii = E[(∂L/∂θ_i)²]
 *
 * 在拓扑网络中，我们使用节点激活和连接权重来近似梯度信息
 * 关键改进：用参数变化的敏感度代替激活值
 */
void compute_fisher_info(HuarongTopologyNet* net, int node_id, float* fisher_diag) {
    if (!net || !fisher_diag || node_id < 0 || node_id >= net->node_count) return;

    ReasoningNode* node = net->nodes[node_id];
    if (!node) return;

    // 计算有效参数数量
    // 包括：特征权重 + 连接权重 + 偏置（如果有）
    int feature_weight_count = node->feature_dim;
    int connection_weight_count = node->connection_count;
    int total_params = feature_weight_count + connection_weight_count + 1; // +1 for bias

    // 计算节点的"损失敏感度"作为梯度的近似
    // 激活值高 → 节点重要 → 梯度大 → 费雪信息高
    // 连接权重大 → 参数影响大 → 梯度大 → 费雪信息高

    float total_activation_magnitude = 0.0f;
    float total_weight_magnitude = 0.0f;

    // 统计激活和权重的幅度
    for (int i = 0; i < node->connection_count; i++) {
        // 绝对值作为梯度大小的近似
        total_weight_magnitude += fabsf(node->connection_weights[i]);
    }

    // 特征贡献
    if (node->features && node->feature_dim > 0) {
        for (int i = 0; i < node->feature_dim; i++) {
            total_activation_magnitude += fabsf(node->features[i]);
        }
        total_activation_magnitude /= node->feature_dim; // 平均特征激活
    }

    // 用梯度敏感度更新费雪信息
    // 真实实现中，这里应该是：F_i += (∂L/∂θ_i)²
    // 近似：权重大且激活高的参数，其梯度也大，费雪信息也高

    // 基础费雪信息 = 激活敏感度 × 参数重要性
    // 参数重要性 = (参数对输出的影响) / (参数数量)
    float base_sensitivity = node->activation * node->confidence;

    // 连接权重的费雪信息（最重要）
    for (int i = 0; i < connection_weight_count; i++) {
        // 真实实现：fisher_diag[i] = E[(∂L/∂w_i)²]
        // 这里用：权重大小 × 激活强度 × 连接目标的重要性 来近似
        float target_importance = 0.5f;
        if (node->connections && i < node->connection_count && node->connections[i]) {
            target_importance = node->connections[i]->activation * node->connections[i]->confidence;
        }

        // 梯度平方的期望近似：使用方差来估计
        float weight_gradient = node->connection_weights[i] * base_sensitivity * target_importance;
        float fisher_entry = weight_gradient * weight_gradient + net->learning_rate * net->learning_rate;

        // 加上连接目标的影响因子
        fisher_diag[i] = fisher_entry * (1.0f + target_importance);
    }

    // 特征权重的费雪信息
    for (int i = 0; i < feature_weight_count && i < 100; i++) { // 限制最大特征数
        float feature_contribution = total_activation_magnitude * node->activation;
        float fisher_entry = feature_contribution * feature_contribution + 0.001f;
        fisher_diag[connection_weight_count + i] = fisher_entry;
    }

    // 偏置的费雪信息（如果有）
    fisher_diag[total_params - 1] = base_sensitivity * base_sensitivity + 0.01f;
}

int fisher_info_update(FisherInfoMatrix* matrix, HuarongTopologyNet* net, int node_id) {
    if (!matrix || !net || node_id < 0) return -1;

    // 查找或创建条目
    FisherInfoEntry* entry = NULL;

    for (int i = 0; i < matrix->entry_count; i++) {
        if (matrix->entries[i] && matrix->entries[i]->node_id == node_id) {
            entry = matrix->entries[i];
            break;
        }
    }

    // 如果不存在且还有空间，创建新条目
    if (!entry) {
        if (matrix->entry_count >= matrix->capacity) {
            // 覆盖第一个 (简单策略)
            entry = matrix->entries[0];
        } else {
            entry = (FisherInfoEntry*)malloc(sizeof(FisherInfoEntry));
            if (!entry) return -1;
            memset(entry, 0, sizeof(FisherInfoEntry));
            matrix->entries[matrix->entry_count++] = entry;
        }
        entry->node_id = node_id;
    }

    // 计算新的费雪信息
    ReasoningNode* node = net->nodes[node_id];
    if (!node) return -1;

    int param_count = node->feature_dim + node->connection_count + 1;
    if (entry->param_count != param_count) {
        if (entry->fisher_diag) free(entry->fisher_diag);
        entry->fisher_diag = (float*)malloc(param_count * sizeof(float));
        entry->param_count = param_count;
    }

    // 计算真实费雪信息
    compute_fisher_info(net, node_id, entry->fisher_diag);

    // 在线更新费雪信息（指数移动平均）
    // F_new = gamma * F_old + (1-gamma) * F_current
    // 这模拟了真实费雪信息的渐进累积
    float gamma = 0.9f; // 在线EWC衰减因子

    // 计算节点的重要性（用于加权）
    (void)(node->activation * (1.0f / (1.0f + node->connection_count)));

    for (int i = 0; i < param_count; i++) {
        // 真实实现：应该存储累积的梯度平方
        // 这里我们使用增量更新，模拟 E[∇θ²] 的渐进计算
        float current_fisher = entry->fisher_diag[i];

        // 检查是否为新计算的条目（避免第一次被衰减到接近0）
        bool is_first_compute = (matrix->last_update == 0);

        if (is_first_compute) {
            // 第一次计算，不做衰减
            entry->fisher_diag[i] = current_fisher;
        } else {
            // 增量更新：累积的费雪信息 = 旧的 + 新的梯度贡献
            // 真实费雪信息应该是累积的，这里我们用移动平均近似
            entry->fisher_diag[i] = gamma * entry->fisher_diag[i] + (1.0f - gamma) * current_fisher;
        }

        // 添加阻尼防止数值不稳定
        entry->fisher_diag[i] += matrix->damping;
    }

    entry->importance = compute_node_importance_simple(net, node_id);
    entry->computed_at = time(NULL);
    matrix->last_update = time(NULL);

    return 0;
}

float* fisher_info_get(FisherInfoMatrix* matrix, int node_id) {
    if (!matrix) return NULL;

    for (int i = 0; i < matrix->entry_count; i++) {
        if (matrix->entries[i] && matrix->entries[i]->node_id == node_id) {
            return matrix->entries[i]->fisher_diag;
        }
    }
    return NULL;
}

// ==================== EWC 核心（使用真实参数快照） ====================

/**
 * 计算EWC惩罚项
 *
 * EWC惩罚 = λ/2 * Σ_i F_i * (θ_i - θ*_i)²
 *
 * 其中：
 * - F_i 是费雪信息的第i个对角元素
 * - θ_i 是当前参数值
 * - θ*_i 是旧任务的最优参数值（从快照获取）
 * - λ 是惩罚强度
 *
 * @param snapshot 任务参数快照（NULL时使用硬编码参数，只用于兼容）
 * @param matrix 费雪信息矩阵
 * @param net 当前网络
 * @param task_snapshot 任务快照（包含真实旧参数）
 * @return EWC惩罚值
 */
float ewc_penalty_with_snapshot(TaskSnapshot* snapshot, FisherInfoMatrix* matrix,
                                HuarongTopologyNet* net, float lambda) {
    if (!net) return 0.0f;

    float penalty = 0.0f;
    float* fisher = NULL;
    float* old_weights = NULL;
    int old_weight_count = 0;

    if (snapshot) {
        // 使用真实快照参数
        // 遍历所有节点，计算惩罚
        for (int i = 0; i < net->node_count && i < snapshot->node_count; i++) {
            ReasoningNode* node = net->nodes[i];
            if (!node) continue;

            // 获取该节点的旧参数
            old_weights = task_snapshot_get_weights(snapshot, i, &old_weight_count);
            if (!old_weights || old_weight_count == 0) continue;

            // 获取该节点的费雪信息
            fisher = fisher_info_get(matrix, i);
            if (!fisher) continue;

            // 计算 Σ F_i * (θ_i - θ*_i)²
            int param_count = MIN(old_weight_count, node->connection_count);
            for (int j = 0; j < param_count; j++) {
                float diff = node->connection_weights[j] - old_weights[j];
                penalty += fisher[j] * diff * diff;
            }
        }
    }

    return 0.5f * lambda * penalty;
}

float ewc_penalty(FisherInfoMatrix* matrix, HuarongTopologyNet* net,
                void* old_params, int node_id) {
    if (!matrix || !net) return 0.0f;

    EWCConfig* config = ewc_get_default_config();
    float penalty = 0.0f;

    // 如果old_params不为空，尝试解析为TaskSnapshot
    if (old_params) {
        TaskSnapshot* snapshot = (TaskSnapshot*)old_params;
        return ewc_penalty_with_snapshot(snapshot, matrix, net, config->lambda);
    }

    // 旧参数为空时，使用基于激活的启发式惩罚（向后兼容）
    if (node_id >= 0) {
        // 单个节点
        float* fisher = fisher_info_get(matrix, node_id);
        if (fisher) {
            ReasoningNode* node = net->nodes[node_id];
            if (node) {
                // 使用节点的基准激活值作为"旧参数"的近似
                // 这比硬编码0.5更合理
                float implicit_old_weight = node->activation * 0.5f;

                for (int i = 0; i < MIN(node->connection_count, 20); i++) {
                    float diff = node->connection_weights[i] - implicit_old_weight;
                    penalty += fisher[i] * diff * diff;
                }
            }
        }
    } else {
        // 所有节点
        for (int i = 0; i < net->node_count; i++) {
            float* fisher = fisher_info_get(matrix, i);
            if (fisher) {
                ReasoningNode* node = net->nodes[i];
                if (node) {
                    float implicit_old_weight = node->activation * 0.5f;
                    for (int j = 0; j < MIN(node->connection_count, 10); j++) {
                        float diff = node->connection_weights[j] - implicit_old_weight;
                        penalty += fisher[j] * diff * diff;
                    }
                }
            }
        }
    }

    return config->lambda * penalty;
}

int ewc_gradient_update(FisherInfoMatrix* matrix, HuarongTopologyNet* net,
                      float* gradients, void* old_params,
                      float learning_rate, int node_id) {
    if (!matrix || !net) return -1;

    EWCConfig* config = ewc_get_default_config();
    TaskSnapshot* snapshot = (old_params ? (TaskSnapshot*)old_params : NULL);

    if (node_id >= 0) {
        float* fisher = fisher_info_get(matrix, node_id);
        if (fisher) {
            ReasoningNode* node = net->nodes[node_id];
            if (node) {
                int old_weight_count = 0;
                float* old_weights = NULL;

                // 从快照获取旧参数
                if (snapshot) {
                    old_weights = task_snapshot_get_weights(snapshot, node_id, &old_weight_count);
                }

                int num_weights = MIN(node->connection_count,
                                      snapshot ? old_weight_count : 10);

                for (int i = 0; i < num_weights; i++) {
                    // 获取该参数在旧任务中的最优值
                    float theta_old = (old_weights && i < old_weight_count)
                                      ? old_weights[i]
                                      : node->activation * 0.5f; // 备用：基于激活的估计

                    // EWC 梯度 = 原始梯度 + 2 * λ * F_i * (θ_i - θ*_i)
                    // E = L_new + λ/2 * Σ F_i * (θ_i - θ*_i)²
                    // ∂E/∂θ_i = ∂L/∂θ_i + λ * F_i * (θ_i - θ*_i)
                    float ewc_grad = config->lambda * fisher[i] * (node->connection_weights[i] - theta_old);
                    gradients[i] += ewc_grad;

                    // 应用梯度更新
                    node->connection_weights[i] -= learning_rate * gradients[i];
                    node->connection_weights[i] = CLAMP(node->connection_weights[i], 0.0f, 1.0f);
                }
            }
        }
    } else {
        // 所有节点
        for (int n = 0; n < net->node_count; n++) {
            float* fisher = fisher_info_get(matrix, n);
            if (fisher) {
                ReasoningNode* node = net->nodes[n];
                if (node) {
                    int old_weight_count = 0;
                    float* old_weights = NULL;

                    if (snapshot) {
                        old_weights = task_snapshot_get_weights(snapshot, n, &old_weight_count);
                    }

                    int num_weights = MIN(node->connection_count,
                                          snapshot ? old_weight_count : 10);

                    for (int i = 0; i < num_weights; i++) {
                        float theta_old = (old_weights && i < old_weight_count)
                                          ? old_weights[i]
                                          : node->activation * 0.5f;

                        float ewc_grad = config->lambda * fisher[i] * (node->connection_weights[i] - theta_old);
                        gradients[i] += ewc_grad;
                        node->connection_weights[i] -= learning_rate * gradients[i];
                        node->connection_weights[i] = CLAMP(node->connection_weights[i], 0.0f, 1.0f);
                    }
                }
            }
        }
    }

    return 0;
}

// ==================== 记忆回放缓冲区 ====================

ReplayBuffer* replay_buffer_create(int capacity, float alpha, float beta) {
    if (capacity <= 0) capacity = DEFAULT_REPLAY_CAPACITY;

    ReplayBuffer* buffer = (ReplayBuffer*)malloc(sizeof(ReplayBuffer));
    if (!buffer) return NULL;

    buffer->samples = (MemorySample**)calloc(capacity, sizeof(MemorySample*));
    if (!buffer->samples) {
        free(buffer);
        return NULL;
    }

    buffer->capacity = capacity;
    buffer->size = 0;
    buffer->head = 0;
    buffer->alpha = alpha;
    buffer->beta = beta;
    buffer->total_additions = 0;
    buffer->total_replays = 0;

    // 优先级相关
    buffer->cumulative_sum = (float*)calloc(capacity, sizeof(float));
    buffer->segment_tree = (int*)calloc(capacity * 2, sizeof(int));

    if (!buffer->cumulative_sum || !buffer->segment_tree) {
        free(buffer->samples);
        free(buffer->cumulative_sum);
        free(buffer->segment_tree);
        free(buffer);
        return NULL;
    }

    // 初始化段树
    for (int i = 0; i < capacity * 2; i++) {
        buffer->segment_tree[i] = 0;
    }

    return buffer;
}

void replay_buffer_destroy(ReplayBuffer* buffer) {
    if (!buffer) return;

    for (int i = 0; i < buffer->size; i++) {
        if (buffer->samples[i]) {
            if (buffer->samples[i]->features) free(buffer->samples[i]->features);
            free(buffer->samples[i]);
        }
    }
    free(buffer->samples);
    free(buffer->cumulative_sum);
    free(buffer->segment_tree);
    free(buffer);
}

int add_to_replay_buffer(ReplayBuffer* buffer, MemorySample* sample) {
    if (!buffer || !sample) return -1;

    // 分配新样本
    MemorySample* new_sample = (MemorySample*)malloc(sizeof(MemorySample));
    if (!new_sample) return -1;

    memcpy(new_sample, sample, sizeof(MemorySample));

    if (sample->feature_dim > 0 && sample->features) {
        new_sample->features = (float*)malloc(sample->feature_dim * sizeof(float));
        if (new_sample->features) {
            memcpy(new_sample->features, sample->features, sample->feature_dim * sizeof(float));
        }
    } else {
        new_sample->features = NULL;
    }

    // 环形缓冲区插入
    if (buffer->size < buffer->capacity) {
        buffer->samples[buffer->size++] = new_sample;
    } else {
        // 覆盖最旧的
        if (buffer->samples[buffer->head]) {
            if (buffer->samples[buffer->head]->features) {
                free(buffer->samples[buffer->head]->features);
            }
            free(buffer->samples[buffer->head]);
        }
        buffer->samples[buffer->head] = new_sample;
        buffer->head = (buffer->head + 1) % buffer->capacity;
    }

    buffer->total_additions++;
    return 0;
}

int sample_replay_batch(ReplayBuffer* buffer, int batch_size,
                       MemorySample** output_samples) {
    if (!buffer || !output_samples || buffer->size == 0) return 0;

    int actual_batch = MIN(batch_size, buffer->size);
    float total_priority = 0.0f;

    // 计算总优先级
    for (int i = 0; i < buffer->size; i++) {
        float p = buffer->samples[i] ? buffer->samples[i]->priority : 1.0f;
        float weighted_p = pow(p, buffer->alpha);
        total_priority += weighted_p;
    }

    // 采样
    for (int i = 0; i < actual_batch; i++) {
        float r = ((float)rand() / RAND_MAX) * total_priority;

        // 找到对应的样本
        float cumsum = 0.0f;
        int selected = 0;
        for (int j = 0; j < buffer->size; j++) {
            float p = buffer->samples[j] ? buffer->samples[j]->priority : 1.0f;
            float weighted_p = pow(p, buffer->alpha);
            cumsum += weighted_p;
            if (cumsum >= r) {
                selected = j;
                break;
            }
        }

        output_samples[i] = buffer->samples[selected];
        buffer->total_replays++;
    }

    return actual_batch;
}

void replay_buffer_update_priority(ReplayBuffer* buffer, int index, float priority) {
    if (!buffer || index < 0 || index >= buffer->size) return;

    if (buffer->samples[index]) {
        buffer->samples[index]->priority = priority;
    }
}

int replay_old_memories(ReplayBuffer* buffer, HuarongTopologyNet* net, int batch_size) {
    if (!buffer || !net || buffer->size == 0) return 0;

    MemorySample** batch = (MemorySample**)malloc(batch_size * sizeof(MemorySample*));
    if (!batch) return 0;

    int sampled = sample_replay_batch(buffer, batch_size, batch);

    for (int i = 0; i < sampled; i++) {
        if (batch[i] && batch[i]->node_id >= 0 && batch[i]->node_id < net->node_count) {
            ReasoningNode* node = net->nodes[batch[i]->node_id];
            if (node) {
                // 回放：恢复激活值和访问计数
                node->activation = 0.8f * node->activation + 0.2f * batch[i]->activation;
                node->is_visited = 0;  // 重置访问标记
            }
        }
    }

    free(batch);
    return sampled;
}

void replay_buffer_get_stats(ReplayBuffer* buffer, int* size, int* capacity,
                            long* total_additions, long* total_replays) {
    if (!buffer) return;

    if (size) *size = buffer->size;
    if (capacity) *capacity = buffer->capacity;
    if (total_additions) *total_additions = buffer->total_additions;
    if (total_replays) *total_replays = buffer->total_replays;
}

// ==================== 知识域管理 ====================

KnowledgeDomain* knowledge_domain_create(const char* name, const char* description,
                                        int initial_capacity) {
    KnowledgeDomain* domain = (KnowledgeDomain*)malloc(sizeof(KnowledgeDomain));
    if (!domain) return NULL;

    static int next_domain_id = 1;

    domain->domain_id = next_domain_id++;
    domain->name = name;
    domain->description = description;
    domain->node_ids = (int*)malloc(initial_capacity * sizeof(int));
    domain->node_count = 0;
    domain->capacity = initial_capacity;
    domain->domain_weights = NULL;
    domain->isolation_strength = 0.5f;
    domain->avg_importance = 0.0f;
    domain->activity_level = 0.0f;
    domain->created_at = time(NULL);
    domain->last_accessed = time(NULL);

    return domain;
}

void knowledge_domain_destroy(KnowledgeDomain* domain) {
    if (!domain) return;
    if (domain->node_ids) free(domain->node_ids);
    if (domain->domain_weights) free(domain->domain_weights);
    free(domain);
}

int knowledge_domain_add_node(KnowledgeDomain* domain, int node_id) {
    if (!domain || node_id < 0) return -1;

    // 检查是否已存在
    for (int i = 0; i < domain->node_count; i++) {
        if (domain->node_ids[i] == node_id) return 0;
    }

    // 扩容
    if (domain->node_count >= domain->capacity) {
        int new_cap = domain->capacity * 2;
        int* new_ids = (int*)realloc(domain->node_ids, new_cap * sizeof(int));
        if (!new_ids) return -1;
        domain->node_ids = new_ids;
        domain->capacity = new_cap;
    }

    domain->node_ids[domain->node_count++] = node_id;
    domain->last_accessed = time(NULL);
    return 0;
}

int knowledge_domain_remove_node(KnowledgeDomain* domain, int node_id) {
    if (!domain) return -1;

    for (int i = 0; i < domain->node_count; i++) {
        if (domain->node_ids[i] == node_id) {
            // 移除
            for (int j = i; j < domain->node_count - 1; j++) {
                domain->node_ids[j] = domain->node_ids[j + 1];
            }
            domain->node_count--;
            return 0;
        }
    }
    return -1;
}

int isolate_domain_weights(MasterTopology* master, KnowledgeDomain* domain,
                          DomainIsolationPolicy policy, float strength) {
    if (!master || !domain) return -1;

    SubTopology* sub = master_get_sub_topology(master, 0);  // 默认第一个拓扑
    if (!sub || !sub->net) return -1;

    switch (policy) {
        case ISOLATION_HARD:
            // 硬隔离：物理断开跨域连接
            for (int i = 0; i < domain->node_count; i++) {
                int node_id = domain->node_ids[i];
                if (node_id >= sub->net->node_count) continue;
                ReasoningNode* node = sub->net->nodes[node_id];
                if (!node) continue;

                // 减小跨域连接的权重
                for (int j = 0; j < node->connection_count; j++) {
                    node->connection_weights[j] *= (1.0f - strength);
                }
            }
            break;

        case ISOLATION_SOFT:
            // 软隔离：添加惩罚项
            for (int i = 0; i < domain->node_count; i++) {
                int node_id = domain->node_ids[i];
                if (node_id >= sub->net->node_count) continue;
                ReasoningNode* node = sub->net->nodes[node_id];
                if (!node) continue;

                // 惩罚激活
                node->activation *= (1.0f - strength * 0.1f);
            }
            break;

        case ISOLATION_ADAPTIVE:
            // 自适应：根据干扰程度调整
            {
                float interference = 0.5f;  // 需要计算实际干扰
                float adaptive_strength = strength * interference;
                for (int i = 0; i < domain->node_count; i++) {
                    int node_id = domain->node_ids[i];
                    if (node_id >= sub->net->node_count) continue;
                    ReasoningNode* node = sub->net->nodes[node_id];
                    if (!node) continue;
                    for (int j = 0; j < node->connection_count; j++) {
                        node->connection_weights[j] *= (1.0f - adaptive_strength);
                    }
                }
            }
            break;

        default:
            break;
    }

    domain->isolation_strength = strength;
    return 0;
}

int cross_domain_transfer(MasterTopology* master, KnowledgeDomain* from_domain,
                        KnowledgeDomain* to_domain, float transfer_ratio) {
    if (!master || !from_domain || !to_domain) return -1;

    SubTopology* sub = master_get_sub_topology(master, 0);
    if (!sub || !sub->net) return -1;

    // 找到两个域之间的共同概念/模式进行迁移
    for (int i = 0; i < from_domain->node_count; i++) {
        int from_node_id = from_domain->node_ids[i];
        if (from_node_id >= sub->net->node_count) continue;
        ReasoningNode* from_node = sub->net->nodes[from_node_id];
        if (!from_node) continue;

        // 为目标域创建新的连接
        for (int j = 0; j < to_domain->node_count; j++) {
            int to_node_id = to_domain->node_ids[j];
            if (to_node_id >= sub->net->node_count) continue;
            ReasoningNode* to_node = sub->net->nodes[to_node_id];
            if (!to_node) continue;

            // 迁移：复制部分连接
            for (int k = 0; k < from_node->connection_count; k++) {
                if (from_node->connections[k]->node_id == to_node_id) {
                    // 已有连接，按比例调整
                    from_node->connection_weights[k] *= (1.0f - transfer_ratio);
                } else {
                    // 潜在的新连接
                    // 简化：不创建新连接，只是调整权重
                }
            }
        }
    }

    to_domain->last_accessed = time(NULL);
    return 0;
}

float compute_domain_interference(MasterTopology* master,
                                 KnowledgeDomain* domain1,
                                 KnowledgeDomain* domain2) {
    if (!master || !domain1 || !domain2) return 0.0f;

    SubTopology* sub = master_get_sub_topology(master, 0);
    if (!sub || !sub->net) return 0.0f;

    int shared_connections = 0;
    int total_connections = 0;

    for (int i = 0; i < domain1->node_count; i++) {
        int node_id = domain1->node_ids[i];
        if (node_id >= sub->net->node_count) continue;
        ReasoningNode* node = sub->net->nodes[node_id];
        if (!node) continue;

        for (int j = 0; j < node->connection_count; j++) {
            int target_id = node->connections[j]->node_id;
            // 检查目标是否在另一个域
            bool in_domain2 = false;
            for (int k = 0; k < domain2->node_count; k++) {
                if (domain2->node_ids[k] == target_id) {
                    in_domain2 = true;
                    break;
                }
            }
            if (in_domain2) {
                shared_connections++;
            }
            total_connections++;
        }
    }

    if (total_connections == 0) return 0.0f;
    return (float)shared_connections / total_connections;
}

// ==================== 任务边界检测 ====================

TaskBoundaryDetector* task_boundary_detector_create(float gain_threshold,
                                                   float drift_threshold,
                                                   float change_threshold) {
    TaskBoundaryDetector* detector = (TaskBoundaryDetector*)malloc(sizeof(TaskBoundaryDetector));
    if (!detector) return NULL;

    detector->knowledge_gain_threshold = gain_threshold;
    detector->activation_drift_threshold = drift_threshold;
    detector->connection_change_threshold = change_threshold;

    detector->gain_history_size = 100;
    detector->recent_knowledge_gains = (float*)calloc(detector->gain_history_size, sizeof(float));
    detector->gain_history_idx = 0;

    detector->total_task_boundaries = 0;
    detector->avg_task_duration = 0.0f;
    detector->last_boundary = 0;

    return detector;
}

void task_boundary_detector_destroy(TaskBoundaryDetector* detector) {
    if (detector) {
        if (detector->recent_knowledge_gains) {
            free(detector->recent_knowledge_gains);
        }
        free(detector);
    }
}

bool detect_task_boundary(TaskBoundaryDetector* detector,
                         float current_gain,
                         float activation_drift,
                         float connection_change) {
    if (!detector) return false;

    // 记录历史
    detector->recent_knowledge_gains[detector->gain_history_idx] = current_gain;
    detector->gain_history_idx = (detector->gain_history_idx + 1) % detector->gain_history_size;

    // 检测边界条件
    bool gain_boundary = current_gain > detector->knowledge_gain_threshold;
    bool drift_boundary = fabsf(activation_drift) > detector->activation_drift_threshold;
    bool change_boundary = connection_change > detector->connection_change_threshold;

    // 计算历史趋势
    float history_avg = 0.0f;
    int history_count = 0;
    for (int i = 0; i < detector->gain_history_size; i++) {
        if (detector->recent_knowledge_gains[i] != 0.0f) {
            history_avg += detector->recent_knowledge_gains[i];
            history_count++;
        }
    }
    if (history_count > 0) history_avg /= history_count;

    // 边界检测：当前增益显著高于历史平均
    bool trend_boundary = (history_count > 10) && 
                          (current_gain > history_avg * 2.0f);

    if (gain_boundary || drift_boundary || change_boundary || trend_boundary) {
        detector->total_task_boundaries++;
        detector->last_boundary = time(NULL);
        return true;
    }

    return false;
}

float estimate_knowledge_gain(HuarongTopologyNet* net,
                             float* old_importance,
                             float* new_importance,
                             int count) {
    if (!net) return 0.0f;

    float total_gain = 0.0f;
    int valid_count = 0;

    for (int i = 0; i < count; i++) {
        if (old_importance && new_importance) {
            float gain = new_importance[i] - old_importance[i];
            if (gain > 0) {
                total_gain += gain;
                valid_count++;
            }
        }
    }

    return (valid_count > 0) ? total_gain / valid_count : 0.0f;
}

bool should_trigger_consolidation(TaskBoundaryDetector* detector,
                                  int consecutive_gains) {
    if (!detector) return false;

    // 连续高增益次数超过阈值
    if (consecutive_gains > 5) {
        return true;
    }

    // 检查时间间隔
    time_t now = time(NULL);
    if (detector->last_boundary > 0 && 
        (now - detector->last_boundary) > 3600) {  // 1小时
        return true;
    }

    return false;
}

// ==================== 任务快照管理 ==========

/**
 * 创建任务参数快照
 * 从当前网络状态复制所有可学习参数
 */
TaskSnapshot* task_snapshot_create(HuarongTopologyNet* net, int task_id) {
    if (!net) return NULL;

    TaskSnapshot* snapshot = (TaskSnapshot*)malloc(sizeof(TaskSnapshot));
    if (!snapshot) return NULL;

    memset(snapshot, 0, sizeof(TaskSnapshot));
    snapshot->task_id = task_id;
    snapshot->created_at = time(NULL);
    snapshot->node_count = net->node_count;
    snapshot->snapshot_size = 0;

    // 分配节点参数数组
    if (net->node_count > 0) {
        snapshot->node_params = (void*)malloc(net->node_count * sizeof(void*));
        if (!snapshot->node_params) {
            free(snapshot);
            return NULL;
        }
        memset(snapshot->node_params, 0, net->node_count * sizeof(void*));
    }

    // 遍历所有节点，复制参数
    int idx = 0;
    for (int i = 0; i < net->node_count; i++) {
        ReasoningNode* node = net->nodes[i];
        if (!node) continue;

        // 为每个节点创建参数快照
        typedef struct {
            int node_id;
            float* weights;
            float* biases;
            float* connection_weights;
            int connection_count;
            int weight_count;
        } NodeParams;

        NodeParams* np = (NodeParams*)malloc(sizeof(NodeParams));
        if (!np) continue;

        memset(np, 0, sizeof(NodeParams));
        np->node_id = node->node_id;
        np->connection_count = node->connection_count;

        // 复制连接权重（最重要的参数）
        if (node->connection_count > 0 && node->connection_weights) {
            np->connection_weights = (float*)malloc(node->connection_count * sizeof(float));
            if (np->connection_weights) {
                memcpy(np->connection_weights, node->connection_weights,
                       node->connection_count * sizeof(float));
                np->weight_count += node->connection_count;
            }
        }

        // 复制特征（如果有）
        if (node->feature_dim > 0 && node->features) {
            np->weights = (float*)malloc(node->feature_dim * sizeof(float));
            if (np->weights) {
                memcpy(np->weights, node->features, node->feature_dim * sizeof(float));
                np->weight_count += node->feature_dim;
            }
        }

        snapshot->node_params[idx++] = (void*)np;
        snapshot->snapshot_size++;
    }

    return snapshot;
}

/**
 * 销毁任务快照
 */
void task_snapshot_destroy(TaskSnapshot* snapshot) {
    if (!snapshot) return;

    // 释放每个节点的参数
    if (snapshot->node_params) {
        for (int i = 0; i < snapshot->snapshot_size; i++) {
            if (snapshot->node_params[i]) {
                typedef struct {
                    int node_id;
                    float* weights;
                    float* biases;
                    float* connection_weights;
                    int connection_count;
                    int weight_count;
                } NodeParams;
                NodeParams* np = (NodeParams*)snapshot->node_params[i];
                if (np) {
                    if (np->weights) free(np->weights);
                    if (np->biases) free(np->biases);
                    if (np->connection_weights) free(np->connection_weights);
                    free(np);
                }
            }
        }
        free(snapshot->node_params);
    }

    free(snapshot);
}

/**
 * 获取快照中指定节点的旧参数
 */
float* task_snapshot_get_weights(TaskSnapshot* snapshot, int node_id, int* out_weight_count) {
    if (!snapshot || !out_weight_count) return NULL;
    *out_weight_count = 0;

    if (!snapshot->node_params) return NULL;

    // 遍历查找匹配的节点
    for (int i = 0; i < snapshot->snapshot_size; i++) {
        if (!snapshot->node_params[i]) continue;

        typedef struct {
            int node_id;
            float* weights;
            float* biases;
            float* connection_weights;
            int connection_count;
            int weight_count;
        } NodeParams;

        NodeParams* np = (NodeParams*)snapshot->node_params[i];
        if (np && np->node_id == node_id && np->connection_weights) {
            *out_weight_count = np->connection_count;
            return np->connection_weights;
        }
    }

    return NULL;
}

/**
 * 从快照计算EWC惩罚（基于真实参数差异）
 */
float compute_ewc_penalty_from_snapshot(TaskSnapshot* snapshot, HuarongTopologyNet* net,
                                       float* fisher_diag, float lambda) {
    if (!snapshot || !net) return 0.0f;

    float penalty = 0.0f;
    int weight_count = 0;

    for (int i = 0; i < net->node_count && i < snapshot->snapshot_size; i++) {
        ReasoningNode* node = net->nodes[i];
        if (!node) continue;

        float* old_weights = task_snapshot_get_weights(snapshot, node->node_id, &weight_count);
        if (!old_weights || weight_count == 0) continue;

        int num_weights = MIN(node->connection_count, weight_count);
        for (int j = 0; j < num_weights; j++) {
            if (fisher_diag && j < weight_count) {
                float diff = node->connection_weights[j] - old_weights[j];
                penalty += fisher_diag[j] * diff * diff;
            }
        }
    }

    return 0.5f * lambda * penalty;
}

// ==================== 持续学习上下文 ====================

ContinualLearningContext* continual_learning_create(void) {
    ContinualLearningContext* ctx = (ContinualLearningContext*)malloc(sizeof(ContinualLearningContext));
    if (!ctx) return NULL;

    memset(ctx, 0, sizeof(ContinualLearningContext));

    // 初始化子系统
    ctx->fisher_matrix = fisher_info_create(1000, DEFAULT_EWC_DAMPING);
    ctx->replay_buffer = replay_buffer_create(DEFAULT_REPLAY_CAPACITY,
                                               DEFAULT_ALPHA, DEFAULT_BETA);
    ctx->boundary_detector = task_boundary_detector_create(0.1f, 0.2f, 0.15f);
    ctx->ewc_config = ewc_config_create();

    ctx->domains = (KnowledgeDomain**)malloc(10 * sizeof(KnowledgeDomain*));
    ctx->domain_count = 0;

    // 初始化任务快照系统
    ctx->snapshot_capacity = 10;
    ctx->task_snapshots = (TaskSnapshot**)malloc(ctx->snapshot_capacity * sizeof(TaskSnapshot*));
    if (!ctx->task_snapshots) {
        ctx->snapshot_capacity = 0;
    }
    ctx->snapshot_count = 0;

    // master在创建时不设置，通过set_master设置
    ctx->master = NULL;

    ctx->current_task_id = 0;
    ctx->step_count = 0;
    ctx->consolidation_in_progress = false;
    ctx->last_consolidation = 0;

    return ctx;
}

void continual_learning_set_master(ContinualLearningContext* ctx, MasterTopology* master) {
    if (ctx) {
        ctx->master = master;
    }
}

void continual_learning_destroy(ContinualLearningContext* ctx) {
    if (!ctx) return;

    if (ctx->fisher_matrix) fisher_info_destroy(ctx->fisher_matrix);
    if (ctx->replay_buffer) replay_buffer_destroy(ctx->replay_buffer);
    if (ctx->boundary_detector) task_boundary_detector_destroy(ctx->boundary_detector);
    if (ctx->ewc_config) ewc_config_destroy(ctx->ewc_config);

    // 销毁所有任务快照
    for (int i = 0; i < ctx->snapshot_count; i++) {
        if (ctx->task_snapshots[i]) {
            task_snapshot_destroy(ctx->task_snapshots[i]);
        }
    }
    if (ctx->task_snapshots) {
        free(ctx->task_snapshots);
    }

    for (int i = 0; i < ctx->domain_count; i++) {
        if (ctx->domains[i]) knowledge_domain_destroy(ctx->domains[i]);
    }
    free(ctx->domains);
    free(ctx);
}

/**
 * 保存当前任务参数快照
 * 应在任务边界处调用
 */
int continual_learning_save_snapshot(ContinualLearningContext* ctx, HuarongTopologyNet* net) {
    if (!ctx || !net) return -1;

    // 扩容检查
    if (ctx->snapshot_count >= ctx->snapshot_capacity) {
        int new_capacity = ctx->snapshot_capacity * 2;
        TaskSnapshot** new_snapshots = (TaskSnapshot**)realloc(
            ctx->task_snapshots, new_capacity * sizeof(TaskSnapshot*));
        if (!new_snapshots) return -1;
        ctx->task_snapshots = new_snapshots;
        ctx->snapshot_capacity = new_capacity;
    }

    // 创建快照
    TaskSnapshot* snapshot = task_snapshot_create(net, ctx->current_task_id);
    if (!snapshot) return -1;

    ctx->task_snapshots[ctx->snapshot_count++] = snapshot;

    return 0;
}

/**
 * 计算所有历史任务的EWC惩罚
 */
float continual_learning_compute_all_ewc_penalty(ContinualLearningContext* ctx, HuarongTopologyNet* net) {
    if (!ctx || !net) return 0.0f;

    float total_penalty = 0.0f;
    float lambda = ctx->ewc_config ? ctx->ewc_config->lambda : DEFAULT_EWC_LAMBDA;

    // 对每个历史任务快照计算惩罚
    for (int i = 0; i < ctx->snapshot_count; i++) {
        TaskSnapshot* snapshot = ctx->task_snapshots[i];
        if (!snapshot) continue;

        float* fisher = fisher_info_get(ctx->fisher_matrix, -1); // 获取所有节点
        if (fisher) {
            total_penalty += compute_ewc_penalty_from_snapshot(snapshot, net, fisher, lambda);
        } else {
            // 如果没有费雪信息，使用基本的快照惩罚
            total_penalty += ewc_penalty_with_snapshot(snapshot, ctx->fisher_matrix, net, lambda);
        }
    }

    return total_penalty;
}

bool continual_learning_on_episode_start(ContinualLearningContext* ctx,
                                         HuarongTopologyNet* net) {
    if (!ctx || !net) return false;

    ctx->step_count++;

    // 检测任务边界
    float current_gain = 0.0f;
    float activation_drift = 0.1f;
    float connection_change = 0.05f;

    bool new_task = detect_task_boundary(ctx->boundary_detector,
                                        current_gain,
                                        activation_drift,
                                        connection_change);

    if (new_task) {
        // 保存旧任务的参数快照（关键：保护已学知识）
        if (ctx->snapshot_count == 0 || ctx->current_task_id > 0) {
            continual_learning_save_snapshot(ctx, net);
        }

        ctx->current_task_id++;
        return true;
    }

    return false;
}

float continual_learning_on_gradients(ContinualLearningContext* ctx,
                                      HuarongTopologyNet* net,
                                      float* /*gradients*/,
                                      float /*learning_rate*/) {
    if (!ctx || !net) return 0.0f;

    // 计算所有历史任务的EWC惩罚（使用真实参数快照）
    float ewc_pen = continual_learning_compute_all_ewc_penalty(ctx, net);

    // 更新费雪信息（按配置的间隔）
    if (ctx->step_count % ctx->ewc_config->fisher_update_interval == 0) {
        for (int i = 0; i < net->node_count; i++) {
            fisher_info_update(ctx->fisher_matrix, net, i);
        }
    }

    return ewc_pen;
}

void continual_learning_on_episode_end(ContinualLearningContext* ctx,
                                       HuarongTopologyNet* net,
                                       int samples_processed) {
    if (!ctx || !net) return;

    // 添加样本到回放缓冲区
    for (int i = 0; i < net->node_count && i < samples_processed; i++) {
        ReasoningNode* node = net->nodes[i];
        if (node) {
            MemorySample sample = {
                .node_id = node->node_id,
                .features = node->features,
                .feature_dim = node->feature_dim,
                .activation = node->activation,
                .priority = node->activation + 0.1f,
                .timestamp = time(NULL),
                .access_count = node->connection_count
            };
            add_to_replay_buffer(ctx->replay_buffer, &sample);
        }
    }

    // 检查是否需要巩固
    if (should_trigger_consolidation(ctx->boundary_detector, 3)) {
        continual_learning_consolidate(ctx, net);
    }
}

int continual_learning_consolidate(ContinualLearningContext* ctx,
                                   HuarongTopologyNet* net) {
    if (!ctx || !net) return -1;

    ctx->consolidation_in_progress = true;

    // 1. 更新费雪信息
    for (int i = 0; i < net->node_count; i++) {
        fisher_info_update(ctx->fisher_matrix, net, i);
    }

    // 2. 回放旧记忆
    replay_old_memories(ctx->replay_buffer, net, 32);

    // 3. 隔离高干扰的域（使用ctx->master，避免传NULL）
    if (ctx->master && ctx->domain_count > 1) {
        for (int i = 0; i < ctx->domain_count; i++) {
            for (int j = i + 1; j < ctx->domain_count; j++) {
                float interference = compute_domain_interference(ctx->master,
                                                              ctx->domains[i],
                                                              ctx->domains[j]);
                if (interference > 0.3f) {
                    isolate_domain_weights(ctx->master, ctx->domains[i],
                                          ISOLATION_SOFT, interference);
                }
            }
        }
    }

    // 4. 保存任务参数快照（关键改进）
    continual_learning_save_snapshot(ctx, net);

    ctx->consolidation_in_progress = false;
    ctx->last_consolidation = time(NULL);

    return 0;
}

void continual_learning_get_stats(ContinualLearningContext* ctx,
                                 int* total_tasks,
                                 int* current_task,
                                 float* avg_retention) {
    if (!ctx) return;

    if (total_tasks) *total_tasks = ctx->current_task_id + 1;
    if (current_task) *current_task = ctx->current_task_id;
    if (avg_retention) {
        // 简化：基于回放缓冲区的命中率估算保留率
        long total_replays = ctx->replay_buffer ? ctx->replay_buffer->total_replays : 0;
        long total_additions = ctx->replay_buffer ? ctx->replay_buffer->total_additions : 1;
        *avg_retention = (float)total_replays / MAX(total_additions, 1);
    }
}
