/**
 * walk_attn3.c —— 两本账逐步走边 v3（离线探针）
 *
 * v2 暴露的三个问题，v3 全部改掉：
 *  ① 停止条件过紧（nc<2 就断）→ 改为 nc==0 / 连续三步 p(top1)<0.20 / 走满上限
 *  ② top() 对"已生成词"也算 ⇒ 候选全是当前词的邻居，top 几乎同值（0.23~0.28）没有区分度
 *     → 改为**只对原始输入词**算 top，已生成词不再参与（那是"漂移"的来源）
 *  ③ 链会飘（音乐→伴奏→旋律→美妙的→做饭→滴滴）
 *     → 加硬闸 GATE：候选必须与**输入词**有语义边，否则不进候选（gate=1）
 *
 * logits = α·z_pos + (1-α)·z_top，两本账各在本步候选集内 z-score
 * 用法：./walk_attn3 <起点> <上文,逗号分隔> [路径数] [步数] [alpha] [T] [gate]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "multi_topology.h"
#include "constants.h"
#include "pivotmind_paths.h"
#include "transition_model.h"

#define MAXC 512
#define SEM_LO 0.10f
#define SEM_HI 0.35f

static HuarongTopologyNet* g_net;

static float edge_sem(ReasoningNode* a, ReasoningNode* b) {
    if (!a || !b || !a->edges) return 0.0f;
    for (int i = 0; i < a->edge_count; i++) {
        if (a->edges[i].target != b) continue;
        float w = a->edges[i].weight;
        if (w < SEM_LO || w > SEM_HI) return 0.0f;   /* 只认语义边，语序边不参与话题 */
        return w;
    }
    return 0.0f;
}

static int out_ok(const char* s) {
    if (!s) return 0;
    if (strlen(s) < 6) return 0;
    if (s[0] == '@' || s[0] == '?') return 0;
    if (s[0] == 'H' && s[1] == 'e') return 0;
    return 1;
}

