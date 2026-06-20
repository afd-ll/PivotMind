/**
 * @file hippocampus.c
 * @brief 海马体实现 — 记忆系统 + 巩固 + 感觉皮层联动
 *
 * 通信原则：
 *   通过丘脑(Thalamus)信号总线获取感知皮层实例和上报反馈，
 *   不直接持有 void* 指针。
 */

#include "hippocampus.h"
#include <string.h>
#include "perception.h"
#include "thalamus.h"
#include <stdio.h>
#include <stdlib.h>

Hippocampus* hippocampus_create(MasterTopology* topology,
                                  MemorySystem* memory,
                                  Thalamus* thalamus) {
    if (!topology || !memory) return NULL;

    Hippocampus* hc = (Hippocampus*)calloc(1, sizeof(Hippocampus));
    if (!hc) return NULL;

    hc->topology   = topology;
    hc->memory     = memory;
    hc->thalamus   = thalamus;

    printf("[海马体] 就绪 (丘脑信号总线=%s)\n",
           thalamus ? "ON" : "OFF");
    return hc;
}

void hippocampus_destroy(Hippocampus* hc) {
    if (!hc) return;
    free(hc);
}

void hippocampus_log_dialog(Hippocampus* hc, const char* input, const char* response) {
    if (!hc || !input || !response) return;
    int slot = hc->log_pos;
    snprintf(hc->dialog_log[slot], 1023, "%s|%s", input, response);
    hc->log_pos = (hc->log_pos + 1) % 4;
    if (hc->log_count < 4) hc->log_count++;
}

int hippocampus_consolidate(Hippocampus* hc) {
    if (!hc) return 0;
    hc->consolidations++;

    int consolidated_conns = 0;
    memory_consolidate(hc->memory);
    consolidated_conns = 1;  /* 基础记忆巩固算 1 */

    /* QA 重放：把最近对话注回拓扑建连接，统计实际连接数 */
    if (hc->topology && hc->log_count > 0) {
        SubTopology* vocab = master_get_sub_topology_by_type(hc->topology, TOPO_VOCABULARY);
        if (vocab && vocab->net) {
            for (int l = 0; l < hc->log_count; l++) {
                char* qa = hc->dialog_log[l];
                char* sep = strchr(qa, '|');
                if (!sep) continue;
                *sep = 0;
                char* q = qa;
                char* a = sep + 1;
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
                        if (qid >= 0 && aid >= 0 && qid != aid) {
                            huarong_net_add_connection(vocab->net, qid, aid, 0.3f);
                            consolidated_conns++;
                        }
                    }
                }
                *sep = '|';
            }
        }
    }

    /* 感觉皮层联动：通过丘脑获取感知皮层实例，选低置信度概念联网查证 */
    int web_queries_this = 0;
    if (hc->thalamus && hc->topology) {
        Perception* p = (Perception*)thalamus_get_region(hc->thalamus, THAL_PERCEPTION);
        if (p) {
            SubTopology* vocab = master_get_sub_topology_by_type(hc->topology, TOPO_VOCABULARY);
            if (vocab && vocab->net && vocab->net->node_count > 0) {
                int best_id = -1;
                float worst_conf = 1.0f;
                for (int i = 0; i < vocab->net->node_count && i < 500; i++) {
                    ReasoningNode* node = vocab->net->nodes[i];
                    if (!node || !node->concept || node->is_cooled) continue;
                    if (node->edge_count > 0 && node->confidence < worst_conf && node->confidence < 0.3f) {
                        worst_conf = node->confidence;
                        best_id = i;
                    }
                }
                if (best_id >= 0) {
                    /* 主路径：直接调用感知皮层公开 API */
                    int qr = perception_consolidate_node(p, best_id);
                    /* 备选路径：通过丘脑信号总线发送 CONS_NODE 信号（解耦） */
                    BrainSignal sig;
                    memset(&sig, 0, sizeof(sig));
                    sig.type     = THAL_SIG_CONSOLIDATE_NODE;
                    sig.source   = THAL_HIPPOCAMPUS;
                    sig.target   = THAL_PERCEPTION;
                    sig.data.consolidate.node_id  = best_id;
                    thalamus_send_signal(hc->thalamus, THAL_PERCEPTION, &sig);
                    if (qr > 0) {
                        hc->web_queries++;
                        web_queries_this = qr;
                    }
                }
            }
        }
    }

    /* 通过丘脑信号总线反馈上报（实际连接数） */
    if (hc->thalamus) {
        thalamus_send_feedback(hc->thalamus, THAL_HIPPOCAMPUS,
                                consolidated_conns, web_queries_this, -1);
    }

    return consolidated_conns;
}
