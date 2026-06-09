/**
 * @file self_learner.c
 * @brief 后台自主学习引擎实现
 *
 * 流程：
 *   1. 好奇心采样 → 2. 深度游走 → 3. 知识审查 → 4. 自纠错 → 5. 新奇度更新
 *
 * 线程安全：使用 node_locks 保护节点写操作
 */

#include "self_learner.h"
#include "huarong_topology.h"
#include "node_hash.h"
#include "common.h"
#include "web_search.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <time.h>

/* 线程本地 RNG */
static unsigned int _sl_rng(void) {
    static unsigned int s = 0;
    if (!s) s = (unsigned int)time(NULL) ^ (unsigned int)(uintptr_t)&s;
    s = s * 1103515245 + 12345;
    return (s >> 16) & 0x7FFF;
}

static int _sl_rand(int max) { return max > 0 ? (int)(_sl_rng() % (unsigned)max) : 0; }
static float _sl_randf(void) { return _sl_rng() / 32767.0f; }

/* ================================================================
 *  自主学习器结构
 * ================================================================ */

typedef struct {
    int     topo_id;
    int     node_id;
    int     explore_count;     /* 被探索次数 */
    float   last_explore;      /* 上次探索时间 (cycle#) */
} ExploreRecord;

struct SelfLearner {
    MasterTopology* master;
    SelfLearnerConfig cfg;

    /* 探索记录 — 简单线性表，超出容量时覆盖最旧的 */
    ExploreRecord* explored;
    int expl_capacity;
    int expl_count;
    int expl_next;

    /* 统计 */
    int total_cycles;
    int total_created;
    int total_demoted;
    int total_transitive;
    int cycle_num;  /* 当前周期编号 */
};

/* ================================================================
 *  探索记录管理
 * ================================================================ */

static ExploreRecord* find_record(SelfLearner* sl, int topo_id, int node_id) {
    for (int i = 0; i < sl->expl_count; i++) {
        if (sl->explored[i].topo_id == topo_id && sl->explored[i].node_id == node_id)
            return &sl->explored[i];
    }
    return NULL;
}

static void mark_explored(SelfLearner* sl, int topo_id, int node_id) {
    ExploreRecord* rec = find_record(sl, topo_id, node_id);
    if (rec) {
        rec->explore_count++;
        rec->last_explore = (float)sl->cycle_num;
        return;
    }
    if (sl->expl_count < sl->expl_capacity) {
        rec = &sl->explored[sl->expl_count++];
    } else {
        rec = &sl->explored[sl->expl_next];
        sl->expl_next = (sl->expl_next + 1) % sl->expl_capacity;
    }
    rec->topo_id = topo_id;
    rec->node_id = node_id;
    rec->explore_count = 1;
    rec->last_explore = (float)sl->cycle_num;
}

/**
 * 好奇心评分：未被探索的节点得分高
 * 公式：1.0 / (1 + explore_count) + time_bonus
 */
static float curiosity_score(SelfLearner* sl, int topo_id, int node_id) {
    ExploreRecord* rec = find_record(sl, topo_id, node_id);
    if (!rec) return 1.0f;  /* 从未探索 → 最高好奇心 */
    float age = (float)sl->cycle_num - rec->last_explore;
    return 1.0f / (1.0f + (float)rec->explore_count) + age * 0.001f;
}

/* ================================================================
 *  好奇心采样 — 从所有拓扑中选最"好奇"的节点
 * ================================================================ */

#define MAX_SEEDS 32

static int sample_by_curiosity(SelfLearner* sl, int* out_topo, int* out_node, int max_n) {
    int found = 0;

    /* 两轮采样：先随机粗筛，再按好奇心排序 */
    typedef struct { int topo; int node; float score; } Candidate;
    Candidate pool[128];
    int pool_n = 0;

    for (int t = 0; t < sl->master->sub_topo_count && pool_n < 120; t++) {
        SubTopology* sub = sl->master->sub_topologies[t];
        if (!sub || !sub->net || sub->net->node_count == 0) continue;

        /* 每个拓扑随机采样 ceil(128 / active_topos) 个候选 */
        int per_topo = 128 / sl->master->sub_topo_count + 1;
        for (int s = 0; s < per_topo && pool_n < 128; s++) {
            int ni = _sl_rand(sub->net->node_count);
            if (sub->net->nodes[ni] && sub->net->nodes[ni]->concept) {
                pool[pool_n].topo  = sub->topo_id;
                pool[pool_n].node  = ni;
                pool[pool_n].score = curiosity_score(sl, sub->topo_id, ni);
                pool_n++;
            }
        }
    }

    /* 按好奇心降序排列 */
    for (int i = 0; i < pool_n - 1; i++) {
        for (int j = i + 1; j < pool_n; j++) {
            if (pool[j].score > pool[i].score) {
                Candidate tmp = pool[i]; pool[i] = pool[j]; pool[j] = tmp;
            }
        }
    }

    /* 取前 max_n */
    for (int i = 0; i < pool_n && found < max_n; i++) {
        if (pool[i].score < 0.1f) continue;  /* 已充分探索 */
        out_topo[found] = pool[i].topo;
        out_node[found] = pool[i].node;
        found++;
    }

    return found;
}

