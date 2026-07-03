/**
 * @file brainstem.c
 * @brief 脑干节律控制器实现 — 数字生命的心跳
 *
 * 独立 pthread 线程，维持系统基础节律。
 * 所有跨脑区通信通过丘脑(Thalamus)信号总线完成。
 */

#include "brainstem.h"
#include "health_monitor.h"
#include "huarong_topology.h"
#include "dmn.h"
#include "node_cache.h"
#include "thalamus.h"
#include "perception.h"
#include "amygdala.h"
#include "hippocampus.h"
#include "cerebellum.h"
#include "reticular.h"
#include "self_learner.h"
#include "topology_brain.h"
#include "topology_growth.h"
#include "broca.h"
#include "hypothalamus.h"
#include "visual_cortex.h"    /* v0.5 视觉皮层 — 多模态感知 */
#include "error.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define msleep(ms) Sleep(ms)
#else
#define msleep(ms) usleep((ms) * 1000)
#endif

/* ================================================================
 *  昼夜间辅助函数（抽离 brainstem_loop 中的时间相关逻辑）
 * 内存读取已交由 include/platform.h 中的 pm_get_process_memory()
 * ================================================================ */

static const CognitiveState BASELINE_STATE = {
    .drive_curiosity  = 0.5f,
    .drive_hunger     = 0.3f,
    .drive_social     = 0.5f,
    .drive_comfort    = 0.5f,
    .emotion_pleasure = 0.5f,
    .emotion_pain     = 0.1f,
    .emotion_security = 0.6f,
    .valence          = 0.0f,
    .current_goal     = NULL,
    .goal_strength    = 0.0f,
    .plan_step        = 0,
    .explore_rate     = 0.5f
};

static unsigned int local_rand(unsigned int* seed) {
    *seed = *seed * 1103515245 + 12345;
    return (*seed >> 16) & 0x7FFF;
}

static void drift_cognitive_state(Brainstem* bs) {
    CognitiveState* state = bs->cognitive_state;
    if (!state) return;

    const float keep = 0.995f;
    const float pull = 0.005f;

    state->drive_curiosity = state->drive_curiosity * keep + BASELINE_STATE.drive_curiosity * pull;
    state->drive_hunger    = state->drive_hunger    * keep + BASELINE_STATE.drive_hunger    * pull;
    state->drive_social    = state->drive_social    * keep + BASELINE_STATE.drive_social    * pull;
    state->drive_comfort   = state->drive_comfort   * keep + BASELINE_STATE.drive_comfort   * pull;

    state->emotion_pleasure = state->emotion_pleasure * keep + BASELINE_STATE.emotion_pleasure * pull;
    state->emotion_pain     = state->emotion_pain     * keep + BASELINE_STATE.emotion_pain     * pull;
    state->emotion_security = state->emotion_security * keep + BASELINE_STATE.emotion_security * pull;
    /* 注：effidence 和 explore_rate 已由杏仁核(amygdala_tick)从情绪拓扑采样更新 */
}

static float circadian_activity(int hour) {
    double rad = (hour - 6) * 3.141592653589793 / 12.0;
    double raw = (sin(rad) + 1.0) / 2.0;
    return 0.1f + (float)raw * 0.9f;
}

static int current_hour(time_t t) {
    struct tm tm_info;
#ifdef _WIN32
    localtime_s(&tm_info, &t);
#else
    localtime_r(&t, &tm_info);
#endif
    return tm_info.tm_hour;
}

/* ================================================================
 *  循环迭代子函数（从 brainstem_loop 中拆分）
 * ================================================================ */

/* 昼夜节律参数包 */
typedef struct {
    float circadian;
    int   hour;
    int   actual_tick_ms;
    float actual_decay;
    float actual_spontaneous;
    int   dream_interval;
    int   selflearn_interval;
} CircadianParams;

