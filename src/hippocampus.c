/**
 * @file hippocampus.c
 * @brief 海马体实现 — 记忆系统 + 巩固 + 感觉皮层联动
 */

#include "hippocampus.h"
#include <string.h>
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

void hippocampus_log_dialog(Hippocampus* hc, const char* input, const char* response) {
    if (!hc || !input || !response) return;
    /* 环形缓冲: 存最近4轮对话 */
    int slot = hc->log_pos;
    snprintf(hc->dialog_log[slot], 1023, "%s|%s", input, response);
    hc->log_pos = (hc->log_pos + 1) % 4;
    if (hc->log_count < 4) hc->log_count++;
}

int hippocampus_consolidate(Hippocampus* hc) {
    if (!hc) return 0;
    hc->consolidations++;

    int consolidated = 0;
    memory_consolidate(hc->memory);
    consolidated = 1;

    /* QA 重放：把最近对话注回拓扑建连接 */
    if (hc->topology && hc->log_count > 0) {
        SubTopology* vocab = NULL;
        for (int t = 0; t < hc->topology->sub_topo_count; t++) {
            if (hc->topology->sub_topologies[t] &&
                hc->topology->sub_topologies[t]->type == TOPO_VOCABULARY)
                { vocab = hc->topology->sub_topologies[t]; break; }
        }
        if (vocab && vocab->net) {
            for (int l = 0; l < hc->log_count; l++) {
                char* qa = hc->dialog_log[l];
                char* sep = strchr(qa, '|');
                if (!sep) continue;
                *sep = 0;
                char* q = qa;
                char* a = sep + 1;
                /* 逐词建 QA 之间的共现连接 */
                for (int qi = 0; q[qi]; qi += 3) {
                    for (int ai = 0; a[ai]; ai += 3) {
                        char qw[4] = {q[qi], q[qi+1], q[qi+2], 0};
                        char aw[4] = {a[ai], a[ai+1], a[ai+2], 0};
                        if (strlen(qw) < 2 || strlen(aw) < 2) continue;
                        int qid = -1, aid = -1;
                        for (int i = 0; i < vocab->net->node_count; i++) {
                            ReasoningNode* n = vocab->net->nodes[i];
                            if (n && n->concept) {
                                if (qid < 0 && strcmp(n->concept, qw) == 0) qid = i;
                                if (aid < 0 && strcmp(n->concept, aw) == 0) aid = i;
                                if (qid >= 0 && aid >= 0) break;
                            }
                        }
                        if (qid >= 0 && aid >= 0 && qid != aid)
                            huarong_net_add_connection(vocab->net, qid, aid, 0.3f);
                    }
                }
                *sep = '|';
            }
        }
    }

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
