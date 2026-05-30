/**
 * debug_seed.c — 57-line debug tool for MasterTopology seed testing
 * Creates a MasterTopology, seeds a '你好' node into vocab,
 * calls auto_learn_concepts on sample text, and prints stats.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multi_topology.h"
#include "dialog_system.h"

int main(void) {
    printf("=== debug_seed: MasterTopology Seed Debug Tool ===\n\n");
    /* 1. Create MasterTopology and add vocab/semantic sub-topologies */
    MasterTopology* master = master_topology_create(10);
    if (!master) { fprintf(stderr, "FAIL: master_topology_create\n"); return 1; }
    int v_id = master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 100, 5);
    int s_id = master_add_sub_topology(master, TOPO_SEMANTIC, "语义拓扑", 100, 5);
    if (v_id < 0 || s_id < 0) { fprintf(stderr, "FAIL: add sub-topology\n"); return 1; }
    printf("[OK] MasterTopology created with %d sub-topologies\n", master->sub_topo_count);
    /* 2. Seed a '你好' node directly into the vocab topology */
    SubTopology* vocab = master_get_sub_topology(master, v_id);
    ReasoningNode* node = huarong_net_add_node(vocab->net, "你好", NULL, 0);
    if (!node) { fprintf(stderr, "FAIL: huarong_net_add_node\n"); return 1; }
    printf("[OK] Seeded node '%s' (id=%d) in vocab\n", node->concept, node->node_id);
    /* 3. Test auto_learn_concepts with sample Chinese text */
    const char* test_text = "你好世界，今天天气真好";
    printf("\n--- Testing auto_learn_concepts ---\nInput: \"%s\"\n", test_text);
    auto_learn_concepts(master, test_text, NULL);
    printf("[OK] auto_learn_concepts completed\n");
    /* 4. Print system stats */
    int total_nodes, total_links;
    float avg_act;
    master_get_system_status(master, &total_nodes, &total_links, &avg_act);
    printf("\n=== MasterTopology Stats ===\n");
    printf("  Sub-topologies  : %d\n", master->sub_topo_count);
    printf("  Total nodes     : %d\n", total_nodes);
    printf("  Cross links     : %d\n", total_links);
    printf("  Avg activation  : %.4f\n", avg_act);
    printf("  Inferences      : %ld total, %ld successful\n",
           master->total_inferences, master->successful_inferences);
    printf("  Training data   : %d\n", master->training_data_count);
    /* 5. Dump vocab node names with activation/confidence */
    int vn = (vocab && vocab->net) ? vocab->net->node_count : 0;
    printf("  Vocab nodes     : %d\n", vn);
    printf("  Vocab node list :");
    for (int i = 0; i < vn && i < 20; i++) {
        ReasoningNode* n = vocab->net->nodes[i];
        printf(" %s(act=%.2f,conf=%.2f)",
               n->concept, n->activation, n->confidence);
    }
    printf("\n  Semantic nodes  : %d\n",
           master_get_sub_topology(master, s_id)->net->node_count);
    printf("\n=== debug_seed complete ===\n");
    master_topology_destroy(master);
    return 0;
}