/* ================================================================
 *  深度游走 — 在拓扑网络中执行多跳探索
 * ================================================================ */

typedef struct {
    int topo_id;
    int node_id;
    ReasoningNode* ptr;
} WalkStep;

/**
 * 从起点出发，在拓扑网络中执行深度游走
 * 返回路径长度，路径存入 steps[]
 */
static int deep_walk(SelfLearner* sl, int start_topo, int start_node,
                     WalkStep* steps, int max_steps) {
    int len = 0;
    int cur_topo = start_topo;
    int cur_node = start_node;

    for (int hop = 0; hop < sl->cfg.walk_depth && len < max_steps; hop++) {
        SubTopology* sub = sl->master->sub_topologies[cur_topo];
        if (!sub || !sub->net || cur_node >= sub->net->node_count) break;
        ReasoningNode* node = sub->net->nodes[cur_node];
        if (!node) break;

        /* 记录当前步 */
        steps[len].topo_id = cur_topo;
        steps[len].node_id = cur_node;
        steps[len].ptr     = node;
        len++;

        /* 选择下一跳：偏向低权重的边（探索未知） */
        if (node->connection_count == 0) {
            /* 无内部边 → 尝试跨拓扑跳转 */
            int jumped = 0;
            if (sl->master->cross_adj) {
                int adj = cur_topo * MAX_NODES_PER_TOPO + cur_node;
                if (adj < sl->master->cross_adj_count) {
                    CrossTopoAdjEntry* e = sl->master->cross_adj[adj];
                    while (e) {
                        CrossTopologyLink* l = (e->link_index < sl->master->cross_link_count)
                            ? sl->master->cross_links[e->link_index] : NULL;
                        if (l && l->to_topo_id != cur_topo) {
                            cur_topo = l->to_topo_id;
                            cur_node = l->to_node_id;
                            jumped = 1;
                            break;
                        }
                        e = e->next;
                    }
                }
            }
            if (!jumped) break;
        } else {
            /* 偏弱边选择（同梦境引擎的反向权重逻辑） */
            float wsum = 0.0f;
            float wbuf[64];
            int nc = node->connection_count < 64 ? node->connection_count : 64;
            for (int i = 0; i < nc; i++) {
                float w = node->connection_weights ? node->connection_weights[i] : 0.5f;
                wbuf[i] = (1.0f - w + 0.05f);
                if (wbuf[i] < 0.01f) wbuf[i] = 0.01f;
                wsum += wbuf[i];
            }
            if (wsum < 0.001f) break;
            float r = _sl_randf() * wsum;
            float acc = 0.0f;
            int chosen = 0;
            for (int i = 0; i < nc; i++) {
                acc += wbuf[i];
                if (r <= acc) { chosen = i; break; }
            }
            cur_node = node->connections[chosen]->node_id;
            /* 30% 概率尝试跨拓扑跳转 */
            if (_sl_randf() < 0.3f && sl->master->cross_adj) {
                int adj = cur_topo * MAX_NODES_PER_TOPO + cur_node;
                if (adj < sl->master->cross_adj_count) {
                    CrossTopoAdjEntry* e = sl->master->cross_adj[adj];
                    if (e) {
                        CrossTopologyLink* l = (e->link_index < sl->master->cross_link_count)
                            ? sl->master->cross_links[e->link_index] : NULL;
                        if (l && l->to_topo_id != cur_topo) {
                            cur_topo = l->to_topo_id;
                            cur_node = l->to_node_id;
                        }
                    }
                }
            }
        }
    }
    return len;
}

/* ================================================================
 *  知识审查 + 自纠错
 * ================================================================ */

/**
 * 审查游走路径并执行自动修正：
 *   - 传递性：A→B→C 但没有 A→C → 新建 A→C（低权重）
 *   - 矛盾：暂简化实现（TODO: 需要否定概念检测）
 *   - 语义：相似节点无连接 → 建议连接
 */