/* 计算昼夜节律参数 */
static CircadianParams brainstem_compute_circadian(Brainstem* bs) {
    CircadianParams cp;
    bs->current_time = time(NULL);
    cp.hour = current_hour(bs->current_time);
    cp.circadian = circadian_activity(cp.hour);
    cp.actual_tick_ms       = (int)(bs->tick_interval_ms * (1.4f - 0.4f * cp.circadian));
    cp.actual_decay         = bs->decay_per_tick + (1.0f - cp.circadian) * 0.03f;
    cp.actual_spontaneous   = bs->spontaneous_prob * cp.circadian;
    cp.dream_interval       = (int)(60.0f * (1.8f - 0.8f * cp.circadian));
    cp.selflearn_interval   = (int)(120.0f * (2.0f - 1.0f * cp.circadian));
    return cp;
}

/* ── 昼夜节律阶段名称统一接口 ──
 * 消除 brainstem_tick_log_hour() 和 brainstem_tick_subcortical()
 * 中两处硬编码的阶段名称转换。 */
static const char* brainstem_circadian_phase_name(int hour, int lang) {
    if (hour >= 0 && hour < 6)   return (lang == 0) ? "sleep"    : "沉睡";
    if (hour >= 6 && hour < 10)  return (lang == 0) ? "waking"   : "苏醒";
    if (hour >= 10 && hour < 14) return (lang == 0) ? "active"   : "活跃";
    if (hour >= 14 && hour < 18) return (lang == 0) ? "afternoon": "午后";
    if (hour >= 18 && hour < 22) return (lang == 0) ? "evening"  : "次活跃";
    return                       (lang == 0) ? "winding"  : "入眠";
}

/* 昼夜节律日志 */
static void brainstem_tick_log_hour(Brainstem* bs, const CircadianParams* cp) {
    static int last_logged_hour = -1;
    if (cp->hour != last_logged_hour && bs->verbose) {
        const char* phase = brainstem_circadian_phase_name(cp->hour, 1);
        LOG_INFO("[脑干] %02d:00 -> %s期 (activity=%.2f)", cp->hour, phase, cp->circadian);
        last_logged_hour = cp->hour;
    }
}

/* 睡眠等待 */
static void brainstem_tick_sleep(Brainstem* bs, const CircadianParams* cp) {
    int slept = 0;
    while (slept < cp->actual_tick_ms && __atomic_load_n(&bs->is_running, __ATOMIC_SEQ_CST)) {
        msleep(100);
        slept += 100;
    }
}

