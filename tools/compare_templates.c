/**
 * @file compare_templates.c
 * @brief Step 3: 模板投票效果对比 (直接构建，绕过 save/load)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multi_topology.h"
#include "huarong_topology.h"
#include "path_encoding.h"
#include "template_builder.h"

#define MAX_WALK_STEPS 20

static const char* TEST_QUERIES[] = {
    "学习", "量子", "世界", "开心", "今天", "什么", "怎么",
    "好", "吃", "喜欢", "知道", "为什么", "可以", "谢谢",
    "天气", "朋友", NULL
};

static void path_to_string(int* path, int len, ReasoningNode** nodes,
                           char* out, int max_len) {
    int pos = 0;
    for (int i = 0; i < len && pos < max_len - 4; i++) {
        ReasoningNode* nd = nodes[path[i]];
        const char* c = nd && nd->concept ? nd->concept : "?";
        int cl = (int)strlen(c);
        if (pos + cl < max_len - 2) {
            if (i > 0) { out[pos++] = '-'; out[pos++] = '>'; }
            memcpy(out + pos, c, cl); pos += cl;
        }
    }
    out[pos] = '\0';
}

typedef struct { int node_id; int conn_count; } NodeRank;
static int rank_cmp_desc(const void* a, const void* b) {
    return ((const NodeRank*)b)->conn_count - ((const NodeRank*)a)->conn_count;
}

static int run_walk(MasterTopology* master, SubTopology* vocab,
                    int start_id, int use_tpl,
                    int* path, float* scores, unsigned char* vis, int nc) {
    int bms = (nc + 7) / 8;
    memset(vis, 0, (size_t)bms);
    ReasoningNode* sn = vocab->net->nodes[start_id];
    float saved = sn ? sn->activation : 0;
    if (sn) sn->activation = 0.8f;
    master->use_template_voting = use_tpl;
    int pl = topology_walk_greedy(vocab, start_id, path, scores,
                                  MAX_WALK_STEPS, vis, 1.0f, master, NULL, NULL);
    master->use_template_voting = 0;
    if (sn) sn->activation = saved;
    return pl;
}

int main(int argc, char** argv) {
    const char* state_file = "pivotmind_state.dat";
    if (argc > 1) state_file = argv[1];

    printf("========================================\n");
    printf("  Template Voting Test (Direct)\n");
    printf("========================================\n\n");

    MasterTopology* master = master_topology_create(16);
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 6000, 10);
    master_add_sub_topology(master, TOPO_SEMANTIC, "语义拓扑", 2000, 9);
    master_add_sub_topology(master, TOPO_EMOTION, "情绪拓扑", 500, 8);
    master_add_sub_topology(master, TOPO_SYNTAX, "语法拓扑", 500, 7);
    master_add_sub_topology(master, TOPO_CONTEXT, "上下文拓扑", 500, 6);
    master_add_sub_topology(master, TOPO_DOMAIN, "领域拓扑", 500, 5);
    master_add_sub_topology(master, TOPO_PRAGMA, "语用拓扑", 500, 4);
    master_add_sub_topology(master, TOPO_CULTURE, "文化拓扑", 500, 3);
    master_add_sub_topology(master, TOPO_CONCEPT, "概念拓扑", 6000, 9);
    master_add_sub_topology(master, TOPO_MASTER, "主拓扑", 100, 0);   /* 占位：保证 TOPO_TEMPLATE 获得正确的 topo_id=10 */
    master_add_sub_topology(master, TOPO_TEMPLATE, "模板拓扑", 2000, 8);

    printf("Loading: %s\n", state_file);
    if (master_load_state(master, state_file) < 0) { fprintf(stderr, "FAIL\n"); return 1; }

    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    SubTopology* tpl   = master_get_sub_topology_by_type(master, TOPO_TEMPLATE);
    int nc = vocab->net->node_count;
    printf("Vocab nodes: %d  Cross links: %d\n\n", nc, master->cross_link_count);

    /* Build templates */
    printf("Building templates (top 100 nodes, 2 walks each)... ");
    fflush(stdout);
    NodeRank* ranks = (NodeRank*)malloc((size_t)nc * sizeof(NodeRank));
    int rk = 0;
    for (int i = 0; i < nc; i++) {
        ReasoningNode* nd = vocab->net->nodes[i];
        if (nd && nd->connection_count > 0) { ranks[rk].node_id = i; ranks[rk].conn_count = nd->connection_count; rk++; }
    }
    qsort(ranks, rk, sizeof(NodeRank), rank_cmp_desc);
    int ns = (100 < rk) ? 100 : rk;

    PathFrequencyTable* freq = path_freq_table_create(PATH_TRIPLET_TABLE_SIZE);
    int bms = (nc + 7) / 8;
    for (int s = 0; s < ns; s++) {
        ReasoningNode* sn = vocab->net->nodes[ranks[s].node_id];
        if (!sn) continue;
        for (int w = 0; w < 2; w++) {
            float sa = sn->activation; sn->activation = 0.8f;
            unsigned char* vis = (unsigned char*)calloc((size_t)bms, 1);
            int pn[32]; float ps[32];
            int pl = topology_walk_greedy(vocab, ranks[s].node_id, pn, ps, 20, vis, 1.0f, master, NULL, NULL);
            sn->activation = sa;
            for (int p = 0; p < pl - 2; p++) path_freq_table_record(freq, vocab->topo_id, pn[p], pn[p+1], pn[p+2]);
            free(vis);
        }
    }
    free(ranks);

    int rc = 0;
    IrreducibilityResult* res = path_analyze_irreducibility(freq, (void* const*)vocab->net->nodes, nc, &rc);
    TemplateBuildConfig cfg = template_config_default();
    cfg.max_templates = 50;
    int gc = 0;
    TripletPrefixGroup* grps = template_group_triplets(res, rc, (ReasoningNode* const*)vocab->net->nodes, nc, &cfg, &gc);
    int cc = 0;
    TemplateCluster* clus = template_cluster_groups(grps, gc, (ReasoningNode* const*)vocab->net->nodes, nc, &cfg, &cc);
    int built = template_build_nodes(master, clus, cc, vocab, cfg.max_templates);
    template_free_clusters(clus, cc);
    template_free_groups(grps, gc);
    free(res);
    path_freq_table_destroy(freq);
    printf("OK (%d templates, %d cross links)\n\n", built, master->cross_link_count);

    /* Compare paths */
    printf("========================================\n");
    printf("  Path Comparison\n");
    printf("========================================\n\n");

    int bms2 = (nc + 7) / 8;
    unsigned char* gv = (unsigned char*)calloc((size_t)bms2, 1);
    int compared = 0, different = 0;

    for (int qi = 0; TEST_QUERIES[qi] != NULL; qi++) {
        const char* q = TEST_QUERIES[qi];
        ReasoningNode* qn = NULL;
        for (int n = 0; n < nc; n++) {
            if (vocab->net->nodes[n] && vocab->net->nodes[n]->concept &&
                strcmp(vocab->net->nodes[n]->concept, q) == 0) { qn = vocab->net->nodes[n]; break; }
        }
        if (!qn) continue;
        compared++;

        int p0[32]; float s0[32]; int l0 = run_walk(master, vocab, qn->node_id, 0, p0, s0, gv, nc);
        int p1[32]; float s1[32]; int l1 = run_walk(master, vocab, qn->node_id, 1, p1, s1, gv, nc);

        char str0[256], str1[256];
        path_to_string(p0, l0, vocab->net->nodes, str0, sizeof(str0));
        path_to_string(p1, l1, vocab->net->nodes, str1, sizeof(str1));

        int same = (l0 == l1);
        if (same) for (int i = 0; i < l0; i++) if (p0[i] != p1[i]) { same = 0; break; }

        if (same) {
            printf("  [%s] SAME (%d): %s\n", q, l0, str0);
        } else {
            different++;
            printf("  [%s] DIFF (%d vs %d)\n", q, l0, l1);
            printf("    OFF : %s\n", str0);
            printf("    ON  : %s\n", str1);
        }
    }
    free(gv);

    printf("\n--- Summary ---\n");
    printf("Compared: %d  Changed: %d (%.0f%%)\n", compared, different,
           compared ? (100.0f * different / compared) : 0.0f);

    /* Show template samples */
    if (tpl && tpl->net && tpl->net->node_count > 0) {
        printf("\n--- Template samples ---\n");
        int show = (tpl->net->node_count < 8) ? tpl->net->node_count : 8;
        for (int i = 0; i < show; i++) {
            ReasoningNode* tn = tpl->net->nodes[i];
            if (tn) printf("  [%d] \"%s\"\n", i, tn->concept);
        }
    }

    master_topology_destroy(master);
    return 0;
}
