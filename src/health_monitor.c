/**
 * @file health_monitor.c
 * @brief 内感受自检 — 检测不健康指标 → 调度器自动干预
 */

#include "health_monitor.h"
#include "feature_io.h"
#include "cross_edge_io.h"
#include "platform.h"
#include "error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

HealthMonitor* health_monitor_create(void) {
    HealthMonitor* hm = (HealthMonitor*)calloc(1, sizeof(HealthMonitor));
    if (!hm) return NULL;

    /* v0.5.7: 阈值改为系统总内存占用率（物理内存 85% 触发 RED——
     * 用户方案正确落地：监控系统总占用（/proc/meminfo），
     * 不是进程 RSS——进程 RSS 的"4GB 的 85%"永远到不了，
     * 系统在 1.3GB 时就 OOM 强杀，RED 形同虚设） */
    hm->rss_yellow_mb     = 0.68f;   /* 系统总占用 68% → YELLOW */
    hm->rss_red_mb        = 0.85f;   /* 系统总占用 85% → RED */
    hm->rss_growth_yellow = 2.0f;   /* MB/min */
    hm->rss_growth_red    = 500.0f;  /* v0.5.7: 不再用于 RED 判定（增速采样不可靠，已两次误判）——保留字段兼容 */
    hm->conn_growth_yellow = 500;
    hm->conn_growth_red    = 2000;
    hm->frozen_yellow      = 5000;   /* v0.5.13: 500 在 12 万节点下必然 YELLOW→冻结加速恶性循环 */
    hm->frozen_red         = 10000; /* v0.6: 正常学习也会累计冻结计数，2000 太易触发 RED 锁存 */

    hm->level  = HM_GREEN;
    hm->reason = "系统正常";

    return hm;
}

void health_monitor_destroy(HealthMonitor* hm) {
    free(hm);
}

HealthLevel health_get_level(HealthMonitor* hm) {
    return hm ? hm->level : HM_GREEN;
}

static void _emergency_save(MasterTopology* master) {
    if (!master) return;
    LOG_WARNING("[内感受] 紧急存盘...");
    int saved = master_save_state(master, "pivotmind_state.dat");
    if (saved >= 0) save_features(master, "features.bin");
    save_cross_edges(master, "cross_edges.bin");
}

static void _aggressive_prune(MasterTopology* master) {
    int released = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        for (int i = 0; i < sub->net->node_count; i++) {
            ReasoningNode* node = sub->net->nodes[i];
            if (!node || !node->edges) continue;
            int lock_idx = node->node_id & (PM_NODE_LOCK_COUNT - 1);
            pthread_mutex_lock(&sub->net->node_locks[lock_idx]);
            int kept = 0;
            for (int c = 0; c < node->edge_count; c++) {
                float w = node->edges[c].weight;
                float conf = node->edges[c].confidence;
                /* RED 模式：阈值提高10倍 */
                if (w * conf < 0.01f) { released++; continue; }
                if (w < 0.005f) { released++; continue; }
                if (kept != c) {
                    node->edges[kept].target = node->edges[c].target;
                    node->edges[kept].weight = node->edges[c].weight;
                    node->edges[kept].motivational_bias =
                        node->edges[c].motivational_bias;
                    node->edges[kept].confidence =
                        node->edges[c].confidence;
                }
                kept++;
            }
            /* 与 add_connection 保持一致的 RELEASE 序 */
            __atomic_store_n(&node->edge_count, kept, __ATOMIC_RELEASE);
            /* 边压缩后重建 conn_hash，索引已变化。
             * 先置 NULL 再 free，防止无锁读 node_conn_hash_lookup 踩悬空指针 */
            {
                void* old_hash = node->conn_hash;
                node->conn_hash = NULL;
                node->conn_hash_mask = -1;
                node->conn_hash_entries = 0;
                free(old_hash);
            }
            for (int ci = 0; ci < node->edge_count; ci++) {
                if (node->edges[ci].target)
                    node_conn_hash_insert(NULL, node, node->edges[ci].target, ci);
            }
            pthread_mutex_unlock(&sub->net->node_locks[lock_idx]);
        }
    }
    if (released > 0)
        LOG_WARNING("[内感受] 紧急修剪: 释放%d条弱边", released);
}