int main(int argc, char** argv) {
    const char* seed = (argc > 1) ? argv[1] : "编程";
    const char* ctx  = (argc > 2) ? argv[2] : "";
    int npath = (argc > 3) ? atoi(argv[3]) : 3;
    int steps = (argc > 4) ? atoi(argv[4]) : 8;
    float alpha = (argc > 5) ? (float)atof(argv[5]) : 0.15f;
    float T     = (argc > 6) ? (float)atof(argv[6]) : 0.5f;
    int gate    = (argc > 7) ? atoi(argv[7]) : 1;

    MasterTopology* master = master_topology_create(16);
    if (!master) return 1;
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 60000, 10);
    master_add_sub_topology(master, TOPO_SEMANTIC,   "语义拓扑", 20000, 9);
    master_add_sub_topology(master, TOPO_EMOTION,    "情绪拓扑", 8000, 8);
    master_add_sub_topology(master, TOPO_SYNTAX,     "语法拓扑", 4000, 7);
    master_add_sub_topology(master, TOPO_CONTEXT,    "上下文拓扑", 4000, 6);
    master_add_sub_topology(master, TOPO_DOMAIN,     "领域拓扑", 4000, 5);
    master_add_sub_topology(master, TOPO_PRAGMA,     "语用拓扑", 4000, 4);
    master_add_sub_topology(master, TOPO_CULTURE,    "文化拓扑", 4000, 3);
    master_add_sub_topology(master, TOPO_CONCEPT,    "概念拓扑", 20000, 9);
    master_add_sub_topology(master, TOPO_MASTER,     "主拓扑", 100, 0);
    master_add_sub_topology(master, TOPO_TEMPLATE,   "模板拓扑", 8000, 8);
    master_add_sub_topology(master, TOPO_VISUAL,     "视觉拓扑", 8000, 7);

    if (master_load_state(master, pm_file(PM_FILE_STATE)) < 0) { fprintf(stderr, "FAIL state\n"); return 1; }
    SubTopology* vocab = NULL;
    for (int i = 0; i < master->sub_topo_count; i++)
        if (master->sub_topologies[i]->type == TOPO_VOCABULARY) { vocab = master->sub_topologies[i]; break; }
    if (!vocab || !vocab->net) { fprintf(stderr, "FAIL vocab\n"); return 1; }
    g_net = vocab->net;

    TransitionModel* tm = tm_create();
    if (!tm || tm_load(tm, pm_file(PM_FILE_TRANSITIONS)) < 0) { fprintf(stderr, "FAIL transitions\n"); return 1; }

    ReasoningNode* cw[64];
    int ncw = 0;
    if (ctx[0]) {
        char buf[1024];
        snprintf(buf, sizeof buf, "%s", ctx);
        for (char* t = strtok(buf, ","); t && ncw < 64; t = strtok(NULL, ",")) {
            int id = huarong_net_find_concept(g_net, t);
            if (id >= 0 && id < g_net->node_count && g_net->nodes[id]) cw[ncw++] = g_net->nodes[id];
        }
    }
    int sid = huarong_net_find_concept(g_net, seed);
    if (sid < 0) { fprintf(stderr, "FAIL seed\n"); return 1; }

    printf("转移表 %d 对 · α=%.2f T=%.2f gate=%d · 上文 %d 词\n",
           tm_pair_count(tm), (double)alpha, (double)T, gate, ncw);

    for (int p = 0; p < npath; p++) {
        ReasoningNode* cur = g_net->nodes[sid];
        ReasoningNode* vis[32];
        int nv = 0;
        vis[nv++] = cur;
        int weak = 0;
        printf("  路径%d: %s", p + 1, cur->concept);

        for (int s = 0; s < steps; s++) {
            ReasoningNode* cand[MAXC];
            float tw[MAXC];
            int nc = 0;
            for (int e = 0; e < cur->edge_count && nc < MAXC; e++) {
                float w = cur->edges[e].weight;
                if (w < SEM_LO || w > SEM_HI) continue;
                ReasoningNode* t = cur->edges[e].target;
                if (!t || !out_ok(t->concept)) continue;
                int dup = 0;
                for (int k = 0; k < nv; k++) if (!strcmp(vis[k]->concept, t->concept)) { dup = 1; break; }
                for (int k = 0; k < nc; k++) if (!strcmp(cand[k]->concept, t->concept)) { dup = 1; break; }
                if (dup) continue;

                /* 话题分：只对**原始输入词**（双向取大） */
                float top = 0.0f;
                for (int k = 0; k < ncw; k++) {
                    float a = edge_sem(t, cw[k]);
                    float b = edge_sem(cw[k], t);
                    if (a > top) top = a;
                    if (b > top) top = b;
                }
                /* 硬闸：必须挂在输入上（否则一步就飘走） */
                if (gate && ncw > 0 && top <= 0.0f) continue;

                cand[nc] = t;
                tw[nc] = top;
                nc++;
            }
            if (nc == 0) { printf(" ⟂(无候选)"); break; }

            float pos[MAXC];
            double mp = 0, mt = 0;
            for (int i = 0; i < nc; i++) {
                float pr = tm_prob(tm, cur->node_id, cand[i]->node_id);
                pos[i] = logf(pr > 1e-6f ? pr : 1e-6f);
                mp += pos[i];
                mt += tw[i];
            }
            mp /= nc; mt /= nc;
            double sp = 0, st = 0;
            for (int i = 0; i < nc; i++) {
                sp += (pos[i] - mp) * (pos[i] - mp);
                st += (tw[i] - mt) * (tw[i] - mt);
            }
            sp = sqrt(sp / nc) + 1e-3;
            st = sqrt(st / nc) + 1e-3;

            float lg[MAXC], mx = -1e30f;
            for (int i = 0; i < nc; i++)
                lg[i] = alpha * (float)((pos[i] - mp) / sp) + (1.0f - alpha) * (float)((tw[i] - mt) / st);
            for (int i = 0; i < nc; i++) if (lg[i] > mx) mx = lg[i];

            float pr2[MAXC], sum = 0;
            for (int i = 0; i < nc; i++) { pr2[i] = expf((lg[i] - mx) / T); sum += pr2[i]; }

            if (s == 0) {
                printf("\n    候选 %d · ", nc);
                float cp[MAXC];
                memcpy(cp, lg, sizeof(float) * (size_t)nc);
                for (int k = 0; k < 3; k++) {
                    int best = -1;
                    for (int i = 0; i < nc; i++)
                        if (cp[i] > -1e29f && (best < 0 || cp[i] > cp[best])) best = i;
                    if (best < 0) break;
                    printf("%s(pos=%.2f,top=%.2f,p=%.2f) ", cand[best]->concept,
                           (double)pos[best], (double)tw[best], (double)(pr2[best] / sum));
                    cp[best] = -1e30f;
                }
                printf("\n");
            }

            float pmax = 0.0f;
            for (int i = 0; i < nc; i++) if (pr2[i] > pmax) pmax = pr2[i];
            pmax /= sum;
            if (s >= 1 && pmax < 0.20f) weak++;

            float r = (float)rand() / (float)RAND_MAX * (float)sum, acc = 0.0f;
            int pick = nc - 1;
            for (int i = 0; i < nc; i++) { acc += pr2[i]; if (acc >= r) { pick = i; break; } }
            cur = cand[pick];
            vis[nv++] = cur;
            printf(" → %s", cur->concept);

            if (weak >= 3) { printf(" ⊣(连三步在猜)"); break; }
        }
        printf("\n");
    }
    tm_destroy(tm);
    return 0;
}
