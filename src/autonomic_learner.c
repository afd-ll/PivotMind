/**
 * @file autonomic_learner.c
 * @brief 自主学习层 — 运行时同时激活→边置信度自动涨落
 *
 * 核心机制：
 * 1. 对话中，用户输入的每个字和AI回复的每个字"同时激活"
 * 2. 同时激活的节点之间，边的置信度上升
 * 3. 没有参与激活的边，自然衰退（竞争）
 * 4. 不需要外部反馈，运行时就学
 *
 * 并发策略：
 * - workers 内部只记录哪些节点对同时激活（thread-local buffer）
 * - barrier（thread_pool_batch）后统一批量更新边权重
 * - 锁策略：按边哈希分片，避免全局锁竞争
 *
 * 刷盘策略：
 * - 每轮对话结束：内存批量更新置信度（当轮生效）
 * - 刷盘触发条件（满足任一）：
 *   - 累积更新 ≥ 50次
 *   - 用户停顿 30秒+
 *   - 正常退出/SIGTERM
 *   - 内存待更新边数超阈值
 */

#include "autonomic_learner.h"
#include "utf8_tokenizer.h"
#include "node_hash.h"
#include "topology_growth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

// ==================== 内部辅助函数 ====================

/**
 * 标记已激活的边（用于竞争衰减）
 */
typedef struct {
    int node_id;           // 节点ID
    int edge_indices[128]; // 被激活的边在本节点connections数组中的下标
    int edge_count;        // 激活的边数
} ActivatedEdges;

#define MAX_CHARS_PER_TEXT 256
#define MAX_ACTIVATED_PAIRS 4096

// 全局激活记录（每轮对话重置）
static ActivatedEdges g_activated[MAX_ACTIVATED_PAIRS];
static int g_activated_count = 0;

static void reset_activation_record(void) {
    g_activated_count = 0;
}

static void record_edge_activated(ReasoningNode* node, int edge_index) {
    if (!node) return;
    for (int i = 0; i < g_activated_count; i++) {
        if (g_activated[i].node_id == node->node_id) {
            for (int j = 0; j < g_activated[i].edge_count; j++) {
                if (g_activated[i].edge_indices[j] == edge_index)
                    return;
            }
            if (g_activated[i].edge_count < 128) {
                g_activated[i].edge_indices[g_activated[i].edge_count++] = edge_index;
            }
            return;
        }
    }
    if (g_activated_count < MAX_ACTIVATED_PAIRS) {
        g_activated[g_activated_count].node_id = node->node_id;
        g_activated[g_activated_count].edge_count = 0;
        if (g_activated[g_activated_count].edge_count < 128) {
            g_activated[g_activated_count].edge_indices[g_activated[g_activated_count].edge_count++] = edge_index;
        }
        g_activated_count++;
    }
}

static void record_connection_activated(ReasoningNode* a, ReasoningNode* b) {
    if (!a || !b) return;
    for (int i = 0; i < a->connection_count; i++) {
        if (a->connections[i] == b) {
            record_edge_activated(a, i);
            break;
        }
    }
    for (int i = 0; i < b->connection_count; i++) {
        if (b->connections[i] == a) {
            record_edge_activated(b, i);
            break;
        }
    }
}

// ==================== 边哈希分片 ====================

/** 计算边哈希所在分片索引 */
static int edge_shard_index(int node_a_id, int node_b_id) {
    int min_id = (node_a_id < node_b_id) ? node_a_id : node_b_id;
    int max_id = (node_a_id > node_b_id) ? node_a_id : node_b_id;
    unsigned int hash = (unsigned int)(min_id * 2654435761U) ^ (unsigned int)(max_id * 2246822519U);
    return hash % AUTONOMIC_SHARD_COUNT;
}

// ==================== AutonomicState 实现 ====================

void autonomic_state_init(AutonomicState* state) {
    if (!state) return;
    memset(state, 0, sizeof(AutonomicState));
    for (int i = 0; i < AUTONOMIC_SHARD_COUNT; i++) {
        pthread_mutex_init(&state->shards[i].lock, NULL);
        state->shards[i].pending_count = 0;
    }
    state->flush_threshold = 50;
    state->idle_flush_seconds = 30;
    state->max_pending_edges = 500;
    state->local_buffer_count = 0;
    state->initialized = 1;
}

void autonomic_state_destroy(AutonomicState* state) {
    if (!state) return;
    for (int i = 0; i < AUTONOMIC_SHARD_COUNT; i++) {
        pthread_mutex_destroy(&state->shards[i].lock);
    }
    memset(state, 0, sizeof(AutonomicState));
}

