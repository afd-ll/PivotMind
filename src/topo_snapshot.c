#include "../include/topo_snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ==================== 快照创建 ====================

TopoSnapshot* topo_snapshot_create(SubTopology* sub) {
    if (!sub || !sub->net) return NULL;

    HuarongTopologyNet* net = sub->net;
    int n = net->node_count;
    if (n <= 0) return NULL;

    TopoSnapshot* snap = (TopoSnapshot*)calloc(1, sizeof(TopoSnapshot));
    if (!snap) return NULL;

    snap->node_count = n;
    snap->topo_type = sub->type;
    snap->feature_dim = (n > 0 && net->nodes[0] && net->nodes[0]->features)
                        ? net->nodes[0]->feature_dim : 0;

    // --- 拷贝概念名 ---
    snap->concepts = (char**)calloc(n, sizeof(char*));
    if (!snap->concepts) { topo_snapshot_destroy(snap); return NULL; }

    // --- 拷贝邻接表 ---
    snap->degrees = (int*)calloc(n, sizeof(int));
    snap->adj_lists = (int**)calloc(n, sizeof(int*));
    snap->edge_weights = (float**)calloc(n, sizeof(float*));
    snap->edge_confidences = (float**)calloc(n, sizeof(float*));
    if (!snap->degrees || !snap->adj_lists || !snap->edge_weights || !snap->edge_confidences) {
        topo_snapshot_destroy(snap);
        return NULL;
    }

    // --- 拷贝特征向量（扁平数组） ---
    int total_feat = n * snap->feature_dim;
    if (total_feat > 0) {
        snap->features_flat = (float*)malloc(total_feat * sizeof(float));
        if (!snap->features_flat) { topo_snapshot_destroy(snap); return NULL; }
        for (int i = 0; i < n; i++) {
            ReasoningNode* node = net->nodes[i];
            if (node && node->features && snap->feature_dim > 0) {
                memcpy(&snap->features_flat[i * snap->feature_dim],
                       node->features,
                       snap->feature_dim * sizeof(float));
            }
        }
    }

    // --- 逐节点拷贝 ---
    for (int i = 0; i < n; i++) {
        ReasoningNode* node = net->nodes[i];
        if (!node) continue;

        // 概念名
        if (node->concept) {
            snap->concepts[i] = strdup(node->concept);
        }

        int deg = node->connection_count;
        if (deg <= 0) continue;

        // 邻接表
        snap->degrees[i] = deg;
        snap->adj_lists[i] = (int*)malloc(deg * sizeof(int));
        snap->edge_weights[i] = (float*)malloc(deg * sizeof(float));
        snap->edge_confidences[i] = (float*)malloc(deg * sizeof(float));

        if (!snap->adj_lists[i] || !snap->edge_weights[i] || !snap->edge_confidences[i]) {
            // 释放当前行，标记为0度继续
            free(snap->adj_lists[i]); snap->adj_lists[i] = NULL;
            free(snap->edge_weights[i]); snap->edge_weights[i] = NULL;
            free(snap->edge_confidences[i]); snap->edge_confidences[i] = NULL;
            snap->degrees[i] = 0;
            continue;
        }

        for (int j = 0; j < deg; j++) {
            ReasoningNode* conn = node->connections[j];
            snap->adj_lists[i][j] = conn ? conn->node_id : -1;
            snap->edge_weights[i][j] = node->connection_weights[j];
            snap->edge_confidences[i][j] = node->connection_confidences[j];
        }
    }

    // --- 新边缓冲区 ---
    snap->new_edge_capacity = 256;
    snap->new_edge_count = 0;
    snap->new_edges = (SnapshotEdge*)malloc(snap->new_edge_capacity * sizeof(SnapshotEdge));
    if (!snap->new_edges) { topo_snapshot_destroy(snap); return NULL; }

    return snap;
}

// ==================== 快照销毁 ====================

void topo_snapshot_destroy(TopoSnapshot* snap) {
    if (!snap) return;

    if (snap->concepts) {
        for (int i = 0; i < snap->node_count; i++)
            free(snap->concepts[i]);
        free(snap->concepts);
    }

    if (snap->adj_lists) {
        for (int i = 0; i < snap->node_count; i++) {
            free(snap->adj_lists[i]);
            free(snap->edge_weights[i]);
            free(snap->edge_confidences[i]);
        }
        free(snap->adj_lists);
        free(snap->edge_weights);
        free(snap->edge_confidences);
    }

    free(snap->degrees);
    free(snap->features_flat);
    free(snap->new_edges);
    free(snap->importance_scores);
    free(snap);
}

// ==================== 添加新边 ====================

int topo_snapshot_add_edge(TopoSnapshot* snap, int from_id, int to_id,
                           float weight, float confidence) {
    if (!snap || from_id < 0 || from_id >= snap->node_count ||
        to_id < 0 || to_id >= snap->node_count)
        return -1;
    if (from_id == to_id) return -1;

    // 检查重复
    for (int i = 0; i < snap->new_edge_count; i++) {
        if (snap->new_edges[i].from_node_id == from_id &&
            snap->new_edges[i].to_node_id == to_id)
            return 0;  // 已存在，不重复提交
    }

    // 扩容
    if (snap->new_edge_count >= snap->new_edge_capacity) {
        int new_cap = snap->new_edge_capacity * 2;
        SnapshotEdge* tmp = (SnapshotEdge*)realloc(
            snap->new_edges, new_cap * sizeof(SnapshotEdge));
        if (!tmp) return -1;
        snap->new_edges = tmp;
        snap->new_edge_capacity = new_cap;
    }

    snap->new_edges[snap->new_edge_count].from_node_id = from_id;
    snap->new_edges[snap->new_edge_count].to_node_id = to_id;
    snap->new_edges[snap->new_edge_count].weight = weight;
    snap->new_edges[snap->new_edge_count].confidence = confidence;
    snap->new_edge_count++;
    return 0;
}

// ==================== 合并回主拓扑 ====================

int topo_snapshot_merge(TopoSnapshot* snap, SubTopology* sub) {
    if (!snap || !sub || !sub->net) return 0;

    HuarongTopologyNet* net = sub->net;
    int merged = 0;

    for (int e = 0; e < snap->new_edge_count; e++) {
        int from = snap->new_edges[e].from_node_id;
        int to = snap->new_edges[e].to_node_id;
        float w = snap->new_edges[e].weight;

        if (from >= net->node_count || to >= net->node_count)
            continue;

        // 先检查是否已存在（huarong_net_add_connection 内部也会检查
        // 但我们在锁外尽量避免无效调用）
        ReasoningNode* fn = net->nodes[from];
        ReasoningNode* tn = net->nodes[to];
        if (!fn || !tn) continue;

        int exists = 0;
        for (int c = 0; c < fn->connection_count; c++) {
            if (fn->connections[c] == tn) { exists = 1; break; }
        }

        if (!exists) {
            int ret = huarong_net_add_connection(net, from, to, w);
            if (ret == 0) merged++;
        }
    }

    return merged;
}