/* 稀疏衰减 + 自发激活（分段持锁，防写锁饥饿） */
#define DECAY_BATCH_SIZE 200  /* 每批处理节点数后释放读锁，给写锁留窗口 */
static void brainstem_tick_decay_spontaneous(Brainstem* bs, const CircadianParams* cp) {
    MasterTopology* master = bs->master;
    float total_nodes = 0.0f;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (sub && sub->net) total_nodes += (float)sub->net->node_count;
    }

    int sample_count = (int)(total_nodes * 0.05f * cp->circadian);
    if (sample_count < 100) sample_count = 100;

    /* 稀疏衰减 — 分批执行，每批后释放 master 读锁 */
    {
    int decayed_total = 0;
    while (decayed_total < sample_count) {
        int batch = sample_count - decayed_total;
        if (batch > DECAY_BATCH_SIZE) batch = DECAY_BATCH_SIZE;

        pthread_rwlock_rdlock(&master->rwlock);

        int batch_done = 0;
        for (int t = 0; t < master->sub_topo_count && batch_done < batch; t++) {
            SubTopology* sub = master->sub_topologies[t];
            if (!sub || !sub->net || sub->net->node_count == 0) continue;
            HuarongTopologyNet* net = sub->net;
            float sub_ratio = (float)net->node_count / total_nodes;
            int sub_samples = (int)(batch * sub_ratio) + 1;
            if (sub_samples > net->node_count) sub_samples = net->node_count;
            if (batch_done + sub_samples > batch) sub_samples = batch - batch_done;

            for (int s = 0; s < sub_samples; s++) {
                int idx = local_rand(&bs->_rng_seed) % net->node_count;
                ReasoningNode* node = net->nodes[idx];
                if (!node || node->is_cooled) continue;

                int lock_idx = node->node_id & (PM_NODE_LOCK_COUNT - 1);
                pthread_mutex_lock(&net->node_locks[lock_idx]);
                if (node->activation <= PM_CLOCK_ACTIVATION_FLOOR) {
                    node->activation = 0.0f;
                } else {
                    node->activation *= cp->actual_decay;
                    if (node->activation < PM_CLOCK_ACTIVATION_FLOOR) {
                        node->activation = 0.0f;
                    }
                }
                pthread_mutex_unlock(&net->node_locks[lock_idx]);
                batch_done++;
                decayed_total++;
            }
        }

        pthread_rwlock_unlock(&master->rwlock);
    }
    }

    /* 自发激活 — 数量少，仍一次性执行 */
    pthread_rwlock_rdlock(&master->rwlock);
    {
    float expected = total_nodes * cp->actual_spontaneous;
    int num_spontaneous = (int)expected;
    if ((float)local_rand(&bs->_rng_seed) / 32767.0f < (expected - num_spontaneous)) {
        num_spontaneous++;
    }
    for (int s = 0; s < num_spontaneous; s++) {
        int topo_idx = local_rand(&bs->_rng_seed) % master->sub_topo_count;
        SubTopology* sub = master->sub_topologies[topo_idx];
        if (!sub || !sub->net || sub->net->node_count == 0) continue;
        int ni = local_rand(&bs->_rng_seed) % sub->net->node_count;
        ReasoningNode* rn = sub->net->nodes[ni];
        if (!rn || rn->is_cooled) continue;

        int lock_idx = rn->node_id & (PM_NODE_LOCK_COUNT - 1);
        pthread_mutex_lock(&sub->net->node_locks[lock_idx]);
        float added = bs->spontaneous_strength * (0.5f + local_rand(&bs->_rng_seed) / 65535.0f);
        rn->activation += added;
        if (rn->activation > 1.0f) rn->activation = 1.0f;
        pthread_mutex_unlock(&sub->net->node_locks[lock_idx]);
    }
    }
    pthread_rwlock_unlock(&master->rwlock);
}

/* 堆监控 */
static void brainstem_tick_monitoring(Brainstem* bs) {
    if (bs->tick_count % 30 != 0) return;
    long rss_kb = 0, vsz_kb = 0;
    pm_get_process_memory(&rss_kb, &vsz_kb);
    int total_nodes = 0, total_conns = 0;
    for (int t = 0; t < bs->master->sub_topo_count; t++) {
        SubTopology* sub = bs->master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        total_nodes += sub->net->node_count;
        for (int i = 0; i < sub->net->node_count; i++)
            if (sub->net->nodes[i])
                total_conns += sub->net->nodes[i]->edge_count;
    }
    LOG_INFO("[堆监控] tick=%d 节点=%d 连接=%d RSS=%.1fMB VSZ=%.1fMB",
        bs->tick_count, total_nodes, total_conns,
        rss_kb / 1024.0f, vsz_kb / 1024.0f);
}

/* 皮层下子系统：小脑 + 内感受 + 网状结构 + 丘脑调度 */
static void brainstem_tick_subcortical(Brainstem* bs, const CircadianParams* cp) {
    Thalamus* th = bs->thalamus;
    if (!th) return;

    if (bs->tick_count % 10 == 0 && thalamus_is_region_enabled(th, THAL_CEREBELLUM)) {
        Cerebellum* cb = (Cerebellum*)thalamus_get_region(th, THAL_CEREBELLUM);
        if (cb) {
            float mem_gb = pm_get_rss_mb() / 1024.0f;
            float load = 0;
            pm_get_load(&load);
            load *= 16.67f;
            float cb_protect = cerebellum_tick(cb, load, mem_gb, cp->circadian);
            thalamus_set_cerebellum_protect(th, cb_protect);
        }
    }

    if (bs->health_monitor && bs->tick_count % 120 == 0) {
        CognitiveController* cc = (CognitiveController*)
            thalamus_get_utility(th, THAL_UTIL_COGNITIVE_CTRL);
        health_monitor_tick(bs->health_monitor, bs->master, cc);
    }

    if (bs->tick_count % 60 == 0) {
        reticular_attend(bs->master, 50);
    }

    if (bs->tick_count % 30 == 0) {
        thalamus_set_circadian(th, cp->circadian,
            brainstem_circadian_phase_name(cp->hour, 0));
        thalamus_tick(th);

        /* 下丘脑：需求/动机调控 — 与丘脑调度同频 */
        if (thalamus_is_region_enabled(th, THAL_HYPOTHALAMUS)) {
            Hypothalamus* hypo = (Hypothalamus*)thalamus_get_region(th, THAL_HYPOTHALAMUS);
            if (hypo) {
                hypothalamus_set_circadian(hypo, cp->circadian);
                hypothalamus_tick(hypo);
            }
        }
    }
}

