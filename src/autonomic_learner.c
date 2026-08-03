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

#include "error.h"
#include "autonomic_learner.h"
#include "common.h"
#include "huarong_topology.h"
#include "utf8_tokenizer.h"
#include "node_hash.h"
#include "cross_edge_io.h"
#include "causal_reasoning.h"
#include "memory_system.h"
#include "feature_learn.h"
#include "topology_growth.h"
#include "feature_io.h"
#include "cognitive_controller.h"  /* POSTag / pos_tag_chinese for syntax topology */
#include "cognitive_params.h"       /* CognitiveState for emotional modulation */
#include "dict_loader.h"           /* 词典分词 + 词性标注 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
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
/* 线程局部存储：__thread (GCC/Clang), __declspec(thread) (MSVC), _Thread_local (C11) */
#if defined(__GNUC__) || defined(__clang__)
#define PM_THREAD_LOCAL __thread
#elif defined(_MSC_VER)
#define PM_THREAD_LOCAL __declspec(thread)
#else
#define PM_THREAD_LOCAL _Thread_local
#endif

#define MAX_ACTIVATED_PAIRS PM_ACTIVATED_PAIRS

/* 全局激活记录（每轮对话重置）—— 线程局部存储支持可重入
 * 使用 (topo_type << 24 | node_id) 复合键避免跨拓扑 node_id 冲突 */
static PM_THREAD_LOCAL ActivatedEdges g_activated[MAX_ACTIVATED_PAIRS];
static PM_THREAD_LOCAL int g_activated_count;

static void reset_activation_record(void) {
    g_activated_count = 0;
}

/* 使用复合键: (topo_type << 24) | node_id
 * 约束: topo_type 须 < 256（当前拓扑类型枚举远小于此值，安全） */
static int make_compound_key(int topo_type, int node_id) {
    return (topo_type << 24) | (node_id & 0xFFFFFF);
}

static void record_edge_activated(ReasoningNode* node, int edge_index, int topo_type) {
    if (!node) return;
    int compound_id = make_compound_key(topo_type, node->node_id);
    for (int i = 0; i < g_activated_count; i++) {
        if (g_activated[i].node_id == compound_id) {
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
        g_activated[g_activated_count].node_id = compound_id;
        g_activated[g_activated_count].edge_count = 1;
        g_activated[g_activated_count].edge_indices[0] = edge_index;
        g_activated_count++;
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
    pthread_mutex_init(&state->flush_mutex, NULL);
    pthread_cond_init(&state->flush_cond, NULL);
    state->flush_threshold = PM_AUTONOMIC_FLUSH_THRESHOLD;
    state->idle_flush_seconds = PM_AUTONOMIC_IDLE_FLUSH_SECS;
    state->max_pending_edges = 500;
    state->local_buffer_count = 0;
    state->initialized = 1;
}

void autonomic_state_destroy(AutonomicState* state) {
    if (!state) return;
    // 安全停止：如果异步线程还在运行，先停掉
    if (state->flush_master && !state->shutdown) {
        state->shutdown = 1;
        pthread_mutex_lock(&state->flush_mutex);
        pthread_cond_signal(&state->flush_cond);
        pthread_mutex_unlock(&state->flush_mutex);
        pthread_join(state->flush_thread, NULL);
    }
    for (int i = 0; i < AUTONOMIC_SHARD_COUNT; i++) {
        pthread_mutex_destroy(&state->shards[i].lock);
    }
    pthread_mutex_destroy(&state->flush_lock);
    pthread_mutex_destroy(&state->flush_mutex);
    pthread_cond_destroy(&state->flush_cond);
    memset(state, 0, sizeof(AutonomicState));
}

// ==================== 刷盘工作函数（提取公共逻辑） ====================

/**
 * 执行刷盘的具体工作：剪枝、特征学习、特征吸引、保存
 * 调用方已持有 flush_lock，保证单线程执行
 */
static void do_flush_work(AutonomicState* state, MasterTopology* master, time_t now) {
    LOG_INFO("[自主学习刷盘] %d 次更新, 距上次 %lds",
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
                    if (!node) continue;
                    if (!node->features) lazy_alloc_node_features(node);
                    if (!node->features) continue;
                    for (int c = 0; c < node->edge_count; c++) {
                        if (node->edges[c].confidence < 0.6f) continue;
                        ReasoningNode* nb = node->edges[c].target;
                        if (!nb) continue;
                        if (!nb->features) lazy_alloc_node_features(nb);
                        if (!nb->features || nb->feature_dim != node->feature_dim) continue;
                        hebbian_update(node->features, nb->features, node->feature_dim, 0.01f);
                        attract_count++;
                    }
                }
            }
            if (attract_count > 0) {
                LOG_INFO("[自主学习刷盘] 特征吸引: %d 对", attract_count);
            }
        }

        char path[PM_PATH_BUF];
        snprintf(path, sizeof(path), "pivotmind_state.dat");

        // 持久化拓扑（单一文件，不备份）
        int saved = master_save_state(master, path);
        if (saved > 0) {
            LOG_INFO("[自主学习刷盘] ✓ 已保存到 %s (%d 节点)", path, saved);
        } else {
            LOG_ERROR("[自主学习刷盘] × 保存失败");
        }

        // 同时保存特征向量, 确保与拓扑状态同步
        int feat_saved = save_features(master, "features.bin");
        if (feat_saved > 0) {
            LOG_INFO("[自主学习刷盘] ✓ 已保存特征向量 (%d 节点)", feat_saved);
        }
    }

    // 重置计数
    state->pending_updates = 0;
    state->last_flush_time = now;
}

