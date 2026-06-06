/**
 * state_dump.c — 快速加载状态并打印训练数据统计
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "multi_topology.h"
#include "cognitive_controller.h"
#include "constants.h"

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "pivotmind_state.dat";
    printf("=== 玄枢状态分析: %s ===\n\n", path);

    MasterTopology* master = master_topology_create(0);
    if (!master) { fprintf(stderr, "FAIL: 创建 MasterTopology\n"); return 1; }
    int loaded = master_load_state(master, path);
    if (loaded < 0) {
        fprintf(stderr, "FAIL: 加载状态文件失败 (返回 %d)\n", loaded);
        master_topology_destroy(master);
        return 1;
    }

    int total_nodes = 0, total_links = 0;
    float avg_act = 0.0f;
    master_get_system_status(master, &total_nodes, &total_links, &avg_act);
    printf("全局统计:\n");
    printf("  子拓扑数:     %d\n", master->sub_topo_count);
    printf("  总节点数:     %d\n", total_nodes);
    printf("  总连接数:     %d\n", total_links);
    printf("  平均激活值:   %.4f\n", avg_act);
    printf("  推理次数:     %ld / %ld 成功\n", master->total_inferences, master->successful_inferences);

    // 跨拓扑连接统计
    int cross_link_count = 0;
    float cross_avg_weight = 0.0f;
    if (master->cross_links) {
        for (int i = 0; i < master->cross_link_count; i++) {
            if (master->cross_links[i] && master->cross_links[i]->weight > 0.001f) {
                cross_link_count++;
                cross_avg_weight += master->cross_links[i]->weight;
            }
        }
    }
    if (cross_link_count > 0) cross_avg_weight /= cross_link_count;
    printf("  跨拓扑连接:   %d (有效 %d, 平均权重 %.4f)\n",
           master->cross_link_count, cross_link_count, cross_avg_weight);

    printf("\n--- 各拓扑详细 ---\n");
    printf("%-12s %6s %8s %8s %8s %8s %8s\n",
           "拓扑", "节点", "边", "平均度", "平均置信", "高置信%", "平均激活");

    for (int t = 0; t < master->sub_topo_count && t < MAX_SUBTOPOS; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;

        HuarongTopologyNet* net = sub->net;
        int nc = net->node_count;
        if (nc <= 0) continue;

        int total_edges = 0;
        float sum_conf = 0.0f, sum_act = 0.0f;
        int high_conf = 0, conf_count = 0;

        for (int n = 0; n < nc; n++) {
            ReasoningNode* node = net->nodes[n];
            if (!node) continue;
            sum_act += node->activation;
            if (node->connection_count > 0) {
                total_edges += node->connection_count;
                if (node->connection_confidences) {
                    for (int c = 0; c < node->connection_count; c++) {
                        float cf = node->connection_confidences[c];
                        sum_conf += cf;
                        if (cf > 0.7f) high_conf++;
                        conf_count++;
                    }
                }
            }
        }

        float avg_deg = (nc > 0) ? (float)total_edges / nc : 0;
        float avg_conf = (conf_count > 0) ? sum_conf / conf_count : 0;
        float high_conf_pct = (conf_count > 0) ? (float)high_conf * 100 / conf_count : 0;
        float avg_a = (nc > 0) ? sum_act / nc : 0;

        // 查找最活跃的节点
        float max_act = 0.0f;
        const char* max_concept = "N/A";
        for (int n = 0; n < nc; n++) {
            if (net->nodes[n] && net->nodes[n]->activation > max_act) {
                max_act = net->nodes[n]->activation;
                max_concept = net->nodes[n]->concept;
            }
        }

        printf("%-12s %6d %8d %8.1f %8.4f %7.1f%% %8.4f  (最活跃:%s=%.3f)\n",
               sub->name, nc, total_edges, avg_deg, avg_conf, high_conf_pct, avg_a,
               max_concept, max_act);
    }

    // 置信度分布
    printf("\n--- 置信度分布 (所有拓扑汇总) ---\n");
    int bins[10] = {0};  // 0-10%, 10-20%, ..., 90-100%
    int total_conf_samples = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (!node || !node->connection_confidences) continue;
            for (int c = 0; c < node->connection_count; c++) {
                int b = (int)(node->connection_confidences[c] * 10);
                if (b < 0) b = 0; if (b > 9) b = 9;
                bins[b]++;
                total_conf_samples++;
            }
        }
    }
    if (total_conf_samples > 0) {
        printf("  总样本: %d\n", total_conf_samples);
        for (int i = 0; i < 10; i++) {
            int bar = bins[i] * 40 / total_conf_samples;
            printf("  %2d-%2d%%: %7d (%5.1f%%) ",
                   i*10, (i+1)*10, bins[i], bins[i]*100.0f/total_conf_samples);
            for (int j = 0; j < bar; j++) printf("█");
            printf("\n");
        }
    }

    // 特征向量统计
    printf("\n--- 特征向量统计 ---\n");
    int nodes_with_feat = 0;
    float feat_dim_used = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (node && node->features && node->feature_dim > 0) {
                nodes_with_feat++;
                // 计算非零维度比例
                int non_zero = 0;
                for (int d = 0; d < node->feature_dim && d < 32; d++)
                    if (fabsf(node->features[d]) > 0.0001f) non_zero++;
                feat_dim_used += (float)non_zero / (node->feature_dim < 32 ? node->feature_dim : 32);
            }
        }
    }
    printf("  有特征节点:   %d / %d (%.1f%%)\n",
           nodes_with_feat, total_nodes,
           total_nodes > 0 ? nodes_with_feat * 100.0f / total_nodes : 0);
    printf("  非零维度占比: %.1f%%\n",
           nodes_with_feat > 0 ? feat_dim_used * 100 / nodes_with_feat : 0);

    master_topology_destroy(master);
    printf("\n=== 分析完成 ===\n");
    return 0;
}