/* 感觉皮层 + 杏仁核 — 每 tick 触发，60s 内必搜一次 */
static int brainstem_tick_perception(Brainstem* bs) {
    Thalamus* th = bs->thalamus;
    if (!th) return 0;
    if (!thalamus_is_region_enabled(th, THAL_PERCEPTION)) return 0;
    int work = 0;
    float p_throttle = thalamus_get_throttle(th, THAL_PERCEPTION);
    Perception* p = (Perception*)thalamus_get_region(th, THAL_PERCEPTION);
    if (p) work = perception_tick(p, p_throttle);

    /* 每小时搜一次 Bing 新闻头条（3600 ticks ≈ 1h） */
    if (p && bs->tick_count % 3600 == 0) {
        perception_search_news(p);
    }

    Amygdala* amy = (Amygdala*)thalamus_get_region(th, THAL_AMYGDALA);
    if (amy) amygdala_tick(amy);

    /* v0.5 视觉皮层 — 每 tick 检查任务队列，按 throttle 限速处理 */
    if (thalamus_is_region_enabled(th, THAL_VISUAL_CORTEX)) {
        VisualCortex* vc = (VisualCortex*)thalamus_get_region(th, THAL_VISUAL_CORTEX);
        if (vc) {
            float vc_throttle = thalamus_get_throttle(th, THAL_VISUAL_CORTEX);
            int vc_work = visual_cortex_tick(vc, vc_throttle);
            if (vc_work > 0 && bs->verbose)
                printf("[脑干] 视觉皮层本轮处理 %d 个文件\n", vc_work);
        }
    }

    return work;
}