// ==================== 异步刷盘后台线程 ====================

/**
 * 异步刷盘线程主函数
 * 等待条件变量信号（flush_requested 或 shutdown 或超时），
 * 收到信号后执行 do_flush_work，调用方不等待。
 */
static void* flush_thread_worker(void* arg) {
    AutonomicState* state = (AutonomicState*)arg;
    if (!state) return NULL;

    struct timespec ts;
    MasterTopology* master = state->flush_master;

    while (1) {
        pthread_mutex_lock(&state->flush_mutex);

        // 等待信号：被 flush_requested 唤醒 或 每秒超时检查空闲条件
        while (!state->flush_requested && !state->shutdown) {
            ts.tv_sec = time(NULL) + 1;
            ts.tv_nsec = 0;
            pthread_cond_timedwait(&state->flush_cond, &state->flush_mutex, &ts);

            // 周期性检查空闲刷盘条件
            if (!state->flush_requested && !state->shutdown && state->initialized) {
                time_t now = time(NULL);
                if (state->pending_updates > 0 &&
                    (now - state->last_flush_time) >= state->idle_flush_seconds) {
                    state->flush_requested = 1;
                }
            }
        }

        if (state->shutdown) {
            pthread_mutex_unlock(&state->flush_mutex);
            break;
        }

        state->flush_requested = 0;
        state->flush_running = 1;
        pthread_mutex_unlock(&state->flush_mutex);

        // 执行刷盘（flush_lock 防止并发，不持 topology 锁，学习线程可运行）
        pthread_mutex_lock(&state->flush_lock);
        if (master && state->initialized && !state->shutdown) {
            do_flush_work(state, master, time(NULL));
        }
        pthread_mutex_unlock(&state->flush_lock);

        // 在 flush_mutex 保护下重置 running，确保主线程路径可靠读取
        pthread_mutex_lock(&state->flush_mutex);
        state->flush_running = 0;
        pthread_mutex_unlock(&state->flush_mutex);
    }

    return NULL;
}

int autonomic_start_async_flush(AutonomicState* state, MasterTopology* master) {
    if (!state || !state->initialized || !master) return 0;

    state->flush_master = master;
    state->shutdown = 0;
    state->flush_requested = 0;
    state->flush_running = 0;

    int ret = pthread_create(&state->flush_thread, NULL, flush_thread_worker, state);
    if (ret != 0) {
        LOG_ERROR("[异步刷盘] 创建线程失败: %d", ret);
        return 0;
    }

    LOG_INFO("[异步刷盘] 后台线程已启动");
    return 1;
}

void autonomic_stop_async_flush(AutonomicState* state) {
    if (!state || !state->initialized) return;

    state->shutdown = 1;
    pthread_mutex_lock(&state->flush_mutex);
    pthread_cond_signal(&state->flush_cond);
    pthread_mutex_unlock(&state->flush_mutex);

    pthread_join(state->flush_thread, NULL);
    LOG_INFO("[异步刷盘] 后台线程已停止");
}

// ==================== 同步/异步刷盘入口 ====================

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

    // ========== 异步路径：发信号给后台线程，立刻返回 ==========
    if (state->flush_master && !state->shutdown && !state->flush_running) {
        pthread_mutex_lock(&state->flush_mutex);
        // double-check under mutex
        if (!state->flush_running && !state->shutdown) {
            state->flush_requested = 1;
            pthread_cond_signal(&state->flush_cond);
            pthread_mutex_unlock(&state->flush_mutex);
            return;
        }
        pthread_mutex_unlock(&state->flush_mutex);
        // 如果正在刷盘，本次跳过（不重复排入）
        return;
    }

    // ========== 同步路径（无异步线程时回退） ==========
    pthread_mutex_lock(&state->flush_lock);

    if (state->pending_updates < state->flush_threshold &&
        (now - state->last_flush_time) < state->idle_flush_seconds) {
        pthread_mutex_unlock(&state->flush_lock);
        return;
    }

    do_flush_work(state, master, now);

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

/* ================================================================
 *  原子浮点加法 — CAS 循环实现
 *  Mingw GCC 不支持 __atomic_fetch_add(float*, ...)
 *  注: CAS 循环存在理论 ABA 风险（另一线程在同一位置做两次修改
 *  导致值回到原样时 CAS 误判成功）；当前使用场景（权重/置信度累加）
 *  下最坏情况丢失一次微小浮点更新，不会导致 crash，可接受
 * ================================================================ */
static inline void atomic_float_add(float* ptr, float val) {
    union { float f; uint32_t i; } old, new_val;
    do {
        old.f = *ptr;
        new_val.f = old.f + val;
    } while (!__sync_bool_compare_and_swap((uint32_t*)ptr, old.i, new_val.i));
}


// ==================== 在拓扑中查找节点 ====================

// ==================== 建边并涨置信度 ====================

