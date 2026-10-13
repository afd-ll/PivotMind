/**
 * probe_semscore.c — 离线复刻 cingulate 的语义一致性分（只读）
 *
 * 口径对齐（重要）：本系统的入口是【逐字】的（utf8_tokenize 一字一 token），
 * 所以本工具也按**单字**取输入锚点、按**单字**取回复词，再套 cingulate
 * semantic_consistency 的两档判据：
 *     回复词与任一输入字的边权 >= 0.5 → 1.0
 *                            >= 0.3 → 0.5
 *                            否则   → 0.1
 *   总分 = 各回复字得分的平均。
 *
 * 用途：给「喂料质量闸门（丙）」定阈值提供**可数**依据。
 * 用法: probe_semscore <state.dat> "输入|回复" ["输入|回复" ...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multi_topology.h"
#include "huarong_topology.h"
#include "pivotmind_paths.h"
#include "lang.h"

#define MAXTOK 256

int main(int argc, char** argv) {
    if (argc < 3) { printf("用法: probe_semscore <state.dat> \"输入|回复\" ...\n"); return 1; }

    MasterTopology* m = master_topology_create(11);
    master_add_sub_topology(m, TOPO_VOCABULARY, "词汇", 100000, 10);
    master_add_sub_topology(m, TOPO_SEMANTIC,   "语义", 50000, 9);
    master_add_sub_topology(m, TOPO_EMOTION,    "情绪", 4000, 8);
    master_add_sub_topology(m, TOPO_SYNTAX,     "语法", 1000, 7);
    master_add_sub_topology(m, TOPO_CONTEXT,    "上下文", 1000, 6);
    master_add_sub_topology(m, TOPO_DOMAIN,     "领域", 1000, 5);
    master_add_sub_topology(m, TOPO_PRAGMA,     "语用", 1000, 4);
    master_add_sub_topology(m, TOPO_CULTURE,    "文化", 1000, 3);
    master_add_sub_topology(m, TOPO_CONCEPT,    "概念", 50000, 9);
    master_add_sub_topology(m, TOPO_MASTER,     "主", 100, 0);
    master_add_sub_topology(m, TOPO_TEMPLATE,   "模板", 20000, 8);
    int n = master_load_state(m, argv[1]);
    if (n <= 10) { printf("× 状态加载失败 (%d)\n", n); return 1; }

    SubTopology* vocab = NULL;
    for (int i = 0; i < m->sub_topo_count; i++)
        if (m->sub_topologies[i] && m->sub_topologies[i]->type == TOPO_VOCABULARY)
            { vocab = m->sub_topologies[i]; break; }
    if (!vocab) { printf("× 无词汇拓扑\n"); return 1; }
    printf("✓ 状态 %d 节点 / 词汇 %d 节点\n\n", n, vocab->net->node_count);
    printf("  %-7s %-4s %-4s %s\n", "分值", "命中", "字数", "输入 → 回复");

    float sum = 0; int cnt = 0;
    int bucket[11] = {0};   /* 分值取整分布 */
    for (int a = 2; a < argc; a++) {
        const char* pair = argv[a];
        const char* bar = strchr(pair, '|');
        if (!bar) continue;
        char inbuf[512]; int il = (int)(bar - pair);
        if (il > 511) il = 511;
        memcpy(inbuf, pair, (size_t)il); inbuf[il] = '\0';
        const char* rep = bar + 1;

        /* 输入：逐字取锚点 */
        int in_ids[MAXTOK], in_n = 0;
        for (const char* p = inbuf; *p && in_n < MAXTOK; ) {
            unsigned cp = 0; int b = pm_utf8_decode(p, &cp);
            if (b <= 0) b = 1;
            char one[8] = {0};
            if (b < 8) { memcpy(one, p, (size_t)b); one[b] = '\0'; }
            p += b;
            if (strlen(one) != 3) continue;         /* 只收中文单字 */
            int id = huarong_net_find_concept(vocab->net, one);
            if (id >= 0) in_ids[in_n++] = id;
        }

        /* 回复：逐字评分 */
        float total = 0; int used = 0;
        for (const char* p = rep; *p; ) {
            unsigned cp = 0; int b = pm_utf8_decode(p, &cp);
            if (b <= 0) b = 1;
            char one[8] = {0};
            if (b < 8) { memcpy(one, p, (size_t)b); one[b] = '\0'; }
            p += b;
            if (strlen(one) != 3) continue;
            int gid = huarong_net_find_concept(vocab->net, one);
            if (gid < 0) continue;
            ReasoningNode* gn = vocab->net->nodes[gid];
            if (!gn) continue;
            float best = 0.0f;
            for (int i = 0; i < in_n; i++) {
                ReasoningNode* inode = vocab->net->nodes[in_ids[i]];
                if (!inode) continue;
                for (int e = 0; e < inode->edge_count; e++) {
                    if (inode->edges[e].target &&
                        inode->edges[e].target->node_id == gid) {
                        if (inode->edges[e].weight > best) best = inode->edges[e].weight;
                        break;
                    }
                }
            }
            total += (best >= 0.5f) ? 1.0f : (best >= 0.3f) ? 0.5f : 0.1f;
            used++;
        }
        float score = used > 0 ? total / (float)used : 0.0f;
        sum += score; cnt++;
        int bi = (int)(score * 10); if (bi > 10) bi = 10; if (bi < 0) bi = 0;
        bucket[bi]++;
        printf("  %-7.3f %-4d %-4d %s → %s\n", score, in_n, used, inbuf, rep);
    }
    if (cnt > 0) printf("\n平均分 = %.3f（%d 条）\n", sum / (float)cnt, cnt);
    printf("分档计数(0.0~1.0 每 0.1)：");
    for (int i = 0; i <= 10; i++) printf(" %.1f:%d", i / 10.0, bucket[i]);
    printf("\n");
    master_topology_destroy(m);
    return 0;
}
