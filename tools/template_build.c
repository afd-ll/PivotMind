/**
 * @file template_build.c
 * @brief Step 2: 模板构建工具 — 路径分析 + 软聚类 + 模板节点生成
 *
 * 用法:
 *   ./build/bin/template_build [state_file] [out_file] [num_starts] [walks_per]
 *
 * 默认:
 *   state_file = pivotmind_state.dat
 *   out_file   = pivotmind_state_with_templates.dat
 *   num_starts = 500
 *   walks_per  = 3
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "multi_topology.h"
#include "huarong_topology.h"
#include "path_encoding.h"
#include "template_builder.h"

#define MAX_WALK_STEPS      20
#define DEFAULT_NUM_STARTS  500
#define DEFAULT_WALKS_PER   3

/* ============================================================
 *   辅助: 节点概念字符串
 * ============================================================ */

static const char* node_concept(ReasoningNode* node, int max_len) {
    static char buf[32];
    if (!node || !node->concept) return "?";
    int len = (int)strlen(node->concept);
    if (len <= max_len) return node->concept;
    memcpy(buf, node->concept, (size_t)max_len);
    buf[max_len] = '\0';
    return buf;
}

/* ============================================================
 *   辅助: 按连接数排序
 * ============================================================ */

typedef struct {
    int node_id;
    int conn_count;
} NodeRank;

static int rank_cmp_desc(const void* a, const void* b) {
    int ca = ((const NodeRank*)a)->conn_count;
    int cb = ((const NodeRank*)b)->conn_count;
    return (ca < cb) ? 1 : (ca > cb) ? -1 : 0;
}


/* ============================================================
 *   主函数
 * ============================================================ */