static void boost_connection_weighted(SubTopology* topo, ReasoningNode* a, ReasoningNode* b,
                                      AutonomicState* state, float weight_mult) {
    if (!a || !b || a == b) return;

    HuarongTopologyNet* net = (topo && topo->net) ? topo->net : NULL;

    if (a->edge_count >= AUTONOMIC_MAX_CONNECTIONS ||
        b->edge_count >= AUTONOMIC_MAX_CONNECTIONS) return;

    float base_weight = AUTONOMIC_BASE_WEIGHT * weight_mult;
    // 回路6: 被走边频繁选中的节点对，赫布学习时给更大boost
    float sel_boost = 1.0f + 0.1f * ((a->selection_count + b->selection_count) * 0.5f) / 10.0f;
    if (sel_boost > 1.5f) sel_boost = 1.5f;
    base_weight *= sel_boost;
    if (base_weight > 0.9f) base_weight = 0.9f;

    int existing_a_to_b = -1;
    int existing_b_to_a = -1;
    
    /* 节点级条纹锁：hash(node_id) & (PM_NODE_LOCK_COUNT - 1) 选锁
     * 20线程 × 分散到256把锁 → 对撞率 <8%，无撞零等待
     * 注意：node_conn_find 在锁外执行，锁内 double-check 防御 TOCTOU */
    int li_a = (a && net) ? (a->node_id & (PM_NODE_LOCK_COUNT - 1)) : -1;
    int li_b = (b && net) ? (b->node_id & (PM_NODE_LOCK_COUNT - 1)) : -1;

    existing_a_to_b = node_conn_find(a, b);
    if (existing_a_to_b >= 0 && net && li_a >= 0) {
        pthread_mutex_lock(&net->node_locks[li_a]);
        /* double-check：持锁后重新查找，防止并发剪枝导致下标失效 */
        int idx = node_conn_find(a, b);
        if (idx >= 0 && idx < a->edge_count) {
            float dw = AUTONOMIC_LEARNING_RATE * 0.5f * weight_mult;
            a->edges[idx].weight += dw;
            if (a->edges[idx].weight > 0.9f)
                a->edges[idx].weight = 0.9f;
            float dc = AUTONOMIC_LEARNING_RATE * (1.0f - a->edges[idx].confidence);
            a->edges[idx].confidence += dc;
            existing_a_to_b = idx;
        } else {
            existing_a_to_b = -1;
        }
        pthread_mutex_unlock(&net->node_locks[li_a]);
    }
    existing_b_to_a = node_conn_find(b, a);
    if (existing_b_to_a >= 0 && net && li_b >= 0) {
        pthread_mutex_lock(&net->node_locks[li_b]);
        int idx = node_conn_find(b, a);
        if (idx >= 0 && idx < b->edge_count) {
            float dw = AUTONOMIC_LEARNING_RATE * 0.5f * weight_mult;
            b->edges[idx].weight += dw;
            if (b->edges[idx].weight > 0.9f)
                b->edges[idx].weight = 0.9f;
            float dc = AUTONOMIC_LEARNING_RATE * (1.0f - b->edges[idx].confidence);
            b->edges[idx].confidence += dc;
            existing_b_to_a = idx;
        } else {
            existing_b_to_a = -1;
        }
        pthread_mutex_unlock(&net->node_locks[li_b]);
    }
    if (existing_a_to_b >= 0) record_edge_activated(a, existing_a_to_b,
        topo ? (int)topo->type : 0);
    if (existing_b_to_a >= 0) record_edge_activated(b, existing_b_to_a,
        topo ? (int)topo->type : 0);

    // ── 新建边：add_connection 内部持锁扩容 ──
    if (existing_a_to_b < 0 && net) {
        int ret = huarong_net_add_connection(net, a->node_id, b->node_id, base_weight);
        if (ret == 0) {
            int idx = node_conn_find(a, b);
            if (idx >= 0) {
                a->edges[idx].confidence = AUTONOMIC_INITIAL_CONFIDENCE;
                record_edge_activated(a, idx, topo ? (int)topo->type : 0);
                if (a->features && b->features && a->feature_dim == b->feature_dim)
                    hebbian_update(a->features, b->features, a->feature_dim, 0.02f);
            }
        }
    }
    if (existing_b_to_a < 0 && net) {
        huarong_net_add_connection(net, b->node_id, a->node_id, base_weight);
        int idx = node_conn_find(b, a);
        if (idx >= 0) {
            b->edges[idx].confidence = AUTONOMIC_INITIAL_CONFIDENCE;
        }
    }


    // 刷新状态累加器
    if (state && state->initialized) {
        __sync_fetch_and_add(&state->pending_updates, 1);
        int sIdx = edge_shard_index(a->node_id, b->node_id);
        pthread_mutex_lock(&state->shards[sIdx].lock);
        state->shards[sIdx].pending_count++;
        pthread_mutex_unlock(&state->shards[sIdx].lock);
    }
}

// ==================== 核心API实现 ====================


/* v0.5.7: 拓扑种子注入（独立挂载——brainstem 启动时调用，
 * 不依赖对话路径。此前种子注入嵌在 autonomic_learn_from_dialog
 * 里，对话少/走 PFE 路径时从不触发 → 领域拓扑永远 0 节点） */