/* 突触缩放（每600tick≈10min） */
static void brainstem_tick_synapse_scale(Brainstem* bs) {
    if (bs->tick_count % 600 != 0) return;
    int decayed = 0, released = 0;
    for (int t = 0; t < bs->master->sub_topo_count; t++) {
        SubTopology* sub = bs->master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        for (int i = 0; i < sub->net->node_count; i++) {
            ReasoningNode* node = sub->net->nodes[i];
            if (!node) continue;
            int lock_idx = node->node_id & (PM_NODE_LOCK_COUNT - 1);
            pthread_mutex_lock(&sub->net->node_locks[lock_idx]);
            int kept = 0;
            for (int c = 0; c < node->edge_count; c++) {
                float w = (node->edges ? node->edges[c].weight : 0.5f);
                float conf = (node->edges ? node->edges[c].confidence : 0.5f);
                float strength = w * conf;
                if (strength < 0.001f) { released++; continue; }
                if (strength < 0.01f) { node->edges[c].weight *= 0.5f; decayed++; }
                else if (strength < 0.05f) { node->edges[c].weight *= 0.9f; decayed++; }
                if (kept != c) {
                    node->edges[kept].target = node->edges[c].target;
                    node->edges[kept].weight = node->edges[c].weight;
                    node->edges[kept].motivational_bias = node->edges[c].motivational_bias;
                    node->edges[kept].confidence = node->edges[c].confidence;
                }
                kept++;
            }
            node->edge_count = kept;
            /* 边压缩后重建 conn_hash：先建新的再 swap，旧哈希延迟释放防 use-after-free */
            {
                int hash_cap = (kept > 0) ? 16 : 0;
                while (hash_cap < kept * 2) hash_cap *= 2;
                ConnHashEntry* new_hash = NULL;
                if (hash_cap > 0) {
                    new_hash = (ConnHashEntry*)calloc(hash_cap, sizeof(ConnHashEntry));
                }
                /* swap 旧 conn_hash → 新 */
                void* old_hash = node->conn_hash;
                node->conn_hash = new_hash;
                node->conn_hash_mask = new_hash ? (hash_cap - 1) : -1;
                node->conn_hash_entries = kept;
                /* 旧哈希入退役链表延迟释放 */
                if (old_hash)
                    huarong_net_retire_blob(sub->net, old_hash);
                /* 填充新哈希 */
                for (int ci = 0; ci < kept; ci++) {
                    if (node->edges[ci].target)
                        node_conn_hash_insert(NULL, node, node->edges[ci].target, ci);
                }
            }
            pthread_mutex_unlock(&sub->net->node_locks[lock_idx]);
        }
    }
    if (decayed > 0 || released > 0)
        LOG_INFO("[突触缩放] 衰减%d条 释放%d条", decayed, released);

    /* 跨拓扑连接质量重评估：每 600 tick 更新 transfer_rate */
    if (bs->master) {
        master_reevaluate_cross_links(bs->master, 10.0f);
    }
}

/* 海马体巩固 + DMN梦境 */
static int brainstem_tick_memory_dream(Brainstem* bs, const CircadianParams* cp) {
    Thalamus* th = bs->thalamus;
    int dmn_work = 0;

    if (th && thalamus_is_region_enabled(th, THAL_HIPPOCAMPUS) && bs->tick_count % bs->consolidate_every_n_ticks == 0) {
        Hippocampus* hc = (Hippocampus*)thalamus_get_region(th, THAL_HIPPOCAMPUS);
        if (hc) hippocampus_consolidate(hc);
    }

    if (bs->tick_count % cp->dream_interval == 0) {
        DmnConfig dmn_cfg = DMN_DEFAULT_CONFIG;
        dmn_cfg.verbose = bs->verbose;
        float dmn_throttle = th ? thalamus_get_throttle(th, THAL_DMN) : 1.0f;
        if (!th || thalamus_is_region_enabled(th, THAL_DMN))
            dmn_work = dmn_cycle(bs->master, bs->memory, &dmn_cfg, dmn_throttle);
    }
    return dmn_work;
}

/* 自主学习 + 脑区索引扫描 */
static void brainstem_tick_learning_scan(Brainstem* bs, const CircadianParams* cp) {
    Thalamus* th = bs->thalamus;
    if (!th) return;

    if (bs->tick_count % cp->selflearn_interval == 0) {
        SelfLearner* sl = (SelfLearner*)thalamus_get_utility(th, THAL_UTIL_SELF_LEARNER);
        if (sl) {
            int mods = self_learner_cycle(sl);
            if (mods > 0 && bs->verbose)
                LOG_INFO("[自主学习] 本轮修改 %d 条边", mods);
        }
    }

    if (bs->tick_count % 600 == 0) {
        TopologyBrain* tb = (TopologyBrain*)thalamus_get_utility(th, THAL_UTIL_TOPO_BRAIN);
        if (tb) {
            int mig = topobrain_scan(tb, bs->master);
            if (mig > 0 && bs->verbose)
                LOG_INFO("[脑区索引] 本轮 %d 个节点发生脑区迁移", mig);
        }
    }

    /* 定期拓扑扩容检查 + 模板构建 */
    if (bs->tick_count % 30 == 0) {
        for (int t = 0; t < bs->master->sub_topo_count; t++) {
            if (check_growth_needed(bs->master, t)) {
                int new_cap = auto_extend_topology(bs->master, t);
                if (new_cap > 0 && bs->verbose)
                    LOG_INFO("[脑干] 拓扑%d 扩容至 %d", t, new_cap);
            }
        }
    }

    /* 布罗卡区：委托给 Broca 自己的 tick 调度（不再硬编码 interval） */
    if (thalamus_is_region_enabled(th, THAL_BROCA)) {
        Broca* broca = (Broca*)thalamus_get_region(th, THAL_BROCA);
        if (broca) {
            int built = broca_tick(broca);
            if (built > 0 && bs->verbose)
                LOG_INFO("[脑干] 模板自构建完成: %d 个", built);
        }
    }
}