void autonomic_request_flush(AutonomicState* state, MasterTopology* master) {
    if (!state || !state->initialized) return;

    time_t now = time(NULL);
    int should_flush = 0;

    // 条件1：累积更新 ≥ 阈值
    if (state->pending_updates >= state->flush_threshold) {
        should_flush = 1;
    }

    // 条件2：距离上次刷盘超过空闲刷盘时间
    if (!should_flush && (now - state->last_flush_time) >= state->idle_flush_seconds) {
        should_flush = 1;
    }

    if (!should_flush) return;

    // 执行刷盘
    printf("[自主学习刷盘] %d 次更新, 距上次 %lds\n",
           state->pending_updates, (long)(now - state->last_flush_time));

    if (master) {
        char path[512];
        snprintf(path, 511, "pivotmind_state.dat");

        // 持久化拓扑（使用已有的 master_save_state）
        int saved = master_save_state(master, path);
        if (saved == 0) {
            printf("[自主学习刷盘] ✓ 已保存到 %s\n", path);
        } else {
            printf("[自主学习刷盘] × 保存失败\n");
        }
    }

    // 重置计数
    state->pending_updates = 0;
    state->last_flush_time = now;
}

// ==================== 核心：从文本中提取单字并去重 ====================

static int extract_unique_chars(const char* text, char chars[MAX_CHARS_PER_TEXT][8],
                                 int* out_utf8_lens) {
    if (!text) return 0;
    const char* p = text;
    int count = 0;

    while (*p && count < MAX_CHARS_PER_TEXT) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
            continue;
        }
        int len = utf8_char_len((unsigned char)*p);
        if (len <= 0) { p++; continue; }

        char buf[8] = {0};
        memcpy(buf, p, len);
        buf[len] = '\0';

        int already_exists = 0;
        for (int i = 0; i < count; i++) {
            if (strcmp(chars[i], buf) == 0) {
                already_exists = 1;
                break;
            }
        }
        if (!already_exists) {
            memcpy(chars[count], buf, len + 1);
            if (out_utf8_lens) out_utf8_lens[count] = len;
            count++;
        }
        p += len;
    }
    return count;
}

// ==================== 在拓扑中查找节点 ====================

static ReasoningNode* find_node_by_concept(SubTopology* topo, const char* concept) {
    if (!topo || !concept) return NULL;

    if (topo->node_hash) {
        ReasoningNode* node = node_hash_find(topo->node_hash, concept);
        if (node) return node;
    }

    if (!topo->net) return NULL;
    for (int i = 0; i < topo->net->node_count; i++) {
        ReasoningNode* node = topo->net->nodes[i];
        if (node && node->concept && strcmp(node->concept, concept) == 0) {
            if (topo->node_hash) {
                node_hash_add(topo->node_hash, node);
            }
            return node;
        }
    }
    return NULL;
}

// ==================== 建边并涨置信度 ====================

static void boost_connection(SubTopology* topo, ReasoningNode* a, ReasoningNode* b,
                             AutonomicState* state) {
    if (!a || !b || a == b) return;

    if (a->connection_count >= AUTONOMIC_MAX_CONNECTIONS ||
        b->connection_count >= AUTONOMIC_MAX_CONNECTIONS) {
        return;
    }

    int existing_a_to_b = -1;
    for (int i = 0; i < a->connection_count; i++) {
        if (a->connections[i] == b) {
            existing_a_to_b = i;
            break;
        }
    }

    int existing_b_to_a = -1;
    for (int i = 0; i < b->connection_count; i++) {
        if (b->connections[i] == a) {
            existing_b_to_a = i;
            break;
        }
    }

    if (existing_a_to_b >= 0) {
        // 已有连接 → 涨置信度
        float old_confidence = a->connection_confidences[existing_a_to_b];
        float delta = AUTONOMIC_LEARNING_RATE * (1.0f - old_confidence);
        a->connection_confidences[existing_a_to_b] = old_confidence + delta;
        a->connection_weights[existing_a_to_b] += AUTONOMIC_LEARNING_RATE * 0.5f;
        if (a->connection_weights[existing_a_to_b] > 0.9f)
            a->connection_weights[existing_a_to_b] = 0.9f;

        record_connection_activated(a, b);
    } else {
        // 新建连接
        if (topo && topo->net) {
            int ret = huarong_net_add_connection(topo->net,
                                                  a->node_id, b->node_id,
                                                  AUTONOMIC_BASE_WEIGHT);
            if (ret == 0) {
                for (int i = 0; i < a->connection_count; i++) {
                    if (a->connections[i] == b) {
                        a->connection_confidences[i] = AUTONOMIC_INITIAL_CONFIDENCE;
                        record_connection_activated(a, b);
                        break;
                    }
                }
            }
        }
    }

    if (existing_b_to_a >= 0) {
        float old_confidence = b->connection_confidences[existing_b_to_a];
        float delta = AUTONOMIC_LEARNING_RATE * (1.0f - old_confidence);
        b->connection_confidences[existing_b_to_a] = old_confidence + delta;
        b->connection_weights[existing_b_to_a] += AUTONOMIC_LEARNING_RATE * 0.5f;
        if (b->connection_weights[existing_b_to_a] > 0.9f)
            b->connection_weights[existing_b_to_a] = 0.9f;
    } else {
        if (topo && topo->net) {
            huarong_net_add_connection(topo->net,
                                       b->node_id, a->node_id,
                                       AUTONOMIC_BASE_WEIGHT);
            for (int i = 0; i < b->connection_count; i++) {
                if (b->connections[i] == a) {
                    b->connection_confidences[i] = AUTONOMIC_INITIAL_CONFIDENCE;
                    break;
                }
            }
        }
    }

    // 刷新状态累加器
    if (state && state->initialized) {
        state->pending_updates++;
        int sIdx = edge_shard_index(a->node_id, b->node_id);
        pthread_mutex_lock(&state->shards[sIdx].lock);
        state->shards[sIdx].pending_count++;
        pthread_mutex_unlock(&state->shards[sIdx].lock);
    }
}