void autonomic_seed_topologies(MasterTopology* master) {
    if (!master) return;
    SubTopology* vocab = NULL, *semantic = NULL, *concept_t = NULL;
    SubTopology* emotion = NULL, *syntax = NULL, *context = NULL;
    SubTopology* domain = NULL, *pragma = NULL, *culture = NULL;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub) continue;
        switch (sub->type) {
            case TOPO_VOCABULARY: vocab = sub; break;
            case TOPO_SEMANTIC:   semantic = sub; break;
            case TOPO_CONCEPT:    concept_t = sub; break;
            case TOPO_EMOTION:    emotion = sub; break;
            case TOPO_SYNTAX:     syntax = sub; break;
            case TOPO_CONTEXT:    context = sub; break;
            case TOPO_DOMAIN:     domain = sub; break;
            case TOPO_PRAGMA:     pragma = sub; break;
            case TOPO_CULTURE:    culture = sub; break;
            default: break;
        }
    }
    if (!vocab || !vocab->net) return;

    /* 种子节点：为空拓扑注入初始数据（仅首次运行） */
    {
        SubTopology* seeds[] = { syntax, context, domain, pragma, culture };
        const char* seed_names[] = {
            /* syntax: 基础POS标签 */
            "NOUN\0VERB\0ADJ\0ADV\0PRON\0PREP\0CONJ\0NUM\0PART\0INTJ",
            /* context: 上下文类型 */
            "Q&A\0CHAT\0EXPLAIN\0HOWTO\0GREET",
            /* domain: 领域标签（v0.5.7 扩展心理/医疗/情绪域） */
            "TECH\0LIFE\0SCIENCE\0HISTORY\0GEOGRAPHY\0PSYCHOLOGY\0MEDICAL\0EMOTION\0LAW\0ECONOMICS",
            /* pragma: 语用功能 */
            "ASK\0TELL\0REQUEST\0SUGGEST\0GREET",
            /* culture: 文化元素 */
            "CN_CULTURE\0FESTIVAL\0CUSTOM\0LANGUAGE\0HISTORY"
        };
        for (int si = 0; si < 5; si++) {
            SubTopology* st = seeds[si];
            if (!st || !st->net || st->net->node_count > 0) continue;
            const char* names = seed_names[si];
            while (*names) {
                huarong_net_find_or_create_node(st->net, names, NULL, 0, st->node_hash);
                names += strlen(names) + 1;
            }
        }
    }
}

