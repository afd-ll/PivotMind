#include "self_learner.h"
#include "multi_topology.h"
#include "huarong_topology.h"
#include "node_hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main() {
    printf("=== SelfLearner 联网自学习测试 ===\n");
    
    /* 创建最小拓扑 */
    MasterTopology* m = master_topology_create(5);
    master_add_sub_topology(m, TOPO_VOCABULARY, "词汇", 1000, 10);
    master_add_sub_topology(m, TOPO_SEMANTIC, "语义", 100, 5);
    master_add_sub_topology(m, TOPO_EMOTION, "情绪", 50, 5);
    master_add_sub_topology(m, TOPO_CONTEXT, "上下文", 100, 5);
    master_add_sub_topology(m, TOPO_CONCEPT, "概念", 100, 5);
    
    SubTopology* vocab = master_get_sub_topology_by_type(m, TOPO_VOCABULARY);
    if (!vocab || !vocab->net) { printf("FAIL: no vocab\n"); return 1; }
    
    /* 创建一些已知节点（有连接） */
    ReasoningNode* n1 = huarong_net_add_node(vocab->net, "温度", NULL, 0);
    ReasoningNode* n2 = huarong_net_add_node(vocab->net, "热", NULL, 0);
    if (n1 && n2) {
        node_hash_add(vocab->node_hash, n1);
        node_hash_add(vocab->node_hash, n2);
        huarong_net_add_connection(vocab->net, n1->node_id, n2->node_id, 0.7f);
    }
    
    /* 创建孤立节点（0连接） */
    ReasoningNode* isolated = huarong_net_add_node(vocab->net, "量子", NULL, 0);
    if (!isolated) { printf("FAIL: isolated node\n"); return 1; }
    node_hash_add(vocab->node_hash, isolated);
    isolated->confidence = 0.1f;  /* 低置信度 */
    
    int before = vocab->net->node_count;
    int before_conn = isolated->connection_count;
    printf("创建前: %d节点, 孤立节点连接数=%d\n", before, before_conn);
    
    /* 创建SelfLearner并运行 */
    SelfLearnerConfig cfg = SELF_LEARNER_DEFAULT_CONFIG;
    cfg.seeds_per_cycle = 10;  /* 多采样，增加命中孤立节点的概率 */
    cfg.verbose = 1;
    
    SelfLearner* sl = self_learner_create(m, &cfg);
    if (!sl) { printf("FAIL: self_learner_create\n"); return 1; }
    
    printf("\n>>> 启动自主学习循环...\n");
    int mods = self_learner_cycle(sl);
    
    int after = vocab->net->node_count;
    int after_conn = isolated->connection_count;
    printf("\n结果: %d处修改, %d节点(前) → %d节点(后)\n", mods, before, after);
    printf("孤立节点 '%s' 连接数: %d(前) → %d(后)\n", 
           isolated->concept, before_conn, after_conn);
    
    if (after > before) {
        printf("✓ 联网学习成功！新增节点:\n");
        for (int i = before; i < after && i < after; i++) {
            if (vocab->net->nodes[i])
                printf("  [%d] %s\n", i, vocab->net->nodes[i]->concept);
        }
    }
    
    /* 显示所有连接 */
    printf("\n'%s' 的连接:\n", isolated->concept);
    for (int c = 0; c < isolated->connection_count && c < 10; c++) {
        ReasoningNode* cn = isolated->connections[c];
        float w = isolated->connection_weights ? isolated->connection_weights[c] : 0;
        printf("  → %s (w=%.2f)\n", cn ? cn->concept : "?", w);
    }
    
    self_learner_destroy(sl);
    master_topology_destroy(m);
    printf("\n=== 测试完成 ===\n");
    return 0;
}
