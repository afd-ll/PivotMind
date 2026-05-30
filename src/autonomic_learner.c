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
#include "common.h"
#include "utf8_tokenizer.h"
#include "node_hash.h"
#include "cross_edge_io.h"
#include "causal_reasoning.h"
#include "memory_system.h"
#include "feature_learn.h"
#include "topology_growth.h"
#include "feature_io.h"
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
    int edge_indices[PM_EDGE_TRACK]; // 被激活的边在本节点connections数组中的下标
    int edge_count;        // 激活的边数
} ActivatedEdges;

#define MAX_CHARS_PER_TEXT PM_CHARS_PER_TEXT
#define MAX_ACTIVATED_PAIRS PM_ACTIVATED_PAIRS

// 全局激活记录（每轮对话重置）—— __thread 支持可重入
static __thread ActivatedEdges g_activated[MAX_ACTIVATED_PAIRS];
static __thread int g_activated_count;

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
            if (g_activated[i].edge_count < PM_EDGE_TRACK) {
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
    pthread_mutex_init(&state->flush_lock, NULL);
    state->flush_threshold = PM_AUTONOMIC_FLUSH_THRESHOLD;
    state->idle_flush_seconds = PM_AUTONOMIC_IDLE_FLUSH_SECS;
    state->max_pending_edges = 500;
    state->local_buffer_count = 0;
    state->initialized = 1;
}

void autonomic_state_destroy(AutonomicState* state) {
    if (!state) return;
    for (int i = 0; i < AUTONOMIC_SHARD_COUNT; i++) {
        pthread_mutex_destroy(&state->shards[i].lock);
    }
    pthread_mutex_destroy(&state->flush_lock);
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

    // 拿锁后二次检查（double-check）：别的线程可能刚刷完盘
    pthread_mutex_lock(&state->flush_lock);

    if (state->pending_updates < state->flush_threshold &&
        (now - state->last_flush_time) < state->idle_flush_seconds) {
        pthread_mutex_unlock(&state->flush_lock);
        return;
    }

    // 执行刷盘 — 先备份再覆写（锁内，单线程安全）
    printf("[自主学习刷盘] %d 次更新, 距上次 %lds\n",
           state->pending_updates, (long)(now - state->last_flush_time));

    if (master) {
        // 刷盘前先剪枝低质量边（控制拓扑膨胀）
        for (int t = 0; t < master->sub_topo_count; t++) {
            SubTopology* sub = master->sub_topologies[t];
            if (sub && sub->net && sub->net->node_count > 0) {
                huarong_net_prune_edges(sub->net, 0.05f, 0.02f);
            }
        }
        master_prune_cross_links(master, 0.05f, 2);

        // 特征学习：用图平滑更新 features（从随机→有意义）
        for (int t = 0; t < master->sub_topo_count; t++) {
            SubTopology* sub = master->sub_topologies[t];
            if (sub && sub->net && sub->net->node_count > 0) {
                feature_learn_graph_smooth(sub->net, 3);
            }
        }

        // 特征吸引: 高置信度边拉近特征向量 (建立边→特征的闭环)
        {
            int attract_count = 0;
            for (int t = 0; t < master->sub_topo_count; t++) {
                SubTopology* sub = master->sub_topologies[t];
                if (!sub || !sub->net) continue;
                for (int i = 0; i < sub->net->node_count; i++) {
                    ReasoningNode* node = sub->net->nodes[i];
                    if (!node || !node->features) continue;
                    for (int c = 0; c < node->connection_count; c++) {
                        if (node->connection_confidences[c] < 0.6f) continue;
                        ReasoningNode* nb = node->connections[c];
                        if (!nb || !nb->features || nb->feature_dim != node->feature_dim) continue;
                        hebbian_update(node->features, nb->features, node->feature_dim, 0.01f);
                        attract_count++;
                    }
                }
            }
            if (attract_count > 0) {
                printf("[自主学习刷盘] 特征吸引: %d 对\n", attract_count);
            }
        }

        char path[512];
        snprintf(path, 511, "pivotmind_state.dat");

        // 备份：如果已有状态文件，先重命名，防止刷盘过程中崩了丢数据
        FILE* existing = fopen(path, "rb");
        if (existing) {
            fclose(existing);
            char bak_path[520];
            snprintf(bak_path, 519, "%s.bak", path);
            remove(bak_path);  // 删旧的备份
            rename(path, bak_path);
        }

        // 持久化拓扑（使用已有的 master_save_state）
        int saved = master_save_state(master, path);
        if (saved > 0) {
            printf("[自主学习刷盘] ✓ 已保存到 %s (%d 节点)\n", path, saved);
        } else {
            printf("[自主学习刷盘] × 保存失败\n");
        }

        // 同时保存特征向量, 确保与拓扑状态同步
        int feat_saved = save_features(master, "features.bin");
        if (feat_saved > 0) {
            printf("[自主学习刷盘] ✓ 已保存特征向量 (%d 节点)\n", feat_saved);
        }
    }

    // 重置计数（锁内，防止其他线程看到过时的 pending_updates）
    state->pending_updates = 0;
    state->last_flush_time = now;

    pthread_mutex_unlock(&state->flush_lock);
}

