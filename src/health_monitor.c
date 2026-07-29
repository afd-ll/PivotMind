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

    /* 默认阈值 — 针对 3.8G 板子，留足喂料缓冲 */
    hm->rss_yellow_mb     = 800.0f;
    hm->rss_red_mb        = 1000.0f;
    hm->rss_growth_yellow = 2.0f;   /* MB/min */
    hm->rss_growth_red    = 5.0f;
    hm->conn_growth_yellow = 500;
    hm->conn_growth_red    = 2000;
    hm->frozen_yellow      = 500;
    hm->frozen_red         = 2000;

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

    if (hm->rss_mb > hm->rss_red_mb || hm->rss_growth_mb_min > hm->rss_growth_red) {
        new_level = HM_RED;
        reason    = hm->rss_mb > hm->rss_red_mb ? "RSS超标" : "内存增速过快";
    } else if (hm->conn_growth > hm->conn_growth_red) {
        new_level = HM_RED;
        reason    = "连接膨胀失控";
    } else if (hm->rss_mb > hm->rss_yellow_mb || hm->rss_growth_mb_min > hm->rss_growth_yellow) {
        new_level = HM_YELLOW;
        reason    = hm->rss_mb > hm->rss_yellow_mb ? "RSS接近上限" : "内存增速偏快";
    } else if (hm->conn_growth > hm->conn_growth_yellow) {
        new_level = HM_YELLOW;
        reason    = "连接增速偏快";
    } else if (hm->frozen_nodes > hm->frozen_red) {
        new_level = HM_RED;
        reason    = "节点冻结过多";
    } else if (hm->frozen_nodes > hm->frozen_yellow) {
        new_level = HM_YELLOW;
        reason    = "冻结速率偏高";
    }

    /* ── 调度器干预 ── */
    if (new_level >= HM_YELLOW && hm->level < HM_YELLOW) {
        LOG_WARNING("[内感受] ⚠ YELLOW → %s | RSS=%.1fMB(+%.2f/min) 连接=%d(+%d) 冻结=%d",
                reason, hm->rss_mb, hm->rss_growth_mb_min, hm->total_conns, conn_delta, hm->frozen_nodes);
        if (controller) {
            controller->satisfaction_threshold = 0.5f;     /* 提高门槛保质量 */
        }
    }

    if (new_level == HM_RED && hm->level < HM_RED) {
        LOG_ERROR("[内感受] 🔴 RED → %s | RSS=%.1fMB(+%.2f/min) 连接=%d 冻结=%d",
                reason, hm->rss_mb, hm->rss_growth_mb_min, hm->total_conns, hm->frozen_nodes);
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
