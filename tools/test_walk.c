/**
 * 走边路径生成测试
 * 测试 topology_walk_greedy 的路径连贯性、终点判断、跨起点重试
 */
#include "multi_topology.h"
#include "node_hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// 辅助：向子拓扑添加节点，自动添加节点哈希
static ReasoningNode* add_node(SubTopology* sub, const char* concept, float activation, float valence) {
    HuarongTopologyNet* net = sub->net;
    ReasoningNode* node = huarong_net_add_node(net, concept, NULL, 0);
    if (!node) return NULL;
    node->activation = activation;
    node->confidence = 0.5f;
    node->valence = valence;
    node_hash_add(sub->node_hash, node);
    return node;
}

// 辅助：在两个节点之间添加双向边
static void add_edge(SubTopology* sub, ReasoningNode* from, ReasoningNode* to,
                     float weight, float conf, float bias) {
    if (!from || !to) return;
    HuarongTopologyNet* net = sub->net;

    // 确保 connection 数组有空间（简化版：直接替换已有连接）
    int idx = from->connection_count;
    int cap = from->connection_capacity;

    if (idx >= cap) {
        int new_cap = cap == 0 ? 8 : cap * 2;
        from->connections = realloc(from->connections, sizeof(ReasoningNode*) * new_cap);
        from->connection_weights = realloc(from->connection_weights, sizeof(float) * new_cap);
        from->connection_confidences = realloc(from->connection_confidences, sizeof(float) * new_cap);
        from->connection_motivational_bias = realloc(from->connection_motivational_bias, sizeof(float) * new_cap);
        from->connection_capacity = new_cap;
    }

    from->connections[idx] = to;
    from->connection_weights[idx] = weight;
    from->connection_confidences[idx] = conf;
    from->connection_motivational_bias[idx] = bias;
    from->connection_count++;
}

static void print_path(SubTopology* sub, int* path, int len) {
    printf("  路径(%d): ", len);
    for (int i = 0; i < len; i++) {
        int nid = path[i];
        if (nid >= 0 && nid < sub->net->node_count && sub->net->nodes[nid]) {
            printf("%s", sub->net->nodes[nid]->concept);
            if (i < len - 1) printf("→");
        } else {
            printf("[?]");
        }
    }
    printf("\n");
}

// ===== 测试1: 路径连贯性 =====
// 构建 人→工→智→能→是→什→么 的链，看是否能完整走出
static void test_coherence() {
    printf("\n========== 测试1: 路径连贯性 ==========\n");

    MasterTopology* master = master_topology_create(10);
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 100, 10);
    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);

    // 构建: 人→工→智→能→是→什→么 (高权重链)
    ReasoningNode* ren   = add_node(vocab, "人", 0.95f, 0.5f);
    ReasoningNode* gong  = add_node(vocab, "工", 0.90f, 0.4f);
    ReasoningNode* zhi   = add_node(vocab, "智", 0.85f, 0.6f);
    ReasoningNode* neng  = add_node(vocab, "能", 0.80f, 0.5f);
    ReasoningNode* shi   = add_node(vocab, "是", 0.75f, 0.3f);
    ReasoningNode* shen  = add_node(vocab, "什", 0.70f, 0.2f);
    ReasoningNode* me    = add_node(vocab, "么", 0.65f, 0.1f);

    // 强连接链：主路径
    add_edge(vocab, ren, gong, 0.9f, 0.95f, 0.5f);
    add_edge(vocab, gong, zhi, 0.85f, 0.90f, 0.5f);
    add_edge(vocab, zhi, neng, 0.80f, 0.88f, 0.5f);
    add_edge(vocab, neng, shi, 0.75f, 0.85f, 0.4f);
    add_edge(vocab, shi, shen, 0.70f, 0.80f, 0.3f);
    add_edge(vocab, shen, me, 0.65f, 0.75f, 0.3f);

    // 添加干扰边：人→A(不相关但激活高)
    ReasoningNode* otherA = add_node(vocab, "A", 0.92f, -0.1f);
    ReasoningNode* otherB = add_node(vocab, "B", 0.88f, -0.2f);
    add_edge(vocab, ren, otherA, 0.3f, 0.4f, 0.1f);  // 弱边但目标激活高
    add_edge(vocab, otherA, otherB, 0.2f, 0.3f, 0.1f);

    // 反向边（低权重）
    add_edge(vocab, gong, ren, 0.1f, 0.2f, 0.0f);

    printf("从「人」出发走边:\n");
    int path[20];
    float scores[20];
    int len = topology_walk_greedy(vocab, ren->node_id, path, scores, 10, NULL);
    print_path(vocab, path, len);
    printf("  预期: 人→工→智→能→是→什→么\n");
    printf("  不包含: A、B（干扰边得分低，应该被过滤掉）\n");

    // 从中间节点出发
    printf("\n从「智」出发走边:\n");
    len = topology_walk_greedy(vocab, zhi->node_id, path, scores, 10, NULL);
    print_path(vocab, path, len);
    printf("  预期: 智→能→是→什→么\n");

    master_topology_destroy(master);
}

