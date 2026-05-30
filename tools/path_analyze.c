/**
 * @file path_analyze.c
 * @brief Step 1: 路径不可分解性分析工具
 *
 * 从现有状态文件加载拓扑，从高连通节点出发走边采集路径，
 * 统计三元组频率，计算"不可分解性"比率，验证该指标是否
 * 能区分有意义路径和随机共现路径。
 *
 * 用法: ./build/bin/path_analyze [state_file] [num_starts] [walks_per_start]
 *   默认: state_file = pivotmind_state.dat, starts = 500, walks = 3
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "multi_topology.h"
#include "huarong_topology.h"
#include "path_encoding.h"

#define MAX_WALK_STEPS      20
#define DEFAULT_NUM_STARTS  500
#define DEFAULT_WALKS_PER   3

/* ============================================================
 *   工具函数
 * ============================================================ */

/** 按连接数降序排序的辅助结构 */
typedef struct {
    int node_id;
    int conn_count;
} NodeRank;

static int rank_cmp_desc(const void* a, const void* b) {
    int ca = ((const NodeRank*)a)->conn_count;
    int cb = ((const NodeRank*)b)->conn_count;
    return (ca < cb) ? 1 : (ca > cb) ? -1 : 0;
}

/** 获取节点概念字符串（UTF-8 安全截断到 N 字节） */
static const char* node_concept(ReasoningNode* node, int max_len) {
    static char buf[32];
    if (!node || !node->concept) return "?";
    int len = (int)strlen(node->concept);
    if (len <= max_len) return node->concept;
    /* UTF-8 安全截断: 最多复制 max_len 字节 */
    memcpy(buf, node->concept, (size_t)max_len);
    buf[max_len] = '\0';
    return buf;
}


/* ============================================================
 *   主函数
 * ============================================================ */