/* 冷节点冻结 */
static void brainstem_tick_freeze(Brainstem* bs, const CircadianParams* cp) {
    int freeze_interval = (int)(600.0f * (2.0f - 1.0f * cp->circadian));
    if (bs->tick_count % freeze_interval != 0) return;
    Thalamus* th = bs->thalamus;
    if (!th) return;
    NodeCache* nc = (NodeCache*)thalamus_get_utility(th, THAL_UTIL_NODE_CACHE);
    if (!nc) return;

    int frozen = 0;
    for (int t = 0; t < bs->master->sub_topo_count && frozen < 10; t++) {
        SubTopology* sub = bs->master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        pthread_rwlock_rdlock(&sub->rwlock);
        for (int attempt = 0; attempt < 50 && frozen < 10; attempt++) {
            int idx = local_rand(&bs->_rng_seed) % sub->net->node_count;
            ReasoningNode* node = sub->net->nodes[idx];
            if (!node || node->is_cooled) continue;
            if (node->edge_count > 0 && node->activation < 0.005f) {
                node_cache_freeze(nc, sub->net, node);
                frozen++;
            }
        }
        pthread_rwlock_unlock(&sub->rwlock);
    }
    if (frozen > 0 && bs->verbose) {
        LOG_INFO("[脑干] 本轮冻结 %d 个冷节点", frozen);
    }
}

static void* brainstem_loop(void* arg) {
    Brainstem* bs = (Brainstem*)arg;
    Thalamus* th = bs->thalamus;

    if (bs->verbose) LOG_INFO("[脑干] 心跳启动, tick=%dms", bs->tick_interval_ms);

    while (__atomic_load_n(&bs->is_running, __ATOMIC_SEQ_CST)) {
        CircadianParams cp = brainstem_compute_circadian(bs);
        brainstem_tick_log_hour(bs, &cp);
        brainstem_tick_sleep(bs, &cp);
        if (!__atomic_load_n(&bs->is_running, __ATOMIC_SEQ_CST)) break;

        bs->tick_count++;

        if (!__atomic_load_n(&bs->is_running, __ATOMIC_SEQ_CST)) break;
        brainstem_tick_decay_spontaneous(bs, &cp);
        if (!__atomic_load_n(&bs->is_running, __ATOMIC_SEQ_CST)) break;
        brainstem_tick_monitoring(bs);
        drift_cognitive_state(bs);
        brainstem_tick_subcortical(bs, &cp);
        int percept_work = brainstem_tick_perception(bs);
        brainstem_tick_synapse_scale(bs);
        int dmn_work = brainstem_tick_memory_dream(bs, &cp);
        /* 学习扫描含模板自构建（O(N×M)），关机时跳过避免长时间阻塞 pthread_join */
        if (__atomic_load_n(&bs->is_running, __ATOMIC_SEQ_CST))
            brainstem_tick_learning_scan(bs, &cp);

        if (th && (percept_work > 0 || dmn_work > 0)) {
            thalamus_send_feedback(th, -1, -1, percept_work, dmn_work);
        }

        brainstem_tick_freeze(bs, &cp);

        /* 定期存盘：每 300 tick (≈5min) 持久化完整状态，防崩溃丢数据 */
        if (bs->tick_count % 300 == 0) {
            int saved = master_save_state(bs->master, "pivotmind_state.dat");
            if (saved > 0 && bs->verbose)
                LOG_INFO("[存盘] tick=%d 已保存 %d 节点", bs->tick_count, saved);
        }
    }

    if (bs->verbose) LOG_INFO("[脑干] 已停止, tick=%d", bs->tick_count);
    return NULL;
}

