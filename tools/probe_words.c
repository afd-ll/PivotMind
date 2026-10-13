/**
 * probe_words.c — 只读探针：验证"词节点在概念拓扑，锚定在词汇拓扑"是否断链
 *
 * 用途：给定状态文件与若干查询词，报告：
 *   1) 它在【词汇拓扑】能否被找到（= 锚定路径 find_concept(vocab) 能否命中）
 *   2) 它在【概念拓扑】能否被找到（= 词涌现晋升后的落点）
 *   3) 从字节点出发，能否经 cross-link 到达词节点（= 通路是否可通）
 *
 * 只读，不保存。用法: probe_words <state.dat> <词1> [词2 ...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multi_topology.h"
#include "huarong_topology.h"
#include "pivotmind_paths.h"
#include "lang.h"

static SubTopology* find_topo(MasterTopology* m, int type) {
    for (int i = 0; i < m->sub_topo_count; i++)
        if (m->sub_topologies[i] && m->sub_topologies[i]->type == type)
            return m->sub_topologies[i];
    return NULL;
}

/* 「pairs」模式：量化字对涌现判据为什么过不去（只读）
 * 对每个中文字节点 a：avg_w(a)、最大相对强度 rel=w/avg_w、双向确认的最强对。 */
static int dump_pairs(HuarongTopologyNet* vocab, HuarongTopologyNet* concept) {
    int vn = vocab->node_count;
    printf("== 字对涌现判据量化（词汇拓扑 %d 节点）==\n", vn);
    double rel_top = 0; int ra = -1, rb = -1;
    int cnt_rel_ge[4] = {0,0,0,0};   /* >=1,2,3,5 */
    int both_dir_hits = 0;
    int max_rel_a = -1, max_rel_b = -1;
    for (int i = 0; i < vn; i++) {
        ReasoningNode* a = vocab->nodes[i];
        if (!a || !a->concept || !a->edges || a->edge_count == 0) continue;
        if (pm_is_zh_char(a->concept) == 0) continue;   /* 仅中文单字 */
        double sum = 0; for (int j = 0; j < a->edge_count; j++) sum += a->edges[j].weight;
        double avg = sum / a->edge_count;
        if (avg <= 1e-6) continue;
        double best = 0; int bi = -1;
        for (int j = 0; j < a->edge_count; j++) {
            ReasoningNode* b = a->edges[j].target;
            if (!b || !b->concept) continue;
            double rel = a->edges[j].weight / avg;
            if (rel > best) { best = rel; bi = j; }
        }
        if (best >= 1.0) cnt_rel_ge[0]++;
        if (best >= 2.0) cnt_rel_ge[1]++;
        if (best >= 3.0) cnt_rel_ge[2]++;
        if (best >= 5.0) cnt_rel_ge[3]++;
        if (best > rel_top) { rel_top = best; ra = i; rb = (bi >= 0 && a->edges[bi].target) ? a->edges[bi].target->node_id : -1; }
        if (best == max_rel_a) {}
        /* 双向确认：反向边存在且反向相对强度也 >= 3 */
        if (bi >= 0) {
            ReasoningNode* b = a->edges[bi].target;
            if (b && b->edges && b->concept) {
                double bsum = 0; for (int k = 0; k < b->edge_count; k++) bsum += b->edges[k].weight;
                double bavg = (b->edge_count > 0) ? bsum / b->edge_count : 0;
                for (int k = 0; k < b->edge_count; k++) {
                    if (b->edges[k].target == a && bavg > 1e-6 &&
                        (b->edges[k].weight / bavg) >= 3.0) { both_dir_hits++; break; }
                }
            }
        }
    }
    printf("  单字节点最大相对强度分布: >=1.0: %d | >=2.0: %d | >=3.0: %d | >=5.0: %d\n",
           cnt_rel_ge[0], cnt_rel_ge[1], cnt_rel_ge[2], cnt_rel_ge[3]);
    printf("  全表最大相对强度 = %.2f", rel_top);
    if (ra >= 0) printf("  (%s → %s)", vocab->nodes[ra]->concept,
                        (rb >= 0 && rb < vn && vocab->nodes[rb]) ? vocab->nodes[rb]->concept : "?");
    printf("\n");
    printf("  双向均 >=3.0 的节点数 = %d\n", both_dir_hits);
    (void)concept;
    return 0;
}