int main(int argc, char** argv) {
    const char* state_file = "pivotmind_state.dat";
    const char* out_file   = "pivotmind_state_with_templates.dat";
    int num_starts  = DEFAULT_NUM_STARTS;
    int walks_per   = DEFAULT_WALKS_PER;

    if (argc > 1) state_file  = argv[1];
    if (argc > 2) out_file    = argv[2];
    if (argc > 3) num_starts  = atoi(argv[3]);
    if (argc > 4) walks_per   = atoi(argv[4]);

    printf("========================================\n");
    printf("  PivotMind Template Builder\n");
    printf("  Step 2: Soft clustering + template gen\n");
    printf("========================================\n\n");
    printf("State file : %s\n", state_file);
    printf("Output file: %s\n", out_file);
    printf("Start nodes: %d\n", num_starts);
    printf("Walks/start: %d\n\n", walks_per);

    /* ---- 1. 加载状态 ---- */
    MasterTopology* master = master_topology_create(16);
    if (!master) {
        fprintf(stderr, "ERROR: master_topology_create failed\n");
        return 1;
    }

    /* 创建子拓扑 (load 前必须) */
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 6000, 10);
    master_add_sub_topology(master, TOPO_SEMANTIC, "语义拓扑", 2000, 9);
    master_add_sub_topology(master, TOPO_EMOTION, "情绪拓扑", 500, 8);
    master_add_sub_topology(master, TOPO_SYNTAX, "语法拓扑", 500, 7);
    master_add_sub_topology(master, TOPO_CONTEXT, "上下文拓扑", 500, 6);
    master_add_sub_topology(master, TOPO_DOMAIN, "领域拓扑", 500, 5);
    master_add_sub_topology(master, TOPO_PRAGMA, "语用拓扑", 500, 4);
    master_add_sub_topology(master, TOPO_CULTURE, "文化拓扑", 500, 3);
    master_add_sub_topology(master, TOPO_CONCEPT, "概念拓扑", 6000, 9);
    /* 占位：保证 TOPO_TEMPLATE 获得正确的 topo_id=10 */
    master_add_sub_topology(master, TOPO_MASTER, "主拓扑", 100, 0);
    /* 新增: 模板拓扑 */
    master_add_sub_topology(master, TOPO_TEMPLATE, "模板拓扑", 2000, 8);

    printf("Loading state... ");
    fflush(stdout);
    if (master_load_state(master, state_file) < 0) {
        fprintf(stderr, "\nERROR: Failed to load state from '%s'\n", state_file);
        master_topology_destroy(master);
        return 1;
    }
    printf("OK\n");

    /* ---- 2. 获取词汇拓扑 + 统计 ---- */
    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    if (!vocab || !vocab->net || vocab->net->node_count == 0) {
        fprintf(stderr, "ERROR: Vocabulary topology not found\n");
        master_topology_destroy(master);
        return 1;
    }

    HuarongTopologyNet* net = vocab->net;
    int node_count = net->node_count;
    printf("Vocabulary nodes: %d\n", node_count);
    printf("Cross links     : %d\n\n", master->cross_link_count);

    /* ---- 3. 选起点 ---- */
    printf("Selecting top-%d most-connected nodes... ", num_starts);
    fflush(stdout);

    NodeRank* ranks = (NodeRank*)malloc((size_t)node_count * sizeof(NodeRank));
    if (!ranks) { fprintf(stderr, "malloc fail\n"); master_topology_destroy(master); return 1; }
    int rk = 0;
    for (int i = 0; i < node_count; i++) {
        ReasoningNode* nd = net->nodes[i];
        if (!nd || nd->connection_count <= 0) continue;
        ranks[rk].node_id = i;
        ranks[rk].conn_count = nd->connection_count;
        rk++;
    }
    qsort(ranks, rk, sizeof(NodeRank), rank_cmp_desc);
    int actual = (num_starts < rk) ? num_starts : rk;
    printf("%d (range: %d-%d)\n", actual, ranks[0].conn_count, ranks[actual-1].conn_count);

    /* ---- 4. 走边采集路径 ---- */
    PathFrequencyTable* freq = path_freq_table_create(PATH_TRIPLET_TABLE_SIZE);
    if (!freq) { fprintf(stderr, "freq table fail\n"); free(ranks); master_topology_destroy(master); return 1; }

    printf("Walking paths (~%d walks)... ", actual * walks_per);
    fflush(stdout);
    int bms = (node_count + 7) / 8;

    for (int s = 0; s < actual; s++) {
        int sid = ranks[s].node_id;
        ReasoningNode* sn = net->nodes[sid];
        if (!sn) continue;

        for (int w = 0; w < walks_per; w++) {
            float saved = sn->activation;
            sn->activation = 0.8f;
            unsigned char* vis = (unsigned char*)calloc((size_t)bms, 1);
            int pn[32]; float ps[32];
            int pl = topology_walk_greedy(vocab, sid, pn, ps, MAX_WALK_STEPS,
                                          vis, 1.0f, master, NULL, NULL);
            sn->activation = saved;
            if (pl >= 3) {
                for (int p = 0; p < pl - 2; p++)
                    path_freq_table_record(freq, vocab->topo_id,
                                           pn[p], pn[p+1], pn[p+2]);
            }
            free(vis);
        }
    }
    printf("OK (unique: %d/%d)\n", freq->entry_count, freq->capacity);
    free(ranks);

    /* ---- 5. 不可分解性分析 ---- */
    printf("Computing irreducibility... ");
    fflush(stdout);
    int result_count = 0;
    IrreducibilityResult* results = path_analyze_irreducibility(
        freq, (void* const*)net->nodes, node_count, &result_count);
    printf("%d results\n", result_count);

    if (!results || result_count == 0) {
        fprintf(stderr, "No irreducibility results\n");
        path_freq_table_destroy(freq);
        master_topology_destroy(master);
        return 1;
    }

    /* ---- 6. 前缀分组 ---- */
    TemplateBuildConfig cfg = template_config_default();
    cfg.min_ratio = TEMPLATE_MIN_RATIO;
    cfg.similarity_threshold = TEMPLATE_SIMILARITY_THRESHOLD;
    cfg.min_cluster_size  = TEMPLATE_MIN_CLUSTER_SIZE;
    cfg.max_templates     = 200;

    printf("Grouping triplets by prefix... ");
    fflush(stdout);
    int group_count = 0;
    TripletPrefixGroup* groups = template_group_triplets(
        results, result_count,
        (ReasoningNode* const*)net->nodes, node_count,
        &cfg, &group_count);
    printf("%d prefix groups\n", group_count);

    /* ---- 7. 软聚类 ---- */
    printf("Soft clustering within groups... ");
    fflush(stdout);
    int cluster_count = 0;
    TemplateCluster* clusters = template_cluster_groups(
        groups, group_count,
        (ReasoningNode* const*)net->nodes, node_count,
        &cfg, &cluster_count);
    printf("%d template clusters\n", cluster_count);

    /* ---- 8. 创建模板节点 ---- */
    printf("Building template nodes... ");
    fflush(stdout);
    int built = template_build_nodes(master, clusters, cluster_count,
                                     vocab, cfg.max_templates);
    printf("%d nodes created\n", built);

    /* ---- 9. 输出报告 ---- */
    printf("\n========================================\n");
    printf("  Template Clusters (top 40)\n");
    printf("========================================\n\n");

    int show = (cluster_count < 40) ? cluster_count : 40;
    for (int i = 0; i < show; i++) {
        TemplateCluster* c = &clusters[i];
        ReasoningNode* na = (c->node_a >= 0 && c->node_a < node_count)
                           ? net->nodes[c->node_a] : NULL;
        ReasoningNode* nb = (c->node_b >= 0 && c->node_b < node_count)
                           ? net->nodes[c->node_b] : NULL;
        ReasoningNode* nc = (c->representative_c >= 0 && c->representative_c < node_count)
                           ? net->nodes[c->representative_c] : NULL;

        printf("%2d. %s→%s→%s  | members=%d  count=%d  node_id=%d\n",
               i+1,
               node_concept(na, 4), node_concept(nb, 4), node_concept(nc, 4),
               c->member_count, c->total_count, c->template_node_id);
    }

    /* 统计 */
    printf("\n========================================\n");
    printf("  Summary\n");
    printf("========================================\n");
    printf("Triplets analyzed  : %d\n", result_count);
    printf("Prefix groups      : %d\n", group_count);
    printf("Template clusters  : %d\n", cluster_count);
    printf("Template nodes     : %d\n", built);

    SubTopology* templ = master_get_sub_topology_by_type(master, TOPO_TEMPLATE);
    if (templ && templ->net) {
        printf("TOPO_TEMPLATE nodes: %d\n", templ->net->node_count);
    }
    printf("Cross links (total): %d\n", master->cross_link_count);

    /* ---- 10. 保存带模板的状态 ---- */
    printf("\nSaving state with templates to: %s... ", out_file);
    fflush(stdout);
    if (master_save_state(master, out_file) >= 0) {
        printf("OK\n");
        printf("  (original state preserved in original file)\n");
    } else {
        printf("FAILED\n");
    }

    /* ---- 清理 ---- */
    template_free_clusters(clusters, cluster_count);
    template_free_groups(groups, group_count);
    free(results);
    path_freq_table_destroy(freq);
    master_topology_destroy(master);

    printf("\nDone. %d templates built.\n", built);
    return 0;
}