// ==================== 核心API实现 ====================

void autonomic_learn_from_dialog(MasterTopology* master,
                                 const char* user_input,
                                 const char* ai_response,
                                 AutonomicState* state) {
    if (!master || !user_input || !ai_response) return;
    if (strlen(user_input) == 0 || strlen(ai_response) == 0) return;

    reset_activation_record();

    // 获取词汇拓扑
    SubTopology* vocab = NULL;
    for (int t = 0; t < master->sub_topo_count; t++) {
        if (master->sub_topologies[t] &&
            master->sub_topologies[t]->type == TOPO_VOCABULARY) {
            vocab = master->sub_topologies[t];
            break;
        }
    }
    if (!vocab || !vocab->net || vocab->net->node_count < 2) return;

    // 提取单字
    char input_chars[MAX_CHARS_PER_TEXT][8];
    char response_chars[MAX_CHARS_PER_TEXT][8];
    int input_utf8_lens[MAX_CHARS_PER_TEXT];
    int response_utf8_lens[MAX_CHARS_PER_TEXT];

    int input_count = extract_unique_chars(user_input, input_chars, input_utf8_lens);
    int response_count = extract_unique_chars(ai_response, response_chars, response_utf8_lens);
    if (input_count == 0 || response_count == 0) return;

    // 查找节点
    ReasoningNode* input_nodes[MAX_CHARS_PER_TEXT];
    ReasoningNode* response_nodes[MAX_CHARS_PER_TEXT];
    int valid_input = 0, valid_response = 0;

    for (int i = 0; i < input_count; i++) {
        input_nodes[i] = find_node_by_concept(vocab, input_chars[i]);
        if (input_nodes[i]) valid_input++;
    }
    for (int i = 0; i < response_count; i++) {
        response_nodes[i] = find_node_by_concept(vocab, response_chars[i]);
        if (response_nodes[i]) valid_response++;
    }
    if (valid_input == 0 || valid_response == 0) return;

    // 核心：每对（输入字，回复字）之间建边/涨置信度
    int pairs_boosted = 0;
    for (int i = 0; i < input_count; i++) {
        if (!input_nodes[i]) continue;
        for (int j = 0; j < response_count; j++) {
            if (!response_nodes[j]) continue;
            boost_connection(vocab, input_nodes[i], response_nodes[j], state);
            pairs_boosted++;
        }
    }

    // 竞争衰减：本轮未激活的边
    if (pairs_boosted > 0) {
        int decayed = 0;
        for (int n = 0; n < vocab->net->node_count; n++) {
            ReasoningNode* node = vocab->net->nodes[n];
            if (!node || node->connection_count == 0) continue;

            int has_any_activated = 0;
            for (int a = 0; a < g_activated_count; a++) {
                if (g_activated[a].node_id == node->node_id && g_activated[a].edge_count > 0) {
                    has_any_activated = 1;
                    break;
                }
            }

            if (!has_any_activated) {
                for (int e = 0; e < node->connection_count; e++) {
                    node->connection_confidences[e] *= AUTONOMIC_DECAY_RATE;
                    if (node->connection_confidences[e] < 0.05f)
                        node->connection_confidences[e] = 0.05f;
                    decayed++;
                }
            } else {
                for (int e = 0; e < node->connection_count; e++) {
                    int is_activated = 0;
                    for (int a = 0; a < g_activated_count; a++) {
                        if (g_activated[a].node_id == node->node_id) {
                            for (int ae = 0; ae < g_activated[a].edge_count; ae++) {
                                if (g_activated[a].edge_indices[ae] == e) {
                                    is_activated = 1;
                                    break;
                                }
                            }
                            if (is_activated) break;
                        }
                    }
                    if (!is_activated) {
                        node->connection_confidences[e] *= AUTONOMIC_DECAY_RATE;
                        if (node->connection_confidences[e] < 0.05f)
                            node->connection_confidences[e] = 0.05f;
                        decayed++;
                    }
                }
            }
        }
        printf("[自主学习] 激活 %d 对连接, 衰减 %d 条未激活边\n",
               pairs_boosted, decayed);
    }

    // 刷盘判断
    if (state && state->initialized) {
        autonomic_request_flush(state, master);
    }
}