// ==================== 核心：从文本中提取单字（保留完整序列，不去重） ====================
// 保留字序信息：同一字多次出现（如"我我我"）全部保留
// 拓扑层通过 huarong_net_find_or_create_node 复用已有节点，不会重复创建

static int extract_ordered_chars(const char* text, char chars[MAX_CHARS_PER_TEXT][8],
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

        memcpy(chars[count], p, len);
        chars[count][len] = '\0';
        if (out_utf8_lens) out_utf8_lens[count] = len;
        count++;
        p += len;
    }
    return count;
}


// ==================== 在拓扑中查找节点 ====================

// ==================== 建边并涨置信度 ====================

static void boost_connection_weighted(SubTopology* topo, ReasoningNode* a, ReasoningNode* b,
                                      AutonomicState* state, float weight_mult) {
    if (!a || !b || a == b) return;

    HuarongTopologyNet* net = (topo && topo->net) ? topo->net : NULL;

    if (a->connection_count >= AUTONOMIC_MAX_CONNECTIONS ||
        b->connection_count >= AUTONOMIC_MAX_CONNECTIONS) return;

    float base_weight = AUTONOMIC_BASE_WEIGHT * weight_mult;
    // 回路6: 被走边频繁选中的节点对，赫布学习时给更大boost
    float sel_boost = 1.0f + 0.1f * ((a->selection_count + b->selection_count) * 0.5f) / 10.0f;
    if (sel_boost > 1.5f) sel_boost = 1.5f;
    base_weight *= sel_boost;
    if (base_weight > 0.9f) base_weight = 0.9f;

    int existing_a_to_b = -1;
    int existing_b_to_a = -1;
    if (net) pthread_mutex_lock(&net->mutex);
    for (int i = 0; i < a->connection_count; i++) {
        if (a->connections[i] == b) { existing_a_to_b = i; break; }
    }
    if (existing_a_to_b >= 0) {
        float dc = AUTONOMIC_LEARNING_RATE * (1.0f - a->connection_confidences[existing_a_to_b]);
        float dw = AUTONOMIC_LEARNING_RATE * 0.5f * weight_mult;
        a->connection_confidences[existing_a_to_b] += dc;
        a->connection_weights[existing_a_to_b] += dw;
        if (a->connection_weights[existing_a_to_b] > 0.9f) a->connection_weights[existing_a_to_b] = 0.9f;
    }
    for (int i = 0; i < b->connection_count; i++) {
        if (b->connections[i] == a) { existing_b_to_a = i; break; }
    }
    if (existing_b_to_a >= 0) {
        float dc = AUTONOMIC_LEARNING_RATE * (1.0f - b->connection_confidences[existing_b_to_a]);
        float dw = AUTONOMIC_LEARNING_RATE * 0.5f * weight_mult;
        b->connection_confidences[existing_b_to_a] += dc;
        b->connection_weights[existing_b_to_a] += dw;
        if (b->connection_weights[existing_b_to_a] > 0.9f) b->connection_weights[existing_b_to_a] = 0.9f;
    }
    if (net) pthread_mutex_unlock(&net->mutex);
    if (existing_a_to_b >= 0) record_connection_activated(a, b);

    // ── 新建边：不持锁调 add_connection（内部锁扩容）──
    if (existing_a_to_b < 0 && net) {
        int ret = huarong_net_add_connection(net, a->node_id, b->node_id, base_weight);
        if (ret == 0) {
            for (int i = 0; i < a->connection_count; i++) {
                if (a->connections[i] == b) {
                    a->connection_confidences[i] = AUTONOMIC_INITIAL_CONFIDENCE;
                    record_connection_activated(a, b);
                    if (a->features && b->features && a->feature_dim == b->feature_dim)
                        hebbian_update(a->features, b->features, a->feature_dim, 0.02f);
                    break;
                }
            }
        }
    }
    if (existing_b_to_a < 0 && net) {
        huarong_net_add_connection(net, b->node_id, a->node_id, base_weight);
        for (int i = 0; i < b->connection_count; i++) {
            if (b->connections[i] == a) {
                b->connection_confidences[i] = AUTONOMIC_INITIAL_CONFIDENCE;
                break;
            }
        }
    }


    // 刷新状态累加器
    if (state && state->initialized) {
        #pragma omp atomic
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
                                 AutonomicState* state,
                                 void* causal_graph,
                                 MemorySystem* memory) {
    if (!master || !user_input || !ai_response) return;
    if (strlen(user_input) == 0 || strlen(ai_response) == 0) return;

    reset_activation_record();

    // 一次性扫描缓存四种拓扑指针（O(n) → 1 次遍历）
    SubTopology* vocab = NULL;
    SubTopology* semantic = NULL;
    SubTopology* concept_t = NULL;
    SubTopology* emotion = NULL;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub) continue;
        switch (sub->type) {
            case TOPO_VOCABULARY: vocab = sub; break;
            case TOPO_SEMANTIC:   semantic = sub; break;
            case TOPO_CONCEPT:    concept_t = sub; break;
            case TOPO_EMOTION:    emotion = sub; break;
            default: break;
        }
    }
    if (!vocab || !vocab->net) return;

    // 提取单字
    char input_chars[MAX_CHARS_PER_TEXT][8];
    char response_chars[MAX_CHARS_PER_TEXT][8];
    int input_utf8_lens[MAX_CHARS_PER_TEXT];
    int response_utf8_lens[MAX_CHARS_PER_TEXT];

    int input_count = extract_ordered_chars(user_input, input_chars, input_utf8_lens);
    int response_count = extract_ordered_chars(ai_response, response_chars, response_utf8_lens);
    if (input_count == 0 || response_count == 0) return;

    // 查找或创建节点 — 不在拓扑中的汉字自动创建
    ReasoningNode* input_nodes[MAX_CHARS_PER_TEXT];
    ReasoningNode* response_nodes[MAX_CHARS_PER_TEXT];

    for (int i = 0; i < input_count; i++) {
        input_nodes[i] = huarong_net_find_or_create_node(vocab->net, input_chars[i], NULL, 0, vocab->node_hash);
    }
    for (int i = 0; i < response_count; i++) {
        response_nodes[i] = huarong_net_find_or_create_node(vocab->net, response_chars[i], NULL, 0, vocab->node_hash);
    }

    // 核心1：输入字内部的共现边（相邻字权重更高，编码字序）
    // 先收集有效且不重复的节点索引（减少 O(n²) 循环规模）
    int valid_in_idx[MAX_CHARS_PER_TEXT];
    int valid_in_count = 0;
    for (int i = 0; i < input_count; i++) {
        if (!input_nodes[i]) continue;
        int dup = 0;
        for (int k = 0; k < valid_in_count; k++) {
            if (input_nodes[valid_in_idx[k]] == input_nodes[i]) { dup = 1; break; }
        }
        if (!dup) valid_in_idx[valid_in_count++] = i;
    }
    for (int vi = 0; vi < valid_in_count; vi++) {
        int i = valid_in_idx[vi];
        for (int vj = vi + 1; vj < valid_in_count; vj++) {
            int j = valid_in_idx[vj];
            int dist = j - i;
            float wmult = (dist == 1) ? 1.5f : (1.5f / dist);
            if (wmult < 0.3f) wmult = 0.3f;
            boost_connection_weighted(vocab, input_nodes[i], input_nodes[j], state, wmult);
        }
    }

    // 核心2：回复字内部的共现边（同样编码字序）
    // 先收集有效且不重复的节点索引
    int valid_res_idx[MAX_CHARS_PER_TEXT];
    int valid_res_count = 0;
    for (int i = 0; i < response_count; i++) {
        if (!response_nodes[i]) continue;
        int dup = 0;
        for (int k = 0; k < valid_res_count; k++) {
            if (response_nodes[valid_res_idx[k]] == response_nodes[i]) { dup = 1; break; }
        }
        if (!dup) valid_res_idx[valid_res_count++] = i;
    }
    for (int vi = 0; vi < valid_res_count; vi++) {
        int i = valid_res_idx[vi];
        for (int vj = vi + 1; vj < valid_res_count; vj++) {
            int j = valid_res_idx[vj];
            int dist = j - i;
            float wmult = (dist == 1) ? 1.5f : (1.5f / dist);
            if (wmult < 0.3f) wmult = 0.3f;
            boost_connection_weighted(vocab, response_nodes[i], response_nodes[j], state, wmult);
        }
    }

    // 核心3 + 回路7+9: 输入↔回复交叉边（含因果图+LTM boost）
    // 先计算每对的因果/LTM增强因子，一次性调用 boost_connection_weighted 避免双重计数
    {
        CausalGraph* cg = (CausalGraph*)causal_graph;
        int causal_boosted = 0;
        int ltm_boosted = 0;

        for (int i = 0; i < input_count; i++) {
            if (!input_nodes[i]) continue;
            for (int j = 0; j < response_count; j++) {
                if (!response_nodes[j]) continue;

                float wm = 1.0f;

                // 回路7: 因果图boost
                if (cg && cg->edge_count > 0) {
                    int cg_cause = -1, cg_effect = -1;
                    for (int k = 0; k < cg->node_count; k++) {
                        if (cg_cause < 0 && cg->node_mapping[k] == input_nodes[i]->node_id)
                            cg_cause = k;
                        if (cg_effect < 0 && cg->node_mapping[k] == response_nodes[j]->node_id)
                            cg_effect = k;
                        if (cg_cause >= 0 && cg_effect >= 0) break;
                    }
                    if (cg_cause >= 0 && cg_effect >= 0) {
                        CausalEdge* ce = get_causal_edge(cg, cg_cause, cg_effect);
                        if (ce && ce->strength > 0.3f) {
                            wm *= 1.0f + ce->strength * 0.5f;
                            causal_boosted++;
                        }
                    }
                }

                // 回路9: 记忆系统boost
                if (memory && input_nodes[i]->concept && response_nodes[j]->concept) {
                    char key[512];
                    int found = 0;
                    snprintf(key, sizeof(key), "concept:%s", input_nodes[i]->concept);
                    MemoryEntry* me = memory_retrieve(memory, key);
                    if (me && me->importance > 0.5f) { wm *= 1.0f + me->importance * 0.3f; found = 1; }
                    snprintf(key, sizeof(key), "concept:%s", response_nodes[j]->concept);
                    me = memory_retrieve(memory, key);
                    if (me && me->importance > 0.5f) { wm *= 1.0f + me->importance * 0.3f; found = 1; }
                    if (found) ltm_boosted++;
                }

                boost_connection_weighted(vocab, input_nodes[i], response_nodes[j],
                                          state, wm);
            }
        }

        if (causal_boosted > 0 || ltm_boosted > 0)
            printf("[回路7+9] 因果boost: %d对 | LTM boost: %d对\n",
                   causal_boosted, ltm_boosted);
    }

    // 核心4：跨拓扑传播 — 使用已缓存的拓扑指针
    // ═══ 回路2: 激活竞争决定学习权 ═══
    // 只有本轮激活值 >= 阈值的拓扑才参与赫布学习
    // 低激活拓扑不做建边——让拓扑自己通过竞争决定哪些经验值得记录
    {
        SubTopology* targets[] = { semantic, concept_t, emotion };
        const int num_targets = 3;

        for (int tgt_i = 0; tgt_i < num_targets; tgt_i++) {
            SubTopology* tgt = targets[tgt_i];
            if (!tgt || !tgt->net) continue;

            // 激活竞争门控: recent_activation < 0.15 → 跳过学习
            if (tgt->recent_activation < 0.15f) {
                // 本轮该拓扑激活不足，仅做激活传播不建新边
                continue;
            }

            // 在目标拓扑中查找或创建节点
            ReasoningNode* tgt_input[MAX_CHARS_PER_TEXT];
            ReasoningNode* tgt_response[MAX_CHARS_PER_TEXT];

            for (int i = 0; i < input_count; i++) {
                tgt_input[i] = huarong_net_find_or_create_node(tgt->net, input_chars[i], NULL, 0, tgt->node_hash);
            }
            for (int i = 0; i < response_count; i++) {
                tgt_response[i] = huarong_net_find_or_create_node(tgt->net, response_chars[i], NULL, 0, tgt->node_hash);
            }

            // 在目标拓扑内部建边（字序编码 + 输入↔回复）
            for (int i = 0; i < input_count; i++) {
                if (!tgt_input[i]) continue;
                for (int j = i + 1; j < input_count; j++) {
                    if (!tgt_input[j]) continue;
                    int dist = j - i;
                    float wmult = (dist == 1) ? 1.5f : (1.5f / dist);
                    if (wmult < 0.3f) wmult = 0.3f;
                    boost_connection_weighted(tgt, tgt_input[i], tgt_input[j], state, wmult);
                }
            }
            for (int i = 0; i < response_count; i++) {
                if (!tgt_response[i]) continue;
                for (int j = i + 1; j < response_count; j++) {
                    if (!tgt_response[j]) continue;
                    int dist = j - i;
                    float wmult = (dist == 1) ? 1.5f : (1.5f / dist);
                    if (wmult < 0.3f) wmult = 0.3f;
                    boost_connection_weighted(tgt, tgt_response[i], tgt_response[j], state, wmult);
                }
            }
            for (int i = 0; i < input_count; i++) {
                if (!tgt_input[i]) continue;
                for (int j = 0; j < response_count; j++) {
                    if (!tgt_response[j]) continue;
                    boost_connection_weighted(tgt, tgt_input[i], tgt_response[j], state, 1.0f);
                }
            }
        }
    }

    // 核心5：跨拓扑连接 — 为当前共现的概念在拓扑之间建立跨连接
    {
        // 收集输入和回复中的所有去重汉字
        int total_chars = input_count + response_count;
        if (total_chars > 0 && total_chars <= MAX_CHARS_PER_TEXT + MAX_CHARS_PER_TEXT) {
            const char* concepts[PM_CONCEPT_MAX];
            int concnt = 0;
            char used[65536] = {0};
            for (int i = 0; i < input_count && concnt < PM_CONCEPT_MAX; i++) {
                unsigned int codepoint = 0;
                const unsigned char* p = (const unsigned char*)input_chars[i];
                if (*p < 0x80) codepoint = *p;
                else if ((*p & 0xE0) == 0xC0) codepoint = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
                else if ((*p & 0xF0) == 0xE0) codepoint = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
                if (codepoint < 65536 && !used[codepoint]) {
                    used[codepoint] = 1;
                    concepts[concnt++] = input_chars[i];
                }
            }
            for (int i = 0; i < response_count && concnt < PM_CONCEPT_MAX; i++) {
                unsigned int codepoint = 0;
                const unsigned char* p = (const unsigned char*)response_chars[i];
                if (*p < 0x80) codepoint = *p;
                else if ((*p & 0xE0) == 0xC0) codepoint = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
                else if ((*p & 0xF0) == 0xE0) codepoint = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
                if (codepoint < 65536 && !used[codepoint]) {
                    used[codepoint] = 1;
                    concepts[concnt++] = response_chars[i];
                }
            }
            if (concnt > 1) {
                int cross_created = auto_link_activated_nodes(master, concepts, concnt);
                if (cross_created > 0 && state) {
                    state->pending_updates += cross_created;
                }
            }
        }
    }

    // 刷盘判断：边数增长到一定程度触发保存
    if (state && state->initialized) {
        autonomic_request_flush(state, master);
    }
}

