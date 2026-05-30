#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multi_topology.h"
#include "cross_edge_io.h"
#include "feature_io.h"

int main(int argc, char** argv) {
    const char* state_file = argc > 1 ? argv[1] : "pivotmind_state.dat";
    
    MasterTopology* master = master_topology_create(9);
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 6000, 10);
    master_add_sub_topology(master, TOPO_SEMANTIC, "语义拓扑", 2000, 9);
    master_add_sub_topology(master, TOPO_EMOTION, "情绪拓扑", 500, 8);
    master_add_sub_topology(master, TOPO_SYNTAX, "语法拓扑", 500, 7);
    master_add_sub_topology(master, TOPO_CONTEXT, "上下文拓扑", 500, 6);
    master_add_sub_topology(master, TOPO_DOMAIN, "领域拓扑", 500, 5);
    master_add_sub_topology(master, TOPO_PRAGMA, "语用拓扑", 500, 4);
    master_add_sub_topology(master, TOPO_CULTURE, "文化拓扑", 500, 3);
    master_add_sub_topology(master, TOPO_CONCEPT, "概念拓扑", 6000, 9);
    
    int loaded = master_load_state(master, state_file);
    if (loaded <= 0) { printf("加载失败\n"); return 1; }
    printf("状态加载: %d 节点\n", loaded);
    
    int feat = load_features(master, "features.bin");
    if (feat <= 0) { feat = init_random_features(master); }
    printf("特征: %d 节点已初始化\n", feat);
    
    printf("\n=== 重建跨拓扑连接 ===\n");
    int rebuilt = rebuild_cross_connections(master);
    printf("总计: %d 条跨拓扑连接\n", rebuilt);
    
    master_topology_destroy(master);
    return 0;
}