/* 「top」模式：把最强字对连名字/权重/相对强度一起打出来（可读，不猜） */
static int dump_top_pairs(HuarongTopologyNet* vocab, double floor_rel) {
    int vn = vocab->node_count;
    typedef struct { int a, b; float w, wa, wb; double ra, rb; } Pair;
    static Pair best[4096]; int n = 0;
    int cand_total = 0;
    for (int i = 0; i < vn; i++) {
        ReasoningNode* a = vocab->nodes[i];
        if (!a || !a->concept || !a->edges || a->edge_count < 5) continue;
        if (pm_is_zh_char(a->concept) == 0) continue;
        if (strlen(a->concept) > 3) continue;          /* 仅单字（中文 3 字节） */
        double sum = 0; for (int j = 0; j < a->edge_count; j++) sum += a->edges[j].weight;
        double avg = sum / a->edge_count; if (avg <= 1e-6) continue;
        for (int j = 0; j < a->edge_count; j++) {
            ReasoningNode* b = a->edges[j].target;
            if (!b || !b->concept || b->node_id <= a->node_id) continue;
            if (pm_is_zh_char(b->concept) == 0 || strlen(b->concept) > 3) continue;
            double ra = a->edges[j].weight / avg;
            if (ra < floor_rel) continue;
            double wb = 0, bsum = 0;
            for (int k = 0; k < b->edge_count; k++) {
                bsum += b->edges[k].weight;
                if (b->edges[k].target == a) wb = b->edges[k].weight;
            }
            double rb = (b->edge_count > 0 && bsum > 1e-6) ? wb / (bsum / b->edge_count) : 0;
            cand_total++;
            if (n < 4096) {
                best[n].a = a->node_id; best[n].b = b->node_id;
                best[n].w = a->edges[j].weight;
                best[n].wa = (float)avg;
                best[n].wb = (float)(b->edge_count ? bsum / b->edge_count : 0);
                best[n].ra = ra; best[n].rb = rb; n++;
            }
        }
    }
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            double mi = best[i].ra < best[i].rb ? best[i].ra : best[i].rb;
            double mj = best[j].ra < best[j].rb ? best[j].ra : best[j].rb;
            if (mj > mi) { Pair t = best[i]; best[i] = best[j]; best[j] = t; }
        }
    printf("== 相对强度 >= %.2f 的字对（仅中文单字、度>=5）==\n", floor_rel);
    printf("   候选总数 = %d，下表按「双向较小值」降序\n\n", cand_total);
    printf("   %-3s %-6s %-6s %9s %9s %9s %8s %8s  %s\n",
           "#", "字A", "字B", "w(A→B)", "avg(A)", "w(B→A)", "rel_ab", "rel_ba", "双向>=2");
    int show = n < 30 ? n : 30;
    for (int i = 0; i < show; i++) {
        ReasoningNode* A = vocab->nodes[best[i].a];
        ReasoningNode* B = vocab->nodes[best[i].b];
        printf("   %-3d %-6s %-6s %9.4f %9.4f %9.4f %8.2f %8.2f  %s\n",
               i + 1, A ? A->concept : "?", B ? B->concept : "?",
               (double)best[i].w, (double)best[i].wa, (double)best[i].wb,
               best[i].ra, best[i].rb,
               (best[i].ra >= 2.0 && best[i].rb >= 2.0) ? "是" : "否");
    }
    if (n == 0) printf("   （无）\n");
    return 0;
}