void autonomic_decay_all(MasterTopology* master) {
    if (!master) return;

    int total_decayed = 0;
    int competition_decayed = 0;
    int preserved = 0;

    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;

        pthread_mutex_lock(&sub->net->mutex);
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (!node || node->connection_count < 2) {
                // 单边或无连接：直接均匀衰减
                if (node) {
                    for (int e = 0; e < node->connection_count; e++) {
                        node->connection_confidences[e] *= AUTONOMIC_DECAY_RATE;
                        if (node->connection_confidences[e] < 0.05f)
                            node->connection_confidences[e] = 0.05f;
                        total_decayed++;
                    }
                }
                continue;
            }

            // 计算该节点所有出边的平均置信度
            float sum_conf = 0.0f;
            for (int e = 0; e < node->connection_count; e++)
                sum_conf += node->connection_confidences[e];
            float avg_conf = sum_conf / node->connection_count;

            // ═══ 回路5: Fisher信息代理（selection_count + confidence 保护重要边）═══
            float node_importance = (node->selection_count > 0)
                ? 1.0f / (1.0f + 0.05f * node->selection_count)  // 越重要衰减越慢
                : 1.0f;
            float eff_decay = 1.0f - (1.0f - AUTONOMIC_DECAY_RATE) * node_importance;

            // 三档差异化衰减
            for (int e = 0; e < node->connection_count; e++) {
                float conf = node->connection_confidences[e];
                float rate;

                if (conf > avg_conf * 1.5f && conf > 0.5f) {
                    // 明显高于同节点其他边：高置信"赢家"，几乎不衰减
                    rate = 1.0f - (1.0f - 0.9995f) * node_importance;
                    preserved++;
                } else if (conf < avg_conf * 0.5f && conf < 0.3f) {
                    // 明显低于同节点其他边：低置信"输家"，加速衰减
                    rate = 0.85f;
                    competition_decayed++;
                } else {
                    // 中间区域：标准衰减（含重要性保护）
                    rate = 1.0f - (1.0f - AUTONOMIC_DECAY_RATE) * node_importance;
                }

                node->connection_confidences[e] = conf * rate;
                if (node->connection_confidences[e] < 0.05f)
                    node->connection_confidences[e] = 0.05f;
                total_decayed++;
            }
        }
        pthread_mutex_unlock(&sub->net->mutex);
    }
    printf("[自主学习] 全局衰减: %d 条 (竞争加速: %d, 保留: %d)\n",
           total_decayed, competition_decayed, preserved);
}