Brainstem* brainstem_create(MasterTopology* master, MemorySystem* memory, CognitiveState* state) {
    if (!master) return NULL;

    Brainstem* bs = (Brainstem*)calloc(1, sizeof(Brainstem));
    if (!bs) return NULL;

    bs->master          = master;
    bs->memory          = memory;
    bs->cognitive_state = state;

    bs->tick_interval_ms       = PM_CLOCK_TICK_INTERVAL_MS;
    bs->decay_per_tick         = PM_CLOCK_DECAY_PER_TICK;
    bs->spontaneous_prob       = PM_CLOCK_SPONTANEOUS_PROB;
    bs->spontaneous_strength   = PM_CLOCK_SPONTANEOUS_STRENGTH;
    bs->consolidate_every_n_ticks = PM_CLOCK_CONSOLIDATE_INTERVAL;

    bs->is_running    = 0;
    bs->tick_count    = 0;
    bs->start_time    = time(NULL);
    bs->current_time  = bs->start_time;
    bs->_rng_seed     = (unsigned int)time(NULL) ^ (unsigned int)(uintptr_t)bs;

    /* 内感受自检（由 brainstem_destroy 统一清理） */
    bs->health_monitor = health_monitor_create();

    return bs;
}

void brainstem_start(Brainstem* bs) {
    if (!bs || bs->is_running) return;
    bs->is_running = 1;
    int ret = pthread_create(&bs->thread, NULL, brainstem_loop, (void*)bs);
    if (ret != 0) {
        bs->is_running = 0;
        LOG_ERROR("[脑干] 线程创建失败 (errno=%d)", ret);
        return;
    }
    if (bs->verbose) LOG_INFO("[脑干] 线程已启动");
}

void brainstem_stop(Brainstem* bs) {
    if (!bs) return;
    /* C11 原子交换替代过时的 __sync_lock_test_and_set */
    int was_running = __atomic_exchange_n(&bs->is_running, 0, __ATOMIC_SEQ_CST);
    if (!was_running) return;
    pthread_join(bs->thread, NULL);
    if (bs->verbose) LOG_INFO("[脑干] 线程已停止 (tick=%d)", bs->tick_count);
}

void brainstem_destroy(Brainstem* bs) {
    if (!bs) return;
    if (bs->is_running) brainstem_stop(bs);
    health_monitor_destroy(bs->health_monitor);
    bs->health_monitor = NULL;
    free(bs);
}

int brainstem_tick_count(Brainstem* bs) {
    return bs ? bs->tick_count : 0;
}

void brainstem_set_verbose(Brainstem* bs, int verbose) {
    if (bs) bs->verbose = verbose;
}

const char* brainstem_get_real_time(Brainstem* bs, char* buf, int size) {
    if (!bs || !buf || size < 16) return NULL;
    struct tm* tm_info = localtime(&bs->current_time);
    if (!tm_info) return NULL;
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", tm_info);
    return buf;
}

long brainstem_get_uptime(Brainstem* bs) {
    if (!bs) return 0;
    return (long)(bs->current_time - bs->start_time);
}

float brainstem_get_circadian(Brainstem* bs) {
    if (!bs) return 0.5f;
    return circadian_activity(current_hour(bs->current_time));
}

const char* brainstem_get_circadian_phase(Brainstem* bs) {
    if (!bs) return "unknown";
    int hour = current_hour(bs->current_time);
    return brainstem_circadian_phase_name(hour, 0);
}

void brainstem_set_thalamus(Brainstem* bs, Thalamus* th) {
    if (bs) bs->thalamus = th;
}

/* 已删除：brainstem_set_node_cache / brainstem_set_perception /
 * brainstem_set_hippocampus / brainstem_set_cerebellum /
 * brainstem_set_cognitive_controller / brainstem_set_self_learner /
 * brainstem_set_topo_brain
 */