int main(int argc, char** argv) {
    const char* state_file = "pivotmind_state.dat";
    int num_starts  = DEFAULT_NUM_STARTS;
    int walks_per   = DEFAULT_WALKS_PER;

    if (argc > 1) state_file  = argv[1];
    if (argc > 2) num_starts  = atoi(argv[2]);
    if (argc > 3) walks_per   = atoi(argv[3]);

    printf("========================================\n");
    printf("  PivotMind Path Irreducibility Analyzer\n");
    printf("  Step 1: Statistical verification\n");
    printf("========================================\n\n");
    printf("State file : %s\n", state_file);
    printf("Start nodes: %d\n", num_starts);
    printf("Walks/start: %d\n", walks_per);
    printf("Max steps  : %d\n\n", MAX_WALK_STEPS);

    /* ---- 1. 加载状态 ---- */
    MasterTopology* master = master_topology_create(16);
    if (!master) {
        fprintf(stderr, "ERROR: master_topology_create failed\n");
        return 1;
    }

    printf("Loading state... ");
    fflush(stdout);

    /* 必须预先创建所有子拓扑，master_load_state 才能把节点放对位置 */
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 6000, 10);
    master_add_sub_topology(master, TOPO_SEMANTIC, "语义拓扑", 2000, 9);
    master_add_sub_topology(master, TOPO_EMOTION, "情绪拓扑", 500, 8);
    master_add_sub_topology(master, TOPO_SYNTAX, "语法拓扑", 500, 7);
    master_add_sub_topology(master, TOPO_CONTEXT, "上下文拓扑", 500, 6);
    master_add_sub_topology(master, TOPO_DOMAIN, "领域拓扑", 500, 5);
    master_add_sub_topology(master, TOPO_PRAGMA, "语用拓扑", 500, 4);
    master_add_sub_topology(master, TOPO_CULTURE, "文化拓扑", 500, 3);
    master_add_sub_topology(master, TOPO_CONCEPT, "概念拓扑", 6000, 9);

    if (master_load_state(master, state_file) < 0) {
        fprintf(stderr, "\nERROR: Failed to load state from '%s'\n", state_file);
        master_topology_destroy(master);
        return 1;
    }
    printf("OK\n");

    /* ---- 2. 获取词汇拓扑 ---- */
    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    if (!vocab || !vocab->net || vocab->net->node_count == 0) {
        fprintf(stderr, "ERROR: Vocabulary topology not found or empty\n");
        master_topology_destroy(master);
        return 1;
    }

    HuarongTopologyNet* net = vocab->net;
    int node_count = net->node_count;
    printf("Vocabulary nodes: %d\n", node_count);
    printf("Sub-topologies  : %d\n", master->sub_topo_count);
    printf("Cross links     : %d\n\n", master->cross_link_count);

    /* ---- 3. 按连接数排序，取 top-N 起点 ---- */
    printf("Selecting top-%d most-connected nodes... ", num_starts);
    fflush(stdout);

    NodeRank* ranks = (NodeRank*)malloc((size_t)node_count * sizeof(NodeRank));
    if (!ranks) {
        fprintf(stderr, "ERROR: malloc ranks failed\n");
        master_topology_destroy(master);
        return 1;
    }

    int rank_count = 0;
    for (int i = 0; i < node_count; i++) {
        ReasoningNode* node = net->nodes[i];
        if (!node || node->connection_count <= 0) continue;
        ranks[rank_count].node_id    = i;
        ranks[rank_count].conn_count = node->connection_count;
        rank_count++;
    }

    qsort(ranks, rank_count, sizeof(NodeRank), rank_cmp_desc);

    int actual_starts = (num_starts < rank_count) ? num_starts : rank_count;
    printf("selected %d (max conn: %d, min conn: %d)\n",
           actual_starts,
           ranks[0].conn_count,
           ranks[actual_starts - 1].conn_count);

    /* ---- 4. 创建频率表 ---- */
    PathFrequencyTable* freq = path_freq_table_create(PATH_TRIPLET_TABLE_SIZE);
    if (!freq) {
        fprintf(stderr, "ERROR: path_freq_table_create failed\n");
        free(ranks);
        master_topology_destroy(master);
        return 1;
    }

    /* ---- 5. 走边采集路径 ---- */
    printf("Walking paths (total walks: ~%d)... ", actual_starts * walks_per);
    fflush(stdout);

    int bitmap_size = (node_count + 7) / 8;
    long total_walks = 0;
    long total_trips = 0;

    for (int si = 0; si < actual_starts; si++) {
        int start_id = ranks[si].node_id;
        ReasoningNode* start_node = net->nodes[start_id];
        if (!start_node) continue;

        for (int w = 0; w < walks_per; w++) {
            /* 重置起点激活值（给走边算法一个起始"推力"） */
            float saved_act = start_node->activation;
            start_node->activation = 0.8f;

            unsigned char* visited = (unsigned char*)calloc((size_t)bitmap_size, 1);
            int path_nodes[32];
            float path_scores[32];

            int path_len = topology_walk_greedy(
                vocab, start_id, path_nodes, path_scores,
                MAX_WALK_STEPS, visited, 1.0f, master, NULL);

            start_node->activation = saved_act;

            if (path_len >= 3) {
                total_walks++;
                /* 提取三元组: path[i], path[i+1], path[i+2] */
                for (int p = 0; p < path_len - 2; p++) {
                    path_freq_table_record(freq, vocab->topo_id,
                                           path_nodes[p],
                                           path_nodes[p + 1],
                                           path_nodes[p + 2]);
                    total_trips++;
                }
            }

            free(visited);
        }
    }

    printf("OK\n");
    printf("  Total walks     : %ld\n", total_walks);
    printf("  Triplets recorded: %ld (unique: %d/%d)\n",
           total_trips, freq->entry_count, freq->capacity);

    /* ---- 6. 计算不可分解性 ---- */
    printf("Computing irreducibility ratios... ");
    fflush(stdout);

    int result_count = 0;
    IrreducibilityResult* results = path_analyze_irreducibility(
        freq, (void* const*)net->nodes, node_count, &result_count);

    printf("got %d results\n\n", result_count);

    if (!results || result_count == 0) {
        printf("No results to display. Try increasing start nodes or walks.\n");
        free(ranks);
        free(results);
        path_freq_table_destroy(freq);
        master_topology_destroy(master);
        return 0;
    }

    /* ---- 7. 输出报告 ---- */
    printf("========================================\n");
    printf("  Irreducibility Analysis Results\n");
    printf("  (ratio > 3.0 = significant semantic unit)\n");
    printf("========================================\n\n");

    printf("%-6s %-28s %-8s %-12s %s\n",
           "Rank", "Path Triplet", "Count", "Ratio", "Significant?");
    printf("------ ---------------------------- -------- ------------ -----------\n");

    int shown = 0;
    int top_n = (result_count < 60) ? result_count : 60;

    for (int i = 0; i < result_count && shown < top_n; i++) {
        IrreducibilityResult* r = &results[i];
        if (r->ir_ratio <= 0.0f) continue;

        ReasoningNode* na = (r->node_a >= 0 && r->node_a < node_count)
                            ? net->nodes[r->node_a] : NULL;
        ReasoningNode* nb = (r->node_b >= 0 && r->node_b < node_count)
                            ? net->nodes[r->node_b] : NULL;
        ReasoningNode* nc = (r->node_c >= 0 && r->node_c < node_count)
                            ? net->nodes[r->node_c] : NULL;

        const char* sig = (r->ir_ratio > 3.0f) ? "YES **" : "no";
        char path_str[48];
        snprintf(path_str, sizeof(path_str), "%s→%s→%s",
                 node_concept(na, 8), node_concept(nb, 8), node_concept(nc, 8));

        printf("%-6d %-28s %-8d %-12.2f %s\n",
               i + 1, path_str, r->count, (double)r->ir_ratio, sig);
        shown++;
    }

    /* ---- 8. 显著性摘要 ---- */
    int sig_count = 0;
    int strong_count = 0;  /* ratio > 10 */
    for (int i = 0; i < result_count; i++) {
        if (results[i].ir_ratio > 3.0f) sig_count++;
        if (results[i].ir_ratio > 10.0f) strong_count++;
    }

    printf("\n========================================\n");
    printf("  Summary\n");
    printf("========================================\n");
    printf("Total triplets analyzed : %d\n", result_count);
    printf("Significant (ratio>3)   : %d (%.1f%%)\n",
           sig_count, result_count ? (100.0f * sig_count / result_count) : 0.0f);
    printf("Strongly signif (>10)   : %d\n", strong_count);
    printf("Mean ratio              : %.2f\n",
           result_count ? (results[0].ir_ratio / result_count) : 0.0f);

    /* 也输出尾部（低比率）供对比 */
    if (result_count > 10) {
        printf("\n--- Bottom 5 (likely random co-occurrence) ---\n");
        int start = result_count - 5;
        if (start < 0) start = 0;
        for (int i = start; i < result_count; i++) {
            IrreducibilityResult* r = &results[i];
            if (r->ir_ratio <= 0.0f) continue;

            ReasoningNode* na = (r->node_a >= 0 && r->node_a < node_count)
                                ? net->nodes[r->node_a] : NULL;
            ReasoningNode* nb = (r->node_b >= 0 && r->node_b < node_count)
                                ? net->nodes[r->node_b] : NULL;
            ReasoningNode* nc = (r->node_c >= 0 && r->node_c < node_count)
                                ? net->nodes[r->node_c] : NULL;

            char path_str[48];
            snprintf(path_str, sizeof(path_str), "%s→%s→%s",
                     node_concept(na, 8), node_concept(nb, 8), node_concept(nc, 8));

            printf("  %s  count=%d  ratio=%.2f\n",
                   path_str, r->count, (double)r->ir_ratio);
        }
    }

    /* 清理 */
    free(ranks);
    free(results);
    path_freq_table_destroy(freq);
    master_topology_destroy(master);

    printf("\nDone.\n");
    return 0;
}