/* 「wordbind」模式：量【字↔词绑定】可达性（只读）
 * 判据（不依赖 relation 字段——它存盘时未写、加载后不可信）：
 *   对概念层每个多字词节点 W，收集 W→词汇层 的 cross-link 目标字，
 *   按 node_id 排序拼成串 S；S == W 的 concept ⇒ 绑定成立。
 *   结构性判据，拼不出自己名字的绑定就是坏绑定，不需要任何词典。 */
static int dump_wordbind(MasterTopology* m, HuarongTopologyNet* vocab, HuarongTopologyNet* concept) {
    int cn = concept->node_count;
    int multi = 0, bound = 0, unbound = 0, shown = 0;
    printf("== 字↔词绑定可达性（概念层 %d 节点）==\n", cn);
    for (int i = 0; i < cn; i++) {
        ReasoningNode* w = concept->nodes[i];
        if (!w || !w->concept || !w->concept[0]) continue;
        if (strlen(w->concept) < 6) continue;         /* 只看多字词（中文单字=3字节） */
        multi++;
        int ids[16]; int nid = 0;
        for (int li = 0; li < m->cross_link_count && nid < 16; li++) {
            CrossTopologyLink* l = m->cross_links[li];
            if (!l || l->from_topo_id != TOPO_CONCEPT || l->from_node_id != i) continue;
            if (l->to_topo_id != TOPO_VOCABULARY) continue;
            ids[nid++] = l->to_node_id;
        }
        if (nid == 0) { unbound++; if (shown < 8) { printf("   ❌ %-10s ← 无任何字链接\n", w->concept); shown++; } continue; }
        for (int a = 0; a < nid; a++)
            for (int b = a + 1; b < nid; b++)
                if (ids[b] < ids[a]) { int t = ids[a]; ids[a] = ids[b]; ids[b] = t; }
        char buf[64] = {0}; int p = 0;
        for (int a = 0; a < nid && p < 48; a++) {
            if (ids[a] < 0 || ids[a] >= vocab->node_count) continue;
            ReasoningNode* vn = vocab->nodes[ids[a]];
            if (!vn || !vn->concept) continue;
            p += snprintf(buf + p, sizeof(buf) - (size_t)p, "%s", vn->concept);
        }
        if (strcmp(buf, w->concept) == 0) {
            bound++;
            if (shown < 8) { printf("   ✅ %-10s ← 字[%s] 拼回一致\n", w->concept, buf); shown++; }
        } else {
            unbound++;
            if (shown < 8) { printf("   ❌ %-10s ← 字[%s] 拼不回\n", w->concept, buf); shown++; }
        }
    }
    /* 口径复核：换「链接出现顺序」与「权重降序」再拼一次，排除排序口径导致的假阴性 */
    int bound_seq = 0, bound_w = 0;
    for (int i = 0; i < cn; i++) {
        ReasoningNode* w = concept->nodes[i];
        if (!w || !w->concept || strlen(w->concept) < 6) continue;
        int seq_ids[16]; float seq_w[16]; int seq_n = 0;
        for (int li = 0; li < m->cross_link_count && seq_n < 16; li++) {
            CrossTopologyLink* l = m->cross_links[li];
            if (!l || l->from_topo_id != TOPO_CONCEPT || l->from_node_id != i) continue;
            if (l->to_topo_id != TOPO_VOCABULARY) continue;
            seq_ids[seq_n] = l->to_node_id; seq_w[seq_n] = l->weight; seq_n++;
        }
        if (seq_n == 0) continue;
        /* (a) 链接顺序 */
        char b1[64] = {0}; int p1 = 0;
        for (int a = 0; a < seq_n && p1 < 48; a++) {
            if (seq_ids[a] < 0 || seq_ids[a] >= vocab->node_count) continue;
            ReasoningNode* vn = vocab->nodes[seq_ids[a]];
            if (vn && vn->concept) p1 += snprintf(b1 + p1, sizeof(b1) - (size_t)p1, "%s", vn->concept);
        }
        if (strcmp(b1, w->concept) == 0) bound_seq++;
        /* (b) 权重降序 */
        int idx[16]; for (int a = 0; a < seq_n; a++) idx[a] = a;
        for (int a = 0; a < seq_n; a++)
            for (int b = a + 1; b < seq_n; b++)
                if (seq_w[idx[b]] > seq_w[idx[a]]) { int t = idx[a]; idx[a] = idx[b]; idx[b] = t; }
        char b2[64] = {0}; int p2 = 0;
        for (int a = 0; a < seq_n && p2 < 48; a++) {
            int j = idx[a];
            if (seq_ids[j] < 0 || seq_ids[j] >= vocab->node_count) continue;
            ReasoningNode* vn = vocab->nodes[seq_ids[j]];
            if (vn && vn->concept) p2 += snprintf(b2 + p2, sizeof(b2) - (size_t)p2, "%s", vn->concept);
        }
        if (strcmp(b2, w->concept) == 0) bound_w++;
    }
    printf("口径复核：链接顺序 = %d ｜ 权重降序 = %d （对比 node_id 序 = %d）\n",
           bound_seq, bound_w, bound);
    printf("\n多字词节点 = %d ｜ 绑定成立 = %d ｜ 不成立或无链接 = %d\n", multi, bound, unbound);
    printf("⇒ 可被「字→词路由」认出的比例 = %.1f%%\n", multi ? 100.0 * bound / multi : 0.0);
    return 0;
}