void autonomic_learn_from_dialog(MasterTopology* master,
                                 const char* user_input,
                                 const char* ai_response,
                                 AutonomicState* state,
                                 void* causal_graph,
                                 MemorySystem* memory) {
    if (!master || !user_input || !ai_response) return;
    if (strlen(user_input) == 0 || strlen(ai_response) == 0) return;

    reset_activation_record();

    /* 情绪调制因子：正效价→加速学习，负效价→抑制（0.5x ~ 1.5x） */
    float emotive_weight_mult = 1.0f;
    if (master->cognitive_state_ptr) {
        float v = ((CognitiveState*)master->cognitive_state_ptr)->valence;
        emotive_weight_mult = 1.0f + v * 0.5f;
        if (emotive_weight_mult < 0.3f) emotive_weight_mult = 0.3f;
        if (emotive_weight_mult > 2.0f) emotive_weight_mult = 2.0f;
    }

    // 一次性扫描缓存四种拓扑指针（O(n) → 1 次遍历）
    SubTopology* vocab = NULL;
    SubTopology* semantic = NULL;
    SubTopology* concept_t = NULL;
    SubTopology* emotion = NULL;
    SubTopology* syntax = NULL;
    SubTopology* context = NULL;
    SubTopology* domain = NULL;
    SubTopology* pragma = NULL;
    SubTopology* culture = NULL;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub) continue;
        switch (sub->type) {
            case TOPO_VOCABULARY: vocab = sub; break;
            case TOPO_SEMANTIC:   semantic = sub; break;
            case TOPO_CONCEPT:    concept_t = sub; break;
            case TOPO_EMOTION:    emotion = sub; break;
            case TOPO_SYNTAX:     syntax = sub; break;
            case TOPO_CONTEXT:    context = sub; break;
            case TOPO_DOMAIN:     domain = sub; break;
            case TOPO_PRAGMA:     pragma = sub; break;
            case TOPO_CULTURE:    culture = sub; break;
            default: break;
        }
    }
    if (!vocab || !vocab->net) return;

    /* 种子节点：为空拓扑注入初始数据（仅首次运行） */
    {
        SubTopology* seeds[] = { syntax, context, domain, pragma, culture };
        const char* seed_names[] = {
            /* syntax: 基础POS标签 */
            "NOUN\0VERB\0ADJ\0ADV\0PRON\0PREP\0CONJ\0NUM\0PART\0INTJ",
            /* context: 上下文类型 */
            "Q&A\0CHAT\0EXPLAIN\0HOWTO\0GREET",
            /* domain: 领域标签 */
            "TECH\0LIFE\0SCIENCE\0HISTORY\0GEOGRAPHY",
            /* pragma: 语用功能 */
            "ASK\0TELL\0REQUEST\0SUGGEST\0GREET",
            /* culture: 文化元素 */
            "CN_CULTURE\0FESTIVAL\0CUSTOM\0LANGUAGE\0HISTORY"
        };
        for (int si = 0; si < 5; si++) {
            SubTopology* st = seeds[si];
            if (!st || !st->net || st->net->node_count > 0) continue;
            const char* names = seed_names[si];
            while (*names) {
                huarong_net_find_or_create_node(st->net, names, NULL, 0, st->node_hash);
                names += strlen(names) + 1;
            }
        }
    }

    // ===== 提取词汇 =====
    // 有词典时：分词 + 词性标注；无词典时：逐字提取（兼容旧行为）
    char input_chars[MAX_CHARS_PER_TEXT][64];
    char response_chars[MAX_CHARS_PER_TEXT][64];
    char input_pos[MAX_CHARS_PER_TEXT][8];
    char response_pos[MAX_CHARS_PER_TEXT][8];
    int input_utf8_lens[MAX_CHARS_PER_TEXT];
    int response_utf8_lens[MAX_CHARS_PER_TEXT];

    DictTable* dict = (DictTable*)master->ext_dict;
    int input_count, response_count;

    if (dict) {
        /* 词典分词路径：词级建模 + 词性标注 */
        input_count  = dict_segment_text(dict, user_input,  input_chars,  input_pos,  MAX_CHARS_PER_TEXT);
        response_count = dict_segment_text(dict, ai_response, response_chars, response_pos, MAX_CHARS_PER_TEXT);
    } else {
        /* 逐字路径（无词典时兼容旧逻辑） */
        char tmp_in_chars[MAX_CHARS_PER_TEXT][8];
        char tmp_res_chars[MAX_CHARS_PER_TEXT][8];
        input_count  = extract_ordered_chars(user_input,  tmp_in_chars,  input_utf8_lens);
        response_count = extract_ordered_chars(ai_response, tmp_res_chars, response_utf8_lens);
        /* 复制到 input_chars 大缓冲区 */
        for (int i = 0; i < input_count; i++)  snprintf(input_chars[i], 64, "%s", tmp_in_chars[i]);
        for (int i = 0; i < response_count; i++) snprintf(response_chars[i], 64, "%s", tmp_res_chars[i]);
        /* 无词典时词性标记为空 */
        for (int i = 0; i < input_count; i++)  input_pos[i][0] = '\0';
        for (int i = 0; i < response_count; i++) response_pos[i][0] = '\0';
    }
    if (input_count == 0 || response_count == 0) return;

    // 查找或创建词汇拓扑节点
    ReasoningNode* input_nodes[MAX_CHARS_PER_TEXT];
    ReasoningNode* response_nodes[MAX_CHARS_PER_TEXT];

    for (int i = 0; i < input_count; i++) {
        input_nodes[i] = huarong_net_find_or_create_node(vocab->net, input_chars[i], NULL, 0, vocab->node_hash);
    }
    for (int i = 0; i < response_count; i++) {
        response_nodes[i] = huarong_net_find_or_create_node(vocab->net, response_chars[i], NULL, 0, vocab->node_hash);
    }

    // ===== 词典词 → 语法拓扑 POS 连接 =====
    if (dict && syntax && syntax->net) {
        for (int i = 0; i < input_count; i++) {
            if (!input_nodes[i] || input_pos[i][0] == '\0' || input_pos[i][0] == 'x') continue;
            const char* syntax_name = pos_to_syntax_node(input_pos[i]);
            if (!syntax_name) continue;
            ReasoningNode* pos_node = huarong_net_find_or_create_node(
                syntax->net, syntax_name, NULL, 0, syntax->node_hash);
            if (pos_node) {
                /* 词汇节点 → 语法 POS 节点（跨拓扑连接） */
                boost_connection_weighted(vocab, input_nodes[i], pos_node, state, 0.3f * emotive_weight_mult);
            }
        }
        for (int i = 0; i < response_count; i++) {
            if (!response_nodes[i] || response_pos[i][0] == '\0' || response_pos[i][0] == 'x') continue;
            const char* syntax_name = pos_to_syntax_node(response_pos[i]);
            if (!syntax_name) continue;
            ReasoningNode* pos_node = huarong_net_find_or_create_node(
                syntax->net, syntax_name, NULL, 0, syntax->node_hash);
            if (pos_node) {
                boost_connection_weighted(vocab, response_nodes[i], pos_node, state, 0.3f * emotive_weight_mult);
            }
        }
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
            boost_connection_weighted(vocab, input_nodes[i], input_nodes[j], state, wmult * emotive_weight_mult);
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
            boost_connection_weighted(vocab, response_nodes[i], response_nodes[j], state, wmult * emotive_weight_mult);
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
                                          state, wm * emotive_weight_mult);
            }
        }

        if (causal_boosted > 0 || ltm_boosted > 0)
            LOG_DEBUG("[回路7+9] 因果boost: %d对 | LTM boost: %d对",
                      causal_boosted, ltm_boosted);
    }

    // 核心4：跨拓扑传播 — 使用已缓存的拓扑指针
    // ═══ 回路2: 激活竞争决定学习权 ═══
    // 只有本轮激活值 >= 阈值的拓扑才参与赫布学习
    // 低激活拓扑不做建边——让拓扑自己通过竞争决定哪些经验值得记录
    {
        SubTopology* targets[] = { semantic, concept_t, emotion, syntax, context, domain, pragma, culture };
        const int num_targets = 8;

        for (int tgt_i = 0; tgt_i < num_targets; tgt_i++) {
            SubTopology* tgt = targets[tgt_i];
            if (!tgt || !tgt->net) continue;

            // 激活竞争学习率: 低激活拓扑用衰减学习率而非完全跳过
            float lr_scale = (tgt->recent_activation < 0.05f) ? 0.1f :
                             (tgt->recent_activation < 0.15f) ? tgt->recent_activation * 2.0f : 1.0f;

            int is_syntax = (tgt->type == TOPO_SYNTAX);

            // 在目标拓扑中查找或创建节点
            ReasoningNode* tgt_input[MAX_CHARS_PER_TEXT];
            ReasoningNode* tgt_response[MAX_CHARS_PER_TEXT];

            /* 语法拓扑: 用涌现词类替代硬编码词典 */
            CognitiveController* cc = master->cognitive_controller;
            for (int i = 0; i < input_count; i++) {
                const char* name = input_chars[i];
                if (is_syntax) name = pos_tag_name(
                    cc ? pos_tag_emergent(cc, name) : pos_tag_chinese(name));
                tgt_input[i] = huarong_net_find_or_create_node(tgt->net, name, NULL, 0, tgt->node_hash);
            }
            for (int i = 0; i < response_count; i++) {
                const char* name = response_chars[i];
                if (is_syntax) name = pos_tag_name(
                    cc ? pos_tag_emergent(cc, name) : pos_tag_chinese(name));
                tgt_response[i] = huarong_net_find_or_create_node(tgt->net, name, NULL, 0, tgt->node_hash);
            }

            // 在目标拓扑内部建边（字序编码 + 输入↔回复）
            for (int i = 0; i < input_count; i++) {
                if (!tgt_input[i]) continue;
                for (int j = i + 1; j < input_count; j++) {
                    if (!tgt_input[j]) continue;
                    int dist = j - i;
                    float wmult = (dist == 1) ? 1.5f : (1.5f / dist);
                    if (wmult < 0.3f) wmult = 0.3f;
                    boost_connection_weighted(tgt, tgt_input[i], tgt_input[j], state, wmult * lr_scale * emotive_weight_mult);
                }
            }
            for (int i = 0; i < response_count; i++) {
                if (!tgt_response[i]) continue;
                for (int j = i + 1; j < response_count; j++) {
                    if (!tgt_response[j]) continue;
                    int dist = j - i;
                    float wmult = (dist == 1) ? 1.5f : (1.5f / dist);
                    if (wmult < 0.3f) wmult = 0.3f;
                    boost_connection_weighted(tgt, tgt_response[i], tgt_response[j], state, wmult * lr_scale * emotive_weight_mult);
                }
            }
            for (int i = 0; i < input_count; i++) {
                if (!tgt_input[i]) continue;
                for (int j = 0; j < response_count; j++) {
                    if (!tgt_response[j]) continue;
                    boost_connection_weighted(tgt, tgt_input[i], tgt_response[j], state, lr_scale * emotive_weight_mult);
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
            char* used = (char*)calloc(65536, 1);
            if (!used) goto skip_cross_topo;  /* OOM — 跳过本轮跨拓扑连接 */
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
            free(used);
            if (concnt > 1) {
                int cross_created = auto_link_activated_nodes(master, concepts, concnt);
                if (cross_created > 0 && state) {
                    state->pending_updates += cross_created;
                }
            }
        }
    }
skip_cross_topo:
    (void)0;

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

        pthread_rwlock_wrlock(&sub->net->mutex);
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (!node || node->edge_count < 2) {
                // 单边或无连接：直接均匀衰减
                if (node) {
                    for (int e = 0; e < node->edge_count; e++) {
                        node->edges[e].confidence *= AUTONOMIC_DECAY_RATE;
                        if (node->edges[e].confidence < 0.05f)
                            node->edges[e].confidence = 0.05f;
                        total_decayed++;
                    }
                }
                continue;
            }

            // 计算该节点所有出边的平均置信度
            float sum_conf = 0.0f;
            for (int e = 0; e < node->edge_count; e++)
                sum_conf += node->edges[e].confidence;
            float avg_conf = sum_conf / node->edge_count;

            // ═══ 回路5: Fisher信息代理（selection_count + confidence 保护重要边）═══
            float node_importance = (node->selection_count > 0)
                ? 1.0f / (1.0f + 0.05f * node->selection_count)  // 越重要衰减越慢
                : 1.0f;
            // 三档差异化衰减
            for (int e = 0; e < node->edge_count; e++) {
                float conf = node->edges[e].confidence;
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

                node->edges[e].confidence = conf * rate;
                if (node->edges[e].confidence < 0.05f)
                    node->edges[e].confidence = 0.05f;
                total_decayed++;
            }
        }
        pthread_rwlock_unlock(&sub->net->mutex);
    }
    LOG_INFO("[自主学习] 全局衰减: %d 条 (竞争加速: %d, 保留: %d)",
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
            LOG_INFO("[文本学习] 已处理 %d 句对...", pairs);
        }
    }

    LOG_INFO("[文本学习] 完成: %d 句对, %d 句", pairs, scount);
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

        pthread_rwlock_wrlock(&sub->net->mutex);
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (!node) continue;

            for (int e = 0; e < node->edge_count; e++) {
                total_edges++;
                sum_confidence += node->edges[e].confidence;
            }
        }
        pthread_rwlock_unlock(&sub->net->mutex);
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
        for (int i = 0; i < node_xue->edge_count; i++) {
            if (node_xue->edges[i].target && node_xue->edges[i].target->concept) {
                printf("  → %s (weight=%.3f, conf=%.3f)\n",
                       node_xue->edges[i].target->concept,
                       node_xue->edges[i].weight,
                       node_xue->edges[i].confidence);
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
        for (int i = 0; i < node_xue->edge_count; i++) {
            if (node_xue->edges[i].target && node_xue->edges[i].target->concept) {
                printf("  → %s (weight=%.3f, conf=%.3f)\n",
                       node_xue->edges[i].target->concept,
                       node_xue->edges[i].weight,
                       node_xue->edges[i].confidence);
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

/* ==================== 词巩固（字拓扑 → 概念拓扑晋升） ==================== */

#define CC_MIN_EDGES       5      /* 字节点边数下限（防稀疏噪声） */
#define CC_THRESHOLD_DEF  1.5f   /* 默认相对强度阈值：w(A,B) > avg(A)*N（可调，2.0 太严，高频字组合如"三国"涌现不出） */
#define CC_MAX_WORDS       2000   /* 每周期涌现上限（20 本书 200 个远不够） */
#define CC_STRENGTH_CAP    0.9f   /* cross-link 权重上限 */

/* 全局可调阈值（后续调参用）：词巩固相对强度阈值 */
float g_compound_threshold = CC_THRESHOLD_DEF;

static int cc_is_cjk_char(const char* s) {
    unsigned char c = (unsigned char)s[0];
    if (c >= 0xE0 && c <= 0xEF)
        return (s[1] != 0 && s[2] != 0 && s[3] == 0) ? 1 : 0;
    return 0;
}

/* 纯虚字表：这些字参与的"词"大多是虚词/短语噪声（的武/你是/很大），
 * 涌现实义词（三国/关羽/武器）才有语义价值（v0.6 调参） */
static int cc_is_void_char(const char* s) {
    static const char* voids[] = {
        "的","了","是","不","也","都","就","很","在","有","这","那",
        "和","与","或","之","其","于","以","而","且","吧","吗","呢",
        "啊","个","们","着","过","被","把","从","向","对","比","里",
        "上","下","中","到","得","为","由","并","又","再","还","已",
        "经","会","能","可","要","想","说","去","来","出","进","起",
        "等","如","若","当","因","但","而","虽","然","如","何","哪",
        "怎","么","什","谁","几","两","多","少","大","小","好","快",
        "新","老","高","低","长","短","真","假","正","反","总","分",
        NULL
    };
    for (int i = 0; voids[i]; i++) {
        if (strcmp(s, voids[i]) == 0) return 1;
    }
    return 0;
}

static int cc_find_edge(ReasoningNode* n, ReasoningNode* target) {
    if (!n || !n->edges) return -1;
    for (int i = 0; i < n->edge_count; i++)
        if (n->edges[i].target == target) return i;
    return -1;
}

typedef struct {
    char word[64];
    int  node_a, node_b;
    float strength;
} CCWordCand;

static int cc_cmp(const void* a, const void* b) {
    float sa = ((const CCWordCand*)a)->strength;
    float sb = ((const CCWordCand*)b)->strength;
    return (sa > sb) ? -1 : (sa < sb) ? 1 : 0;
}

/**
 * 词巩固：扫描字拓扑边权，按相对强度（Hebbian）把高绑定字对
 * 晋升为词节点到概念拓扑，cross-link 连回组成字。
 * 词从语料共现统计自己长出来（边权=共现计数），不依赖外部词典。
 * 返回新建词节点数。调用方不应持有 master->rwlock（内部函数自管锁）。
 */
int autonomic_compound_consolidate(MasterTopology* master) {
    if (!master) return 0;

    /* 全程持写锁：遍历 vocab 边权 + 建词节点期间阻塞其他线程
     * （对话学习/扩散/健康扫描），防 realloc 悬垂导致的 SIGSEGV
     * （v0.6 实测：并发下 gateway 反复崩 139） */
    pthread_rwlock_wrlock(&master->rwlock);

    SubTopology* vocab = NULL;
    SubTopology* concept = NULL;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub) continue;
        if ((int)sub->type == TOPO_VOCABULARY) vocab = sub;
        else if ((int)sub->type == TOPO_CONCEPT) concept = sub;
    }
    if (!vocab || !vocab->net || !concept || !concept->net) return 0;
    if (vocab->net->node_count <= 0) return 0;

    int vn = vocab->net->node_count;
    float* avg_w = (float*)calloc((size_t)vn, sizeof(float));
    if (!avg_w) return 0;

    /* 1. 边权均值（Hebbian 边权 = 共现计数） */
    for (int i = 0; i < vn; i++) {
        ReasoningNode* n = vocab->net->nodes[i];
        if (!n || !n->edges || n->edge_count == 0) continue;
        float sum = 0.0f;
        for (int j = 0; j < n->edge_count; j++) sum += n->edges[j].weight;
        avg_w[i] = sum / (float)n->edge_count;
    }

    /* 2. 候选收集（高频方向定词序） */
    CCWordCand* cands = (CCWordCand*)malloc(sizeof(CCWordCand) * (size_t)(vn * 2));
    if (!cands) { free(avg_w); return 0; }
    int cand_count = 0;

    for (int i = 0; i < vn && cand_count < vn * 2; i++) {
        ReasoningNode* a = vocab->net->nodes[i];
        if (!a || !a->edges || a->edge_count < CC_MIN_EDGES) continue;
        if (avg_w[i] <= 0.001f) continue;
        if (!a->concept || !cc_is_cjk_char(a->concept)) continue;

        for (int j = 0; j < a->edge_count; j++) {
            Edge* e = &a->edges[j];
            if (!e->target || e->weight <= 0.0f) continue;
            ReasoningNode* b = e->target;
            if (b->node_id <= a->node_id) continue;
            if (!b->concept || !cc_is_cjk_char(b->concept)) continue;
            if (b->edge_count < CC_MIN_EDGES) continue;

            /* 虚字过滤：含纯虚字的字对跳过（防"的武/你是/很大"类
             * 虚词噪声占据候选榜，让实义词（三国/关羽）能涌现） */
            if (cc_is_void_char(a->concept) || cc_is_void_char(b->concept)) continue;

            float w_ab = e->weight;   /* A→B（语序方向） */

            /* 反向边（B→A）用于方向判定：若反向更强，词序取 BA */
            int ri = cc_find_edge(b, a);
            float w_ba = (ri >= 0) ? b->edges[ri].weight : 0.0f;

            /* 高频方向 = 词序；相对强度只查高频方向。
             * 双通道（v0.6）：相对强度超阈值（低频字组合如"衣服"）
             * 或绝对强度高（高频实体词如"三国"共现几百次边权饱和到
             * 0.9，但相对强度因高频字 avg 高而不突出）都涌现 */
            int fwd = (w_ab >= w_ba);
            float w_hi = fwd ? w_ab : w_ba;
            float avg_hi = fwd ? avg_w[i] : avg_w[b->node_id];
            if (avg_hi <= 0.001f) continue;
            float rel = w_hi / avg_hi;
            if (rel < g_compound_threshold && w_hi < 0.85f) continue;

            /* 方向确定性：两个方向都强且接近 → 可能是回文/噪声，跳过 */
            float w_lo = fwd ? w_ba : w_ab;
            if (w_lo > 0.0f && w_hi / w_lo < 1.5f) continue;

            CCWordCand* c = &cands[cand_count++];
            c->node_a = a->node_id;
            c->node_b = b->node_id;
            c->strength = rel;
            if (fwd)
                snprintf(c->word, sizeof(c->word), "%s%s", a->concept, b->concept);
            else
                snprintf(c->word, sizeof(c->word), "%s%s", b->concept, a->concept);
        }
    }

    /* 3. 按强度排序 + 限量（防爆炸） */
    qsort(cands, (size_t)cand_count, sizeof(CCWordCand), cc_cmp);
    if (cand_count > CC_MAX_WORDS) cand_count = CC_MAX_WORDS;

    /* 4. 建词节点 + cross-link（master_add_cross_link 内部自管锁） */
    int created = 0;
    for (int k = 0; k < cand_count; k++) {
        CCWordCand* c = &cands[k];
        if (huarong_net_find_concept(concept->net, c->word) >= 0) continue; /* 已存在 */
        ReasoningNode* wn = huarong_net_add_node(concept->net, c->word, NULL, 0);
        if (!wn) continue;
        /* 词节点热值：初值 0 会被 RED 冻结当冷节点删光（实测 343 词
         * 全灭）——新涌现的词是重要知识，给 0.5 热值保活 */
        wn->activation = 0.5f;
        node_hash_add(concept->node_hash, wn);

        float w = 0.3f + c->strength * 0.15f;
        if (w > CC_STRENGTH_CAP) w = CC_STRENGTH_CAP;
        master_add_cross_link_nolock(master, TOPO_CONCEPT, wn->node_id,
                                     TOPO_VOCABULARY, c->node_a, w, "compound");
        master_add_cross_link_nolock(master, TOPO_CONCEPT, wn->node_id,
                                     TOPO_VOCABULARY, c->node_b, w, "compound");
        created++;
    }

    pthread_rwlock_unlock(&master->rwlock);

    free(cands);
    free(avg_w);
    return created;
}

/* v0.5.7: 领域归纳——概念词通过与领域锚点的共现关联，归入
 * 领域种子（跨拓扑建边，不复制节点——防镜像爆炸）。
 * 领域锚点 = 领域标签的中文代表词（概念拓扑里的词节点）。
 * 归纳结果：焦虑→PSYCHOLOGY、发烧→MEDICAL（真正的上位归纳） */
void autonomic_domain_induction(MasterTopology* master) {
    if (!master) return;

    SubTopology *concept = NULL, *domain = NULL;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub) continue;
        if (sub->type == TOPO_CONCEPT) concept = sub;
        else if (sub->type == TOPO_DOMAIN) domain = sub;
    }
    if (!concept || !concept->net || !concept->node_hash) return;
    if (!domain || !domain->net || !domain->node_hash) return;
    if (domain->net->node_count == 0) return;  /* 种子未注入 */

    /* 领域锚点：英文标签 + 中文代表词（概念拓扑里的词） */
    static const struct { const char* label; const char* anchor; } ANCHORS[] = {
        {"PSYCHOLOGY", "心理"}, {"PSYCHOLOGY", "情绪"}, {"PSYCHOLOGY", "焦虑"},
        {"MEDICAL",    "健康"}, {"MEDICAL",    "医疗"}, {"MEDICAL",    "医生"},
        {"EMOTION",    "心情"}, {"EMOTION",    "感情"},
        {"TECH",       "技术"}, {"LIFE",       "生活"},
        {"SCIENCE",    "科学"}, {"HISTORY",    "历史"},
        {"GEOGRAPHY",  "地理"}, {"LAW",        "法律"}, {"ECONOMICS",  "经济"},
    };
    const int N_ANCHORS = (int)(sizeof(ANCHORS) / sizeof(ANCHORS[0]));

    /* 锚点节点缓存（概念拓扑里找代表词） */
    ReasoningNode* anchor_nodes[32];
    for (int a = 0; a < N_ANCHORS; a++)
        anchor_nodes[a] = node_hash_find(concept->node_hash, ANCHORS[a].anchor);

    /* 扫描概念拓扑：词 ↔ 锚点共现边权 → 最强领域 */
    int induced = 0;
    pthread_rwlock_wrlock(&master->rwlock);
    for (int i = 0; i < concept->net->node_count && induced < 3000; i++) {
        ReasoningNode* node = concept->net->nodes[i];
        if (!node || !node->concept || node->edge_count == 0) continue;

        const char* best_label = NULL;
        float best_w = 0.35f;  /* 阈值：共现边权 > 0.35 才算关联 */
        for (int a = 0; a < N_ANCHORS; a++) {
            ReasoningNode* anchor = anchor_nodes[a];
            if (!anchor || anchor == node) continue;
            /* 在 node 的边里找 anchor */
            for (int e = 0; e < node->edge_count; e++) {
                if (node->edges[e].target == anchor && node->edges[e].weight > best_w) {
                    best_w = node->edges[e].weight;
                    best_label = ANCHORS[a].label;
                }
            }
        }
        if (best_label) {
            ReasoningNode* seed = node_hash_find(domain->node_hash, best_label);
            if (seed) {
                master_add_cross_link_nolock(master, TOPO_CONCEPT, node->node_id,
                                             TOPO_DOMAIN, seed->node_id,
                                             best_w, "domain-induction");
                induced++;
            }
        }
    }
    pthread_rwlock_unlock(&master->rwlock);

    if (induced > 0)
        LOG_INFO("[领域归纳] %d 个概念归入领域拓扑", induced);
}