void health_monitor_tick(HealthMonitor* hm,
                         MasterTopology* master,
                         CognitiveController* controller) {
    if (!hm || !master) return;

    hm->ticks_since_check++;

    /* ── 采集当前指标（跨平台内存读取） ── */
    { long rss_kb = 0;
      if (pm_get_process_memory(&rss_kb, NULL) == 0)
          hm->rss_mb = (float)rss_kb / 1024.0f;

    /* v0.5.7: 系统总内存占用率（/proc/meminfo）——RED 判定的
     * 主信号：物理内存占用 85% 就是该 RED，不管进程 RSS */
    {
        /* v0.5.7: 真实占用（不含可回收 cache）——MemAvailable 含 Cached
         * 波动，读取瞬间可瞬时归零（实测 RSS 273MB 时报系统占用 100%），
         * 不能做 RED 信号。真实占用 = MemTotal - MemFree - Buffers - Cached，
         * 稳定不随 cache 回收波动 */
        long mem_total = 0, mem_free = 0, mem_buffers = 0, mem_cached = 0;
        FILE* mf = fopen("/proc/meminfo", "r");
        if (mf) {
            /* v0.5.7: 逐行 fgets 解析——fscanf("%63s %ld") 会被值后的
             * "kB" 单位坑：第2轮 %s 读到 "kB"、%ld 读 "MemFree:" 失败
             * → 循环退出 → MemFree/Buffers/Cached 全 0 → 占用率恒 100%！
             * （实测 RSS 695MB 报 100%，RED 门禁形同虚设） */
            char line[128];
            while (fgets(line, sizeof(line), mf)) {
                char key[64]; long val;
                if (sscanf(line, "%63s %ld", key, &val) == 2) {
                    if (strcmp(key, "MemTotal:") == 0) mem_total = val;
                    else if (strcmp(key, "MemFree:") == 0) mem_free = val;
                    else if (strcmp(key, "Buffers:") == 0) mem_buffers = val;
                    else if (strcmp(key, "Cached:") == 0) { mem_cached = val; break; }
                }
            }
            fclose(mf);
        }
        if (mem_total > 0) {
            long used = mem_total - mem_free - mem_buffers - mem_cached;
            if (used < 0) used = 0;
            hm->sys_usage_ratio = (float)used / (float)mem_total;
        }
    }
    }

    hm->total_nodes = 0;
    hm->total_conns = 0;
    hm->frozen_nodes = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        hm->total_nodes += sub->net->node_count;
        for (int i = 0; i < sub->net->node_count; i++) {
            ReasoningNode* n = sub->net->nodes[i];
            if (!n) continue;
            hm->total_conns += n->edge_count;
            if (n->is_cooled) hm->frozen_nodes++;
        }
    }

    /* 首次采样初始化基线 */
    if (hm->last_tick == 0) {
        hm->last_rss_mb       = hm->rss_mb;
        hm->last_total_conns  = hm->total_conns;
        hm->last_tick         = hm->ticks_since_check;
        return;
    }

    /* ── 计算增速 ── */
    int   tick_delta  = hm->ticks_since_check - hm->last_tick;
    float rss_delta   = hm->rss_mb - hm->last_rss_mb;
    int   conn_delta  = hm->total_conns - hm->last_total_conns;

    hm->rss_growth_mb_min = tick_delta > 0 ?
        (rss_delta / tick_delta) * 60.0f : 0;
    hm->conn_growth        = conn_delta;

    /* ── 判断健康等级 ── */
    HealthLevel new_level = HM_GREEN;
    const char* reason = "系统正常";

    /* v0.5.7: RED 只由内存占用触发（85% = 3400MB）——瞬时增速两次误判
     * （176MB +142/min、389MB +2092/min 删节点）+ 冻结计数累计误判
     * （正常学习累计 5.6 万冻结超 10000 触发 RED）。采样不可靠的
     * 指标全部不参与 RED 判定，只有真实内存占用才算数 */
    /* v0.5.7: RED 双重确认——系统占用 >85% 且 RSS >1500MB 才触发。
     * 系统占用率含 Shmem/瞬时波动（实测 RSS 699MB 时误报超标，
     * 实际玄枢+其他进程封顶 ~78%），单看占用率不可靠。
     * RSS 是玄枢自己的真实内存，到 1.5GB 才接近系统压力。 */
    if (hm->sys_usage_ratio > hm->rss_red_mb && hm->rss_mb > 1500.0f) {
        hm->red_streak++;
        if (hm->red_streak >= 3) {
            new_level = HM_RED;
            reason    = "系统内存占用超标";
        } else {
            new_level = HM_YELLOW;  /* 未确认前先黄，不触发 RED 动作 */
            reason    = "系统占用高（确认中）";
        }
    } else {
        hm->red_streak = 0;
        if (hm->sys_usage_ratio > hm->rss_yellow_mb) {
            new_level = HM_YELLOW;
            reason    = "RSS接近上限";
        } else if (hm->frozen_nodes > hm->frozen_yellow) {
            new_level = HM_YELLOW;
            reason    = "冻结速率偏高";
        }
    }

    /* ── 调度器干预 ── */
    if (new_level >= HM_YELLOW && hm->level < HM_YELLOW) {
        LOG_WARNING("[内感受] ⚠ YELLOW → %s | RSS=%.1fMB(+%.2f/min) 连接=%d(+%d) 冻结=%d 系统占用=%.0f%%",
                reason, hm->rss_mb, hm->rss_growth_mb_min, hm->total_conns, conn_delta, hm->frozen_nodes, hm->sys_usage_ratio * 100.0f);
        if (controller) {
            controller->satisfaction_threshold = 0.5f;     /* 提高门槛保质量 */
        }
    }

    if (new_level == HM_RED && hm->level < HM_RED) {
        LOG_ERROR("[内感受] 🔴 RED → %s | RSS=%.1fMB(+%.2f/min) 连接=%d 冻结=%d 系统占用=%.0f%%",
                reason, hm->rss_mb, hm->rss_growth_mb_min, hm->total_conns, hm->frozen_nodes, hm->sys_usage_ratio * 100.0f);
        _emergency_save(master);
        _aggressive_prune(master);
        hm->emergency_saves++;
        if (controller) {
            controller->satisfaction_threshold = 0.8f;     /* 严格模式 */
        }
    }

    if (new_level < hm->level) {
        LOG_INFO("[内感受] ✓ 恢复 GREEN | RSS=%.1fMB", hm->rss_mb);
        if (controller) controller->satisfaction_threshold = 0.15f;  /* 恢复宽松 */
    }

    hm->level  = new_level;
    hm->reason = reason;
    hm->interventions += (new_level >= HM_YELLOW);

    /* 更新基线 */
    hm->last_rss_mb      = hm->rss_mb;
    hm->last_total_conns = hm->total_conns;
    hm->last_tick        = hm->ticks_since_check;
}