// ===== 测试2: 终点判断 =====
// 构建不同长度的链，看阈值0.05是否合理
static void test_termination() {
    printf("\n========== 测试2: 终点判断 ==========\n");

    MasterTopology* master = master_topology_create(10);
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 100, 10);
    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);

    // 构建 学→习→新→知→识 (依次递减的边权重，模拟末端自然结束)
    ReasoningNode* xue  = add_node(vocab, "学", 0.90f, 0.6f);
    ReasoningNode* xi   = add_node(vocab, "习", 0.85f, 0.5f);
    ReasoningNode* xin  = add_node(vocab, "新", 0.60f, 0.4f);
    ReasoningNode* zhi  = add_node(vocab, "知", 0.40f, 0.3f);
    ReasoningNode* shi2 = add_node(vocab, "识", 0.30f, 0.2f);

    add_edge(vocab, xue, xi, 0.90f, 0.92f, 0.5f);   // 强
    add_edge(vocab, xi, xin, 0.70f, 0.75f, 0.4f);    // 中
    add_edge(vocab, xin, zhi, 0.30f, 0.35f, 0.2f);   // 弱
    add_edge(vocab, zhi, shi2, 0.08f, 0.10f, 0.1f);  // 极弱，接近阈值

    printf("从「学」出发走边（渐进弱化链）:\n");
    int path[20];
    float scores[20];
    int len = topology_walk_greedy(vocab, xue->node_id, path, scores, 10, NULL);
    print_path(vocab, path, len);
    printf("  每步得分: ");
    for (int i = 0; i < len; i++) printf("%.4f ", scores[i]);
    printf("\n");
    printf("  预期: 学→习→新→知（「识」的连接权重+激活值可能低于0.05阈值被截断）\n");

    // 无路可走的叶子节点
    ReasoningNode* leaf = add_node(vocab, "叶", 0.50f, 0.0f);
    printf("\n从「叶」出发（无连接）：\n");
    len = topology_walk_greedy(vocab, leaf->node_id, path, scores, 10, NULL);
    print_path(vocab, path, len);
    printf("  预期: 叶（只有起点，无路径）\n");

    master_topology_destroy(master);
}

// ===== 测试3: 跨起点重试 =====
// 第一起点走不长，换第二起点
static void test_multi_start() {
    printf("\n========== 测试3: 跨起点重试 ==========\n");

    MasterTopology* master = master_topology_create(10);
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 100, 10);
    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);

    // 孤立起点A（只有1步连接）
    ReasoningNode* a1 = add_node(vocab, "A", 0.95f, 0.3f);
    ReasoningNode* a2 = add_node(vocab, "A2", 0.30f, 0.2f);
    add_edge(vocab, a1, a2, 0.5f, 0.5f, 0.3f);
    // A->A2后就没路了（A2无出边）

    // 长链起点B（可以走很远）
    ReasoningNode* b1 = add_node(vocab, "B", 0.90f, 0.5f);
    ReasoningNode* b2 = add_node(vocab, "B2", 0.85f, 0.4f);
    ReasoningNode* b3 = add_node(vocab, "B3", 0.80f, 0.4f);
    ReasoningNode* b4 = add_node(vocab, "B4", 0.75f, 0.3f);
    add_edge(vocab, b1, b2, 0.9f, 0.9f, 0.5f);
    add_edge(vocab, b2, b3, 0.8f, 0.8f, 0.5f);
    add_edge(vocab, b3, b4, 0.7f, 0.7f, 0.4f);

    printf("模拟: 排序后的起点列表 A(act=0.95) > B(act=0.90)\n");
    printf("从A出发: 只能走1步(A→A2)\n");
    printf("跨起点重试: 换B出发 → 可走B→B2→B3→B4\n\n");

    // 先走A
    int path[20];
    float scores[20];
    int len = topology_walk_greedy(vocab, a1->node_id, path, scores, 10, NULL);
    printf("起点A:\n");
    print_path(vocab, path, len);

    // 走B（用同一个visited bitmap也行，但不共享）
    len = topology_walk_greedy(vocab, b1->node_id, path, scores, 10, NULL);
    printf("起点B:\n");
    print_path(vocab, path, len);

    master_topology_destroy(master);
}

// ===== 测试4: 三角环防循环 =====
// 人↔工↔智↔人（循环），不能死循环
static void test_loop_prevention() {
    printf("\n========== 测试4: 防循环 ==========\n");

    MasterTopology* master = master_topology_create(10);
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 100, 10);
    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);

    ReasoningNode* a = add_node(vocab, "A", 0.90f, 0.5f);
    ReasoningNode* b = add_node(vocab, "B", 0.85f, 0.4f);
    ReasoningNode* c = add_node(vocab, "C", 0.80f, 0.3f);
    ReasoningNode* d = add_node(vocab, "D", 0.75f, 0.3f);

    // 三角环: A→B→C→A
    add_edge(vocab, a, b, 0.9f, 0.9f, 0.5f);
    add_edge(vocab, b, c, 0.8f, 0.8f, 0.5f);
    add_edge(vocab, c, a, 0.7f, 0.7f, 0.5f);
    // 分支: C→D（唯一出路）
    add_edge(vocab, c, d, 0.6f, 0.6f, 0.4f);

    printf("三角环 A→B→C→A + 分支 C→D\n");
    printf("不能死循环在A→B→C→A，必须走到D\n\n");

    int path[20];
    float scores[20];
    int len = topology_walk_greedy(vocab, a->node_id, path, scores, 10, NULL);
    print_path(vocab, path, len);
    printf("  预期: A→B→C→D（不会回到A）\n");

    master_topology_destroy(master);
}

int main() {
    printf("========================================\n");
    printf("  走边路径生成测试\n");
    printf("  六维评分: 边(权重+置信+动机) + 节点(激活+置信+效价)\n");
    printf("  阈值: < 0.05 停止\n");
    printf("========================================\n");

    test_coherence();
    test_termination();
    test_multi_start();
    test_loop_prevention();

    printf("\n========================================\n");
    printf("  测试完成\n");
    printf("========================================\n");
    return 0;
}
