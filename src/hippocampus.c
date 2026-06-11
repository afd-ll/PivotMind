/**
 * @file hippocampus.c
 * @brief 海马体实现 — 记忆系统 + 巩固 + 感觉皮层联动
 */

#include "hippocampus.h"
#include "perception.h"
#include "thalamus.h"
#include <stdio.h>
#include <stdlib.h>

Hippocampus* hippocampus_create(MasterTopology* topology,
                                  MemorySystem* memory,
                                  void* perception,
                                  void* thalamus) {
    if (!topology || !memory) return NULL;

    Hippocampus* hc = (Hippocampus*)calloc(1, sizeof(Hippocampus));
    if (!hc) return NULL;

    hc->topology   = topology;
    hc->memory     = memory;
    hc->perception = perception;
    hc->thalamus   = thalamus;

    printf("[海马体] 就绪 (感知联动=%s, 丘脑反馈=%s)\n",
           perception ? "ON" : "OFF",
           thalamus ? "ON" : "OFF");
    return hc;
}

void hippocampus_destroy(Hippocampus* hc) {
    if (!hc) return;
    free(hc);
}

int hippocampus_consolidate(Hippocampus* hc) {
    if (!hc) return 0;
    hc->consolidations++;

    int consolidated = 0;
    memory_consolidate(hc->memory);
    consolidated = 1;  /* 标记已执行 */

    /* 感觉皮层联动：选低置信度概念联网查证 */
    if (hc->perception && hc->topology) {
        SubTopology* vocab = NULL;
        for (int t = 0; t < hc->topology->sub_topo_count; t++) {
            SubTopology* sub = hc->topology->sub_topologies[t];
            if (sub && sub->type == TOPO_VOCABULARY) { vocab = sub; break; }
        }
        if (vocab && vocab->net && vocab->net->node_count > 0) {
            int best_id = -1;
            float worst_conf = 1.0f;
            for (int i = 0; i < vocab->net->node_count && i < 500; i++) {
                ReasoningNode* node = vocab->net->nodes[i];
                if (!node || !node->concept || node->is_cooled) continue;
                if (node->connection_count > 0 && node->confidence < worst_conf && node->confidence < 0.3f) {
                    worst_conf = node->confidence;
                    best_id = i;
                }
            }
            if (best_id >= 0) {
                Perception* p = (Perception*)hc->perception;
                if (perception_consolidate_node(p, best_id) > 0) {
                    hc->web_queries++;
                }
            }
        }
    }

    /* 通知丘脑 */
    if (hc->thalamus) {
        thalamus_report((Thalamus*)hc->thalamus, consolidated, (int)hc->web_queries, -1);
    }

    return consolidated;
}
