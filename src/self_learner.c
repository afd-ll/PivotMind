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
#include "error.h"
#include "perception.h"

#include "topology_growth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <time.h>

/* 线程本地 RNG — _Thread_local 替代 static 防止多线程竞态 */
static unsigned int _sl_rng(void) {
    static _Thread_local unsigned int s = 0;
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
        /* v0.5.9: 覆盖「悬垂优先 + 最久未探索」的记录——
         * 保留活跃探索记录（类似巩固度淘汰），顺带回收节点
         * 已冻结/删除的悬垂记录（占位不清理的问题）。 */
        int victim = -1;
        for (int i = 0; i < sl->expl_capacity; i++) {
            SubTopology* sub = NULL;
            for (int t = 0; t < sl->master->sub_topo_count; t++) {
                if (sl->master->sub_topologies[t] &&
                    (int)sl->master->sub_topologies[t]->type == sl->explored[i].topo_id)
                    { sub = sl->master->sub_topologies[t]; break; }
            }
            int exists = 0;
            if (sub && sub->net && sl->explored[i].node_id >= 0 &&
                sl->explored[i].node_id < sub->net->node_count) {
                ReasoningNode* n = sub->net->nodes[sl->explored[i].node_id];
                exists = (n && n->concept) ? 1 : 0;
            }
            if (!exists) { victim = i; break; }  /* 悬垂优先回收 */
        }
        if (victim < 0) {
            float oldest = sl->explored[0].last_explore;
            victim = 0;
            for (int i = 1; i < sl->expl_capacity; i++) {
                if (sl->explored[i].last_explore < oldest) {
                    oldest = sl->explored[i].last_explore;
                    victim = i;
                }
            }
        }
        rec = &sl->explored[victim];
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
/* 过滤垃圾概念：纯ASCII乱码/全大写无元音/全标点等不应作为搜索词 */
static int _is_meaningful_concept(const char* s) {
    if (!s || !s[0]) return 0;
    int cjk = 0, alpha = 0, vowel = 0, punct = 0, upper = 0;
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 0xE0)      { cjk = 1; p += 2; }
        else if (c >= 0xC0) { cjk = 1; p += 1; }
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            alpha++;
            if (c == 'a'||c == 'e'||c == 'i'||c == 'o'||c == 'u'||
                c == 'A'||c == 'E'||c == 'I'||c == 'O'||c == 'U') vowel++;
            if (c >= 'A' && c <= 'Z') upper++;
        }
        else if (c == '/' || c == '\\' || c < 0x20) punct++;
    }
    if (cjk) return 1;                    /* 有中文 → 有效 */
    if (alpha >= 2 && vowel > 0) return 1; /* 有元音的英文词 */
    return 0;                              /* 全辅音/纯数字/标点 → 垃圾 */
}


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
            if (sub->net->nodes[ni] && sub->net->nodes[ni]->concept
                && !sub->net->nodes[ni]->is_cooled) {
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

    /* 入口边界检查：防止采样到的 topo_id 意外越界 */
    if (cur_topo < 0 || cur_topo >= sl->master->sub_topo_count) return 0;

    for (int hop = 0; hop < sl->cfg.walk_depth && len < max_steps; hop++) {
        /* 跨拓扑跳转后可能越界 */
        if (cur_topo < 0 || cur_topo >= sl->master->sub_topo_count) break;

        SubTopology* sub = sl->master->sub_topologies[cur_topo];
        if (!sub || !sub->net || cur_node < 0 || cur_node >= sub->net->node_count) break;
        ReasoningNode* node = sub->net->nodes[cur_node];
        if (!node) break;

        /* 记录当前步（冻节点也记录，但不在其上继续行走） */
        steps[len].topo_id = cur_topo;
        steps[len].node_id = cur_node;
        steps[len].ptr     = node;
        len++;

        /* 冻节点：连接数据已释放不可遍历，记录后终止本路径 */
        if (node->is_cooled) break;

        /* 选择下一跳：偏向低权重的边（探索未知） */
        if (node->edge_count == 0) {
            /* 无内部边 → 尝试跨拓扑跳转 */
            int jumped = 0;
            if (sl->master->cross_adj) {
                int adj = cur_topo * MAX_NODES_PER_TOPO + cur_node;
                if (adj < sl->master->cross_adj_count) {
                    CrossTopoAdjEntry* e = sl->master->cross_adj[adj];
                    while (e) {
                        CrossTopologyLink* l = (sl->master->cross_links
                            && e->link_index >= 0
                            && e->link_index < sl->master->cross_link_count)
                            ? sl->master->cross_links[e->link_index] : NULL;
                        if (l && l->to_topo_id != cur_topo
                            && l->to_topo_id >= 0 && l->to_topo_id < sl->master->sub_topo_count) {
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
            /* 完整性校验：edges 可能为悬垂指针（hash overflow 残留脏数据） */
            if (!node->edges || node->edge_capacity == 0) break;
            float wsum = 0.0f;
            float wbuf[64];
            int nc = node->edge_count < 64 ? node->edge_count : 64;
            for (int i = 0; i < nc && i < node->edge_capacity; i++) {
                float w = node->edges[i].weight;
                wbuf[i] = (1.0f - w + 0.05f);
                if (wbuf[i] < 0.01f) wbuf[i] = 0.01f;
                wsum += wbuf[i];
            }
            if (wsum < 0.001f) break;
            float r = _sl_randf() * wsum;
            float acc = 0.0f;
            int chosen = 0;
            for (int i = 0; i < nc && i < node->edge_capacity; i++) {
                acc += wbuf[i];
                if (r <= acc) { chosen = i; break; }
            }
            /* 三重守卫：edges 非空 + capacity 有效 + target 非空 */
            if (!node->edges || node->edge_capacity == 0
                || chosen >= node->edge_capacity
                || !node->edges[chosen].target) break;
            cur_node = node->edges[chosen].target->node_id;
            /* 图平滑：走过边微量强化 (+0.01)，好路径被随机游走自然加强 */
            {
                int lk = node->node_id & (PM_NODE_LOCK_COUNT - 1);
                pthread_mutex_lock(&sub->net->node_locks[lk]);
                float* pw = &node->edges[chosen].weight;
                *pw += 0.01f;
                if (*pw > 1.0f) *pw = 1.0f;
                pthread_mutex_unlock(&sub->net->node_locks[lk]);
            }
            /* 30% 概率尝试跨拓扑跳转 */
            if (_sl_randf() < 0.3f && sl->master->cross_adj) {
                int adj = cur_topo * MAX_NODES_PER_TOPO + cur_node;
                if (adj < sl->master->cross_adj_count) {
                    CrossTopoAdjEntry* e = sl->master->cross_adj[adj];
                    if (e) {
                        CrossTopologyLink* l = (sl->master->cross_links
                            && e->link_index >= 0
                            && e->link_index < sl->master->cross_link_count)
                            ? sl->master->cross_links[e->link_index] : NULL;
                        if (l && l->to_topo_id != cur_topo
                            && l->to_topo_id >= 0 && l->to_topo_id < sl->master->sub_topo_count) {
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
 *   - 矛盾检测：弱边衰减
 *   - 语义关联：跨拓扑特征相似节点建连接（需过余弦相似度闸门）
 *
 * 注意：同拓扑传递闭包（A→B→C → 建A→C）已移除。
 * 随机游走路径的三元组缺乏语义依据，会造成 O(n²) 边爆炸。
 * 图增长由联网搜索/文章阅读（真实语料）+ 跨拓扑语义链接（质量闸门）驱动。
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

            /* === 语义缺失检测 === */
            /* 跨拓扑：特征相似但无连接 → 创建跨拓扑连接 */
            if (steps[a].topo_id != steps[c].topo_id) {
                /* v0.5.14 fix: 写锁贯穿下不能调带锁版 cross_link_exists（EDEADLK），
                 * 删除外层判重——master_add_cross_link_nolock 内部自带判重
                 * （已存在返回 -1），语义等价且省一次 512 维余弦白算 */
                float sim = 0.0f;
                if (na->features && nc->features &&
                    na->feature_dim > 0 && na->feature_dim == nc->feature_dim) {
                    sim = cosine_similarity(na->features, nc->features, na->feature_dim);
                }
                if (sim > sl->cfg.similarity_threshold) {
                    float init_w = sl->cfg.transitive_boost + sim * 0.15f;
                    if (init_w > 0.6f) init_w = 0.6f;
                    int ret = master_add_cross_link_nolock(sl->master,
                                steps[a].topo_id, na->node_id,
                                steps[c].topo_id, nc->node_id,
                                init_w, "self-learn");
                    if (ret >= 0) {   /* 仅真新建才计数（已存在返回 -1） */
                        sl->total_created++;
                        mods++;
                        LOG_INFO("[自学] 语义关联 %s(%d) ↔ %s(%d) sim=%.2f",
                                 na->concept ? na->concept : "?", steps[a].topo_id,
                                 nc->concept ? nc->concept : "?", steps[c].topo_id, sim);
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
                if (na2 && nb2 && na2->edges) {
                    for (int ci = 0; ci < na2->edge_count; ci++) {
                        if (na2->edges[ci].target == nb2) {
                            float w = (na2->edges ? na2->edges[ci].weight : 0.0f);
                            if (w < 0.1f && w > 0.0f) {
                                /* 极弱边：标记衰减 */
                                int lk = na2->node_id & (PM_NODE_LOCK_COUNT - 1);
                                SubTopology* sub = sl->master->sub_topologies[steps[a].topo_id];
                                if (sub && sub->net) {
                                    pthread_mutex_lock(&sub->net->node_locks[lk]);
                                    na2->edges[ci].weight *= sl->cfg.contradiction_decay;
                                    if (na2->edges[ci].weight < 0.01f)
                                        na2->edges[ci].weight = 0.0f;
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

    /* v0.5.14 fix: 读锁→写锁（08-12/08-13 两次"锁无主死锁"真凶：
     * 持读锁贯穿 cycle → audit_path 内 master_add_cross_link 求写锁
     * → glibc rwlock 同线程升级自死锁。self_learner 是低频后台任务，
     * 写锁贯穿可接受；锁内全部改用 nolock 变体（写锁下不能调带锁版）。 */
    pthread_rwlock_wrlock(&sl->master->rwlock);

    /* v0.5.8: 锁内只收集待搜索概念，网络搜索移到解锁后执行——
     * 持读锁跑 curl（perception_learn_concept）会让主循环 wrlock
     * 饿死 STUCK（08-07 gdb 实锤：自学线程持读锁 usleep 等网络，
     * 主循环 brainstem_loop 卡在 pthread_rwlock_wrlock） */
    char* pend_queries[32];
    int   pend_count = 0;

    sl->cycle_num++;
    int seeds_topo[MAX_SEEDS], seeds_node[MAX_SEEDS];

    /* 1. 好奇心采样 */
    int n = sample_by_curiosity(sl, seeds_topo, seeds_node, sl->cfg.seeds_per_cycle);

    LOG_DEBUG("[自学] 周期#%d 采样%d个起点...", sl->cycle_num, n);

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

        /* 6. 孤立节点 → 锁内只收集概念名，解锁后统一搜索学习 */
        if (len <= 1) {
            SubTopology* sub = sl->master->sub_topologies[seeds_topo[s]];
            if (sub && sub->net && seeds_node[s] < sub->net->node_count) {
                ReasoningNode* node = sub->net->nodes[seeds_node[s]];
                if (node && node->concept && node->edge_count == 0) {
                    extern Perception* g_perception;  /* 由主程序注入的全局感觉皮层 */
                    if (g_perception && _is_meaningful_concept(node->concept) &&
                        pend_count < 32) {
                        pend_queries[pend_count] = strdup(node->concept);
                        if (pend_queries[pend_count]) pend_count++;
                    }
                }
            }
        }
    }

    /* 7. 二次好奇心：对新发现的节点（未探索）做快速审查 */
    {
        SubTopology* vocab = sl->master->sub_topologies[0];
        if (vocab && vocab->net) {
            int second_pass = 0;
            for (int i = 0; i < vocab->net->node_count && second_pass < 3; i++) {
                ReasoningNode* n = vocab->net->nodes[i];
                if (!n || !n->concept) continue;
                /* 跳过已探索或连接数太多（已充分关联）的节点 */
                ExploreRecord* rec = find_record(sl, vocab->topo_id, n->node_id);
                if (rec && rec->explore_count > 0) continue;
                if (n->edge_count > 3) continue;
                /* 对新概念做快速探索 */
                WalkStep steps[16];
                int len = deep_walk(sl, vocab->topo_id, n->node_id, steps, 16);
                if (len >= 3) total_mods += audit_path(sl, steps, len);
                for (int j = 0; j < len; j++)
                    mark_explored(sl, steps[j].topo_id, steps[j].node_id);
                second_pass++;
            }
            if (second_pass > 0)
                LOG_DEBUG("[自学] 二次好奇心: 探索 %d 个新概念", second_pass);
        }
    }

    sl->total_cycles++;
    if (total_mods > 0) {
        LOG_INFO("[自学] 周期#%d完成: %d处修正 (传递:%d 创建:%d 降权:%d)",
                 sl->cycle_num, total_mods,
                 sl->total_transitive, sl->total_created, sl->total_demoted);
    }

    pthread_rwlock_unlock(&sl->master->rwlock);

    /* v0.5.8: 锁外执行网络搜索——改走异步入队（感知 worker 串行执行），
     * 调用方（主循环/自学线程）绝不阻塞网络 */
    {
        extern Perception* g_perception;
        for (int q = 0; q < pend_count; q++) {
            if (g_perception) {
                int r = perception_enqueue_search(g_perception, pend_queries[q]);
                if (r > 0) total_mods++;
            }
            free(pend_queries[q]);
        }
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