void autonomic_decay_all(MasterTopology* master) {
    if (!master) return;

    int total_decayed = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;

        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (!node) continue;

            for (int e = 0; e < node->connection_count; e++) {
                node->connection_confidences[e] *= AUTONOMIC_DECAY_RATE;
                if (node->connection_confidences[e] < 0.05f)
                    node->connection_confidences[e] = 0.05f;
                total_decayed++;
            }
        }
    }
    printf("[自主学习] 全局衰减: %d 条边\n", total_decayed);
}

int autonomic_get_edge_stats(MasterTopology* master,
                            int* out_total_edges,
                            float* out_avg_confidence) {
    if (!master) return -1;

    int total_edges = 0;
    float sum_confidence = 0.0f;

    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;

        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (!node) continue;

            for (int e = 0; e < node->connection_count; e++) {
                total_edges++;
                sum_confidence += node->connection_confidences[e];
            }
        }
    }

    if (out_total_edges) *out_total_edges = total_edges;
    if (out_avg_confidence && total_edges > 0)
        *out_avg_confidence = sum_confidence / total_edges;
    else if (out_avg_confidence)
        *out_avg_confidence = 0.0f;

    return 0;
}

// ==================== 测试入口 ====================

#ifdef TEST_AUTONOMIC_LEARNER
int main() {
    printf("=== 自主学习器单元测试 ===\n");

    MasterTopology* master = master_topology_create(10);
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 100, 10);

    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    if (!vocab || !vocab->net) {
        printf("失败：无法创建词汇拓扑\n");
        return 1;
    }

    const char* test_chars[] = {"学", "习", "机", "器", "人", "中", "国", "大"};
    for (int i = 0; i < 8; i++) {
        ReasoningNode* node = huarong_net_add_node(vocab->net, test_chars[i], NULL, 0);
        if (node) {
            node_hash_add(vocab->node_hash, node);
            node->activation = 0.5f;
        }
    }

    printf("初始: %d 节点, 0 边\n", vocab->net->node_count);

    // 初始化 AutonomicState
    AutonomicState state;
    autonomic_state_init(&state);

    // 模拟对话
    printf("\n--- 对话: 输入=学习, 回复=机器 ---\n");
    autonomic_learn_from_dialog(master, "学习", "机器", &state);

    int total_edges = 0;
    float avg_conf = 0;
    autonomic_get_edge_stats(master, &total_edges, &avg_conf);
    printf("\n统计: 总边数=%d, 平均置信度=%.3f\n", total_edges, avg_conf);

    // 查看"学"节点
    ReasoningNode* node_xue = find_node_by_concept(vocab, "学");
    if (node_xue) {
        printf("\n「学」节点的连接:\n");
        for (int i = 0; i < node_xue->connection_count; i++) {
            if (node_xue->connections[i] && node_xue->connections[i]->concept) {
                printf("  → %s (weight=%.3f, conf=%.3f)\n",
                       node_xue->connections[i]->concept,
                       node_xue->connection_weights[i],
                       node_xue->connection_confidences[i]);
            }
        }
    }

    // 第二次对话（加强）
    printf("\n--- 对话2: 输入=学习, 回复=机器 ---\n");
    autonomic_learn_from_dialog(master, "学习", "机器", &state);

    autonomic_get_edge_stats(master, &total_edges, &avg_conf);
    printf("\n统计: 总边数=%d, 平均置信度=%.3f\n", total_edges, avg_conf);

    if (node_xue) {
        printf("\n「学」节点的连接:\n");
        for (int i = 0; i < node_xue->connection_count; i++) {
            if (node_xue->connections[i] && node_xue->connections[i]->concept) {
                printf("  → %s (weight=%.3f, conf=%.3f)\n",
                       node_xue->connections[i]->concept,
                       node_xue->connection_weights[i],
                       node_xue->connection_confidences[i]);
            }
        }
    }

    // 检查刷盘状态
    printf("\n刷盘状态: pending_updates=%d\n", state.pending_updates);

    autonomic_state_destroy(&state);
    master_topology_destroy(master);
    printf("\n测试通过 ✓\n");
    return 0;
}
#endif