// ==================== 批量文本学习（替代 reader 工具） ====================

/**
 * 判断是否为句子分隔符（。！？；\\n）
 * 输入为 UTF-8 编码的单字节或中文字符首字节
 */
static int is_sentence_sep(const char* p) {
    if (!p) return 0;
    unsigned char c = (unsigned char)*p;
    if (c == '\n' || c == '\r') return 1;
    // 中文标点：UTF-8 编码的 。！？；
    if (c == 0xE3) {
        unsigned char c2 = (unsigned char)*(p+1);
        unsigned char c3 = (unsigned char)*(p+2);
        if (c2 == 0x80 && c3 == 0x82) return 1;  // 。
        if (c2 == 0x80 && c3 == 0x81) return 1;  // 、（非句子分隔，但可作为短语边界）
    }
    if (c == 0xEF) {
        unsigned char c2 = (unsigned char)*(p+1);
        unsigned char c3 = (unsigned char)*(p+2);
        if (c2 == 0xBC && c3 == 0x81) return 1;  // ！
        if (c2 == 0xBC && c3 == 0x9F) return 1;  // ？
        if (c2 == 0xBC && c3 == 0x9B) return 1;  // ；
    }
    return 0;
}

int autonomic_learn_from_text(MasterTopology* master,
                              const char* text,
                              int text_len,
                              AutonomicState* state) {
    if (!master || !text) return 0;

    if (text_len < 0) text_len = (int)strlen(text);
    if (text_len == 0) return 0;

    // 收集句子起始位置
    #define MAX_SENTENCES 10000
    const char* sentence_starts[MAX_SENTENCES];
    int sentence_lens[MAX_SENTENCES];
    int scount = 0;

    const char* sent_start = text;
    int sent_len = 0;
    const char* p = text;
    const char* end = text + text_len;

    while (p < end && scount < MAX_SENTENCES) {
        int clen = utf8_char_len((unsigned char)*p);
        if (clen <= 0) { p++; continue; }

        if (is_sentence_sep(p)) {
            // 收集当前句子（如果非空）
            if (sent_len > 0) {
                sentence_starts[scount] = sent_start;
                sentence_lens[scount] = sent_len;
                scount++;
            }
            p += clen;
            sent_start = p;
            sent_len = 0;
        } else {
            sent_len += clen;
            p += clen;
        }
    }
    // 最后一句
    if (sent_len > 0 && scount < MAX_SENTENCES) {
        sentence_starts[scount] = sent_start;
        sentence_lens[scount] = sent_len;
        scount++;
    }

    if (scount < 2) return 0;

    // 逐对推进：每对相邻句子作为 (输入, 回复)
    int pairs = 0;
    char buf_a[PM_CHARS_PER_TEXT * 4];   // 最坏每个字4字节
    char buf_b[PM_CHARS_PER_TEXT * 4];

    for (int i = 0; i < scount - 1; i++) {
        int len_a = sentence_lens[i];
        int len_b = sentence_lens[i + 1];
        if (len_a <= 0 || len_b <= 0) continue;

        // 截断到缓冲大小
        if (len_a >= (int)sizeof(buf_a)) len_a = (int)sizeof(buf_a) - 1;
        if (len_b >= (int)sizeof(buf_b)) len_b = (int)sizeof(buf_b) - 1;

        memcpy(buf_a, sentence_starts[i], len_a);
        buf_a[len_a] = '\0';
        memcpy(buf_b, sentence_starts[i + 1], len_b);
        buf_b[len_b] = '\0';

        autonomic_learn_from_dialog(master, buf_a, buf_b, state, NULL, NULL);
        pairs++;

        // 每100对打印进度
        if (pairs % 100 == 0) {
            printf("[文本学习] 已处理 %d 句对...\r", pairs);
            fflush(stdout);
        }
    }

    printf("[文本学习] 完成: %d 句对, %d 句\n", pairs, scount);
    return pairs;
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

        pthread_mutex_lock(&sub->net->mutex);
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
    autonomic_learn_from_dialog(master, "学习", "机器", &state, NULL, NULL);

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
    autonomic_learn_from_dialog(master, "学习", "机器", &state, NULL, NULL);

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