static int audit_path(SelfLearner* sl, WalkStep* steps, int len) {
    int mods = 0;

    for (int a = 0; a < len - 2; a++) {
        for (int c = a + 2; c < len; c++) {
            int b = a + 1;

            ReasoningNode* na = steps[a].ptr;
            ReasoningNode* nb = steps[b].ptr;
            ReasoningNode* nc = steps[c].ptr;
            if (!na || !nb || !nc || na == nc || nb == nc) continue;

            /* === 传递性检测 === */
            /* A→B 存在 (在路径中) + B→C 存在 (在路径中) + A→C 不存在 → 创建 A→C */
            if (steps[a].topo_id == steps[c].topo_id) {
                /* 同拓扑内的传递检测 */
                int a_to_c = 0;
                for (int ci = 0; ci < na->connection_count; ci++) {
                    if (na->connections[ci] == nc) { a_to_c = 1; break; }
                }
                if (!a_to_c) {
                    SubTopology* sub = sl->master->sub_topologies[steps[a].topo_id];
                    if (sub && sub->net) {
                        huarong_net_add_connection(sub->net, na->node_id,
                            nc->node_id, sl->cfg.transitive_boost);
                        sl->total_transitive++;
                        mods++;
                        if (sl->cfg.verbose) {
                            fprintf(stderr, "[自学] 传递性: %s→%s→%s → 新建 %s→%s\n",
                                    na->concept, nb->concept, nc->concept,
                                    na->concept, nc->concept);
                        }
                    }
                }
            }

            /* === 语义缺失检测 === */
            /* 跨拓扑：特征相似但无连接 → 创建跨拓扑连接 */
            if (steps[a].topo_id != steps[c].topo_id) {
                if (!cross_link_exists(sl->master, steps[a].topo_id, na->node_id,
                                       steps[c].topo_id, nc->node_id)) {
                    float sim = 0.0f;
                    if (na->features && nc->features &&
                        na->feature_dim > 0 && na->feature_dim == nc->feature_dim) {
                        sim = cosine_similarity(na->features, nc->features, na->feature_dim);
                    }
                    if (sim > sl->cfg.similarity_threshold) {
                        float init_w = sl->cfg.transitive_boost + sim * 0.15f;
                        if (init_w > 0.6f) init_w = 0.6f;
                        master_add_cross_link(sl->master,
                            steps[a].topo_id, na->node_id,
                            steps[c].topo_id, nc->node_id,
                            init_w, "self-learn");
                        sl->total_created++;
                        mods++;
                        if (sl->cfg.verbose) {
                            fprintf(stderr, "[自学] 语义关联 %s(%d) ↔ %s(%d) sim=%.2f\n",
                                    na->concept, steps[a].topo_id,
                                    nc->concept, steps[c].topo_id, sim);
                        }
                    }
                }
            }
        }

        /* === 矛盾检测（简化版） === */
        /* 检查每对相邻节点间是否存在极弱边 */
        {
            int b2 = a + 1;
            if (b2 < len) {
                ReasoningNode* na2 = steps[a].ptr;
                ReasoningNode* nb2 = steps[b2].ptr;
                if (na2 && nb2) {
                    for (int ci = 0; ci < na2->connection_count; ci++) {
                        if (na2->connections[ci] == nb2) {
                            float w = na2->connection_weights ? na2->connection_weights[ci] : 0.0f;
                            if (w < 0.1f && w > 0.0f) {
                                /* 极弱边：标记衰减 */
                                int lk = na2->node_id & (PM_NODE_LOCK_COUNT - 1);
                                SubTopology* sub = sl->master->sub_topologies[steps[a].topo_id];
                                if (sub && sub->net) {
                                    pthread_mutex_lock(&sub->net->node_locks[lk]);
                                    na2->connection_weights[ci] *= sl->cfg.contradiction_decay;
                                    if (na2->connection_weights[ci] < 0.01f)
                                        na2->connection_weights[ci] = 0.0f;
                                    pthread_mutex_unlock(&sub->net->node_locks[lk]);
                                    sl->total_demoted++;
                                    mods++;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return mods;
}

/* ================================================================
 *  主循环
 * ================================================================ */

int self_learner_cycle(SelfLearner* sl) {
    if (!sl || !sl->master) return 0;

    sl->cycle_num++;
    int seeds_topo[MAX_SEEDS], seeds_node[MAX_SEEDS];

    /* 1. 好奇心采样 */
    int n = sample_by_curiosity(sl, seeds_topo, seeds_node, sl->cfg.seeds_per_cycle);

    if (sl->cfg.verbose) {
        fprintf(stderr, "[自学] 周期#%d 采样%d个起点...\n", sl->cycle_num, n);
    }

    int total_mods = 0;

    /* 2-4. 对每个种子：游走 → 审查 → 修正 */
    for (int s = 0; s < n; s++) {
        WalkStep steps[16];
        int len = deep_walk(sl, seeds_topo[s], seeds_node[s], steps, 16);

        if (len >= 3) {
            total_mods += audit_path(sl, steps, len);
        }

        /* 5. 标记已探索 */
        for (int i = 0; i < len; i++) {
            mark_explored(sl, steps[i].topo_id, steps[i].node_id);
        }

        /* 6. 孤立节点 → 联网搜索学习 */
        if (len <= 1) {
            SubTopology* sub = sl->master->sub_topologies[seeds_topo[s]];
            if (sub && sub->net && seeds_node[s] < sub->net->node_count) {
                ReasoningNode* node = sub->net->nodes[seeds_node[s]];
                if (node && node->concept && node->connection_count == 0) {
                    /* 构建搜索URL：使用简单的搜索引擎 */
                    char search_url[1024];
                    /* 对中文词做URL编码简化处理 */
                    snprintf(search_url, sizeof(search_url),
                        "http://www.baidu.com/s?wd=%s&rn=1",
                        node->concept);

                    WebResult* wr = web_search(search_url, 5000, 32768);
                    if (wr && wr->keyword_count > 0) {
                        SubTopology* vocab = sl->master->sub_topologies[0];
                        if (vocab && vocab->net && vocab->node_hash) {
                            int learned = 0;
                            for (int k = 0; k < wr->keyword_count && learned < 5; k++) {
                                if (!wr->keywords[k] || strlen(wr->keywords[k]) < 2) continue;
                                /* 跳过纯数字/英文短词 */
                                int is_text = 0;
                                for (const char* cp = wr->keywords[k]; *cp; cp++)
                                    if ((unsigned char)*cp > 127) { is_text = 1; break; }
                                if (!is_text) continue;

                                ReasoningNode* exist = node_hash_find(vocab->node_hash, wr->keywords[k]);
                                if (!exist) {
                                    ReasoningNode* new_n = huarong_net_add_node(
                                        vocab->net, wr->keywords[k], NULL, 0);
                                    if (new_n) {
                                        new_n->confidence = 0.3f;
                                        node_hash_add(vocab->node_hash, new_n);
                                        huarong_net_add_connection(vocab->net,
                                            node->node_id, new_n->node_id, 0.25f);
                                        learned++;
                                        total_mods++;
                                    }
                                } else if (exist != node) {
                                    int already = 0;
                                    for (int c = 0; c < node->connection_count; c++)
                                        if (node->connections[c] == exist) { already = 1; break; }
                                    if (!already) {
                                        huarong_net_add_connection(vocab->net,
                                            node->node_id, exist->node_id, 0.3f);
                                        total_mods++;
                                    }
                                }
                            }
                            if (sl->cfg.verbose && learned > 0) {
                                fprintf(stderr, "[自学] 联网学习 '%s' → 新增%d个关联概念\n",
                                        node->concept, learned);
                            }
                        }
                    }
                    web_result_free(wr);
                }
            }
        }
    }

    sl->total_cycles++;
    if (sl->cfg.verbose && total_mods > 0) {
        fprintf(stderr, "[自学] 周期#%d完成: %d处修正 (传递:%d 创建:%d 降权:%d)\n",
                sl->cycle_num, total_mods,
                sl->total_transitive, sl->total_created, sl->total_demoted);
    }

    return total_mods;
}

/* ================================================================
 *  生命周期
 * ================================================================ */

SelfLearner* self_learner_create(MasterTopology* master,
                                  SelfLearnerConfig* config) {
    if (!master) return NULL;

    SelfLearner* sl = (SelfLearner*)calloc(1, sizeof(SelfLearner));
    if (!sl) return NULL;

    sl->master = master;
    if (config) sl->cfg = *config;
    else { SelfLearnerConfig d = SELF_LEARNER_DEFAULT_CONFIG; sl->cfg = d; }

    sl->expl_capacity = sl->cfg.max_explore_history;
    sl->explored = (ExploreRecord*)calloc(sl->expl_capacity, sizeof(ExploreRecord));
    if (!sl->explored) { free(sl); return NULL; }

    return sl;
}

void self_learner_destroy(SelfLearner* sl) {
    if (!sl) return;
    free(sl->explored);
    free(sl);
}

void self_learner_stats(SelfLearner* sl, int* total_cycles,
                        int* total_created, int* total_demoted,
                        int* total_transitive) {
    if (!sl) return;
    if (total_cycles)    *total_cycles    = sl->total_cycles;
    if (total_created)   *total_created   = sl->total_created;
    if (total_demoted)   *total_demoted   = sl->total_demoted;
    if (total_transitive) *total_transitive = sl->total_transitive;
}
