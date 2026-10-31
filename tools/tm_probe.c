/* tools/tm_probe.c — 转移表质量探针（这一步决定「沿边生成」这条路成不成立）
 *
 * 用法: ./tm_probe <state_file> <transitions.bin> [词1 词2 ...]
 *
 * 判据（人工可判，不需要额外基线）：
 *   · 给一个词，它的 top-5 后继应当【语法上接得上、语义上同场景】；
 *   · 若后继全是「的/了/是/我」这类泛用词 ⇒ 语料是闲聊垃圾，这条路走不通；
 *   · 若后继具体（`雪` → 山/花/人/地）⇒ 数据可用，可以做沿边生成。
 *
 * 另外打印每个词的「出边基尼」：出边分布越平，说明该词的下一词越不可预测
 * （典型如「的」）。这个量就是「左熵」的离散版，也是打分时该惩罚的东西。
 */
#include "pivotmind_paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "multi_topology.h"
#include "huarong_topology.h"
#include "transition_model.h"
#include "word_embed.h"   /* 走链的全局约束：指纹相似度 */
#include "common.h"

static MasterTopology* load_master(const char* path) {
    MasterTopology* master = master_topology_create(16);
    if (!master) return NULL;
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
    if (master_load_state(master, path) < 0) {
        master_topology_destroy(master);
        return NULL;
    }
    return master;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "用法: %s <state_file> <transitions.bin> [词...]\n", argv[0]);
        return 2;
    }
    MasterTopology* master = load_master(argv[1]);
    if (!master) { fprintf(stderr, "状态加载失败\n"); return 1; }

    SubTopology* vocab = NULL;
    for (int t = 0; t < master->sub_topo_count; t++)
        if (master->sub_topologies[t] && master->sub_topologies[t]->type == TOPO_VOCABULARY) {
            vocab = master->sub_topologies[t]; break;
        }
    if (!vocab || !vocab->net) { fprintf(stderr, "vocab 缺失\n"); return 1; }

    TransitionModel* tm = tm_create();
    if (!tm) { fprintf(stderr, "tm_create 失败\n"); return 1; }
    if (tm_load(tm, argv[2]) != 0) { fprintf(stderr, "转移表加载失败: %s\n", argv[2]); return 1; }
    printf("[tm] 转移表 %d 个唯一对\n\n", tm_pair_count(tm));

    /* 默认测这组词：覆盖实词/虚词/多字词/单字 */
    static const char* dft[] = {
        "雪", "山", "电脑", "学习", "朋友", "时间", "爱", "心",
        "的", "了", "是", "我", "不", NULL
    };
    const char** words = dft;
    int nw = 0;
    if (argc >= 4) { words = (const char**)&argv[3]; nw = argc - 3; }
    else { while (dft[nw]) nw++; }

    for (int i = 0; i < nw; i++) {
        const char* w = words[i];
        int id = huarong_net_find_concept(vocab->net, w);
        if (id < 0) { printf("%-8s 不在词表\n", w); continue; }
        uint32_t out = tm_out_count(tm, id);
        if (out == 0) { printf("%-8s 出边 0（表里没有这条）\n", w); continue; }

        int nid[8];
        int k = tm_top_next(tm, id, nid, 8);
        printf("%-8s 出边=%u · top-%d:", w, out, k);
        double H = 0.0;
        int shown = 0;
        for (int j = 0; j < k && shown < 5; j++) {
            if (nid[j] < 0 || nid[j] >= vocab->net->node_count) continue;
            ReasoningNode* nx = vocab->net->nodes[nid[j]];
            if (!nx || !nx->concept) continue;
            float pr = tm_prob(tm, id, nid[j]);
            printf("  %s(%g)", nx->concept, (double)pr);
            if (pr > 1e-9f) H -= (double)pr * log2((double)pr);
            shown++;
        }
        printf("   | 前8项熵=%.2fbit\n", H);
    }

    /* ── 走链实验（这条路最直接的判据）──
     * 从起始词出发，每步贪心选 P(next|prev) 最高的后继，不走回头路。
     * 如果产出的链像话，说明「沿边逐步生成」可行；如果立刻死循环或跳到
     * 不相干的词，说明转移表本身没有主干。 */
    if (getenv("TM_WALK")) {
        const char* start = getenv("TM_WALK");
        int steps = 8;
        const char* se = getenv("TM_STEPS");
        if (se && atoi(se) > 0) steps = atoi(se);
        int sid = huarong_net_find_concept(vocab->net, start);
        printf("\n[tm] 走链 from「%s」%d 步:\n  %s", start, steps, start);
        int visited[64]; int nv = 0;
        int cur = sid;
        /* 全局约束的锚点（TM_ANCHOR，默认同起始词）。用指纹相似度当「主线」度量：
         * 局部转移概率管「接得上」，指纹管「还在说这件事」。 */
        const char* anchor = getenv("TM_ANCHOR");
        if (!anchor || !anchor[0]) anchor = start;
        for (int s = 0; s < steps && cur >= 0; s++) {
            int nid[24];
            int k = tm_top_next(tm, cur, nid, 24);
            int pick = -1; float best = -1.0f; float best_pr = 0.0f;
            for (int j = 0; j < k; j++) {
                int c = nid[j];
                if (c < 0 || c >= vocab->net->node_count) continue;
                int seen = 0;
                for (int v = 0; v < nv; v++) if (visited[v] == c) { seen = 1; break; }
                if (seen) continue;
                ReasoningNode* cand = vocab->net->nodes[c];
                if (!cand || !cand->concept) continue;
                float pr = tm_prob(tm, cur, c);
                float sc = pr;
                float sim = wemb_similar(cand->concept, anchor);
                if (sim > 0.0f) sc = pr * (0.2f + 2.0f * sim);   /* sim 0.1 → ×0.4；0.5 → ×1.2 */
                if (sc > best) { best = sc; pick = c; best_pr = pr; }
            }
            if (pick < 0) { printf(" → (断)"); break; }
            ReasoningNode* nx = vocab->net->nodes[pick];
            printf(" → %s(%.4f)", nx && nx->concept ? nx->concept : "?", (double)best_pr);
            if (nv < 64) visited[nv++] = cur;
            cur = pick;
        }
        printf("\n");
    }

    /* ── 碎片反查（TM_SCAN=<阈值>）──
     * 判据：如果一个「词」的后继能以极高概率只接一个特定字，说明它跨了词边界
     * ——「公吵」几乎总接「架」，正因为真正的词是「吵架」。
     * 这里把「多字词 → 单字」的高概率转移全列出来，就是一份切分错误清单。 */
    if (getenv("TM_SCAN")) {
        float thr = (float)atof(getenv("TM_SCAN"));
        int maxout = 80;
        const char* mo = getenv("TM_SCAN_MAX");
        if (mo && atoi(mo) > 0) maxout = atoi(mo);
        printf("\n[tm] 碎片反查: prev 是多字词、next 是单字、P >= %.2f\n", (double)thr);
        printf("%-10s → %-6s  P        prev出边  判读\n", "prev", "next");
        int shown = 0, hits = 0;
        for (int i = 0; i < vocab->net->node_count && shown < maxout; i++) {
            ReasoningNode* n0 = vocab->net->nodes[i];
            if (!n0 || !n0->concept) continue;
            size_t L0 = strlen(n0->concept);
            if (L0 < 6) continue;                 /* prev 必须多字词 */
            int nid1[4];
            if (tm_top_next(tm, i, nid1, 4) < 1) continue;
            int c = nid1[0];
            if (c < 0 || c >= vocab->net->node_count) continue;
            ReasoningNode* n1 = vocab->net->nodes[c];
            if (!n1 || !n1->concept) continue;
            if (strlen(n1->concept) != 3) continue;   /* next 必须单字 */
            float pr = tm_prob(tm, i, c);
            if (pr < thr) continue;
            hits++;
            printf("%-10s → %-6s  %.3f    %-8u  %s%s\n",
                   n0->concept, n1->concept, (double)pr, tm_out_count(tm, i),
                   "疑似跨词界：", n0->concept);
            shown++;
        }
        printf("[tm] 命中 %d 个（显示 %d）\n", hits, shown);
    }

    /* 判读说明 */
    printf("\n[tm] 读法：\n");
    printf("  · 实词（雪/电脑/朋友）的后继应当具体 —— 「雪」→ 山/花/人/地 才算可用；\n");
    printf("  · 若实词后继也是「的/了/是」这类 ⇒ 语料是闲聊噪声，沿边生成走不通；\n");
    printf("  · 虚词（的/了/是）出边基尼高、熵大是正常的（它们接什么都行）。\n");

    tm_destroy(tm);
    master_topology_destroy(master);
    return 0;
}