/* 「dups」模式：量词汇拓扑里【同名字符串是否对应多个节点 id】（只读）
 * 为什么量：字对计数按 node_id 做键；若同一个字有多个 id，计数就被拆散
 * ⇒ 概率算出 >1，且真词的次数被稀释到阈值以下。 */
static int dump_dups(HuarongTopologyNet* vocab, int show) {
    int vn = vocab->node_count;
    int dup_concepts = 0, dup_nodes = 0, shown = 0;
    for (int i = 0; i < vn; i++) {
        ReasoningNode* a = vocab->nodes[i];
        if (!a || !a->concept || !a->concept[0]) continue;
        int cnt = 0, ids[8];
        for (int j = 0; j < vn && cnt < 8; j++) {
            ReasoningNode* b = vocab->nodes[j];
            if (!b || !b->concept) continue;
            if (strcmp(a->concept, b->concept) == 0) ids[cnt++] = b->node_id;
        }
        if (cnt > 1) {
            if (a->node_id != ids[0]) continue;   /* 每个概念只统计一次 */
            dup_concepts++; dup_nodes += cnt;
            if (shown < show) {
                printf("   「%s」 x%d  ids=", a->concept, cnt);
                for (int k = 0; k < cnt; k++) printf("%d ", ids[k]);
                printf("\n"); shown++;
            }
        }
    }
    printf("\n词汇拓扑 %d 节点 ｜ 重名概念 %d 个（占 %d 个节点）\n",
           vn, dup_concepts, dup_nodes);
    printf("⇒ 按 node_id 计数会被拆散：%d 个概念受影响\n", dup_concepts);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) { printf("用法: probe_words <state.dat> <词...|pairs>\n"); return 1; }
    const char* path = argv[1];

    MasterTopology* m = master_topology_create(11);
    master_add_sub_topology(m, TOPO_VOCABULARY, "词汇拓扑", 100000, 10);
    master_add_sub_topology(m, TOPO_SEMANTIC,   "语义拓扑", 50000, 9);
    master_add_sub_topology(m, TOPO_EMOTION,    "情绪拓扑", 4000, 8);
    master_add_sub_topology(m, TOPO_SYNTAX,     "语法拓扑", 1000, 7);
    master_add_sub_topology(m, TOPO_CONTEXT,    "上下文拓扑", 1000, 6);
    master_add_sub_topology(m, TOPO_DOMAIN,     "领域拓扑", 1000, 5);
    master_add_sub_topology(m, TOPO_PRAGMA,     "语用拓扑", 1000, 4);
    master_add_sub_topology(m, TOPO_CULTURE,    "文化拓扑", 1000, 3);
    master_add_sub_topology(m, TOPO_CONCEPT,    "概念拓扑", 50000, 9);
    master_add_sub_topology(m, TOPO_MASTER,     "主拓扑", 100, 0);
    master_add_sub_topology(m, TOPO_TEMPLATE,   "模板拓扑", 20000, 8);

    int n = master_load_state(m, path);
    if (n <= 10) { printf("× 状态加载失败 (%d)\n", n); return 1; }
    printf("✓ 状态: %d 节点\n", n);

    SubTopology* vocab   = find_topo(m, TOPO_VOCABULARY);
    SubTopology* concept = find_topo(m, TOPO_CONCEPT);
    printf("  词汇拓扑 %d 节点 / 概念拓扑 %d 节点 / cross_link %d\n\n",
           vocab ? vocab->net->node_count : -1,
           concept ? concept->net->node_count : -1,
           m->cross_link_count);

    if (strcmp(argv[2], "dups") == 0) { int rc = dump_dups(vocab ? vocab->net : NULL, 12); master_topology_destroy(m); return rc; }
    if (strcmp(argv[2], "wordbind") == 0) { int rc = dump_wordbind(m, vocab ? vocab->net : NULL, concept ? concept->net : NULL); master_topology_destroy(m); return rc; }
    if (strcmp(argv[2], "top") == 0) { double fl = (argc > 3) ? atof(argv[3]) : 1.5; int rc = dump_top_pairs(vocab ? vocab->net : NULL, fl); master_topology_destroy(m); return rc; }
    if (strcmp(argv[2], "pairs") == 0) { int rc = dump_pairs(vocab ? vocab->net : NULL, concept ? concept->net : NULL); master_topology_destroy(m); return rc; }
    for (int a = 2; a < argc; a++) {
        const char* w = argv[a];
        int vi = vocab   ? huarong_net_find_concept(vocab->net, w)   : -1;
        int ci = concept ? huarong_net_find_concept(concept->net, w) : -1;
        printf("词 「%s」:\n", w);
        printf("  词汇拓扑命中 : %s%s", vi >= 0 ? "是" : "否（锚定路径找不到它）",
               vi >= 0 ? "" : "\n");
        if (vi >= 0) {
            ReasoningNode* vn = vocab->net->nodes[vi];
            printf("  id=%d 度=%d\n", vi, vn ? vn->edge_count : -1);
        }
        printf("  概念拓扑命中 : %s", ci >= 0 ? "是" : "否\n");
        if (ci >= 0) {
            ReasoningNode* cn = concept->net->nodes[ci];
            printf("  id=%d 度=%d\n", ci, cn ? cn->edge_count : -1);
            /* 从概念词节点出发的 cross-link（应连回组成它的字） */
            int hit = 0;
            for (int li = 0; li < m->cross_link_count; li++) {
                CrossTopologyLink* l = m->cross_links[li];
                if (!l || l->from_topo_id != TOPO_CONCEPT || l->from_node_id != ci) continue;
                printf("    cross-link → topo=%d node=%d w=%.3f type=%s\n",
                       l->to_topo_id, l->to_node_id, (double)l->weight,
                       l->relation ? l->relation : "?");
                hit++;
                if (hit >= 6) break;
            }
            if (!hit) printf("    （无 cross-link）\n");
        }
        /* 反向：从字节点看能否到达词节点（Anchor 若按字反查词，需要这条） */
        printf("\n");
    }
    master_topology_destroy(m);
    return 0;
}
