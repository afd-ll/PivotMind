/**
 * @file test_dialog.c  (v3)
 * @brief 对话测试工具 — 加载状态 + QA共现建边 + 测试对话
 *
 * 用法: ./build/bin/test_dialog [state_file] [input_text] [epochs]
 *       默认: pivotmind_state.dat "你好" 1 (跑1轮共现建边)
 *       跑共现建边: ./build/bin/test_dialog pivotmind_state.dat "你好" 50
 *
 * 基于 QA 数据的字符共现关系直接建边。
 * 与 Hebbian 特征训练不同——共现建边直接创建拓扑连接（边），不修改特征向量。
 * QA 对中问题字→回答字 建立有向边，多次共现累积权重。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <time.h>

#include "multi_topology.h"
#include "common.h"
#include "feature_io.h"
#include "cross_edge_io.h"

#define MAX_CHARS 256
#define MAX_QA 100000

// ==================== JSON 解析 ====================

static const char* skip_ws(const char* p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static char* parse_json_string(const char** p) {
    *p = skip_ws(*p);
    if (**p != '"') return NULL;
    (*p)++;
    const char* start = *p;
    while (**p && **p != '"') {
        if (**p == '\\' && *(*p+1)) (*p)++;
        (*p)++;
    }
    if (**p != '"') return NULL;
    int len = (int)(*p - start);
    char* result = (char*)malloc(len + 1);
    if (!result) return NULL;
    int out = 0;
    for (int i = 0; i < len; i++) {
        if (start[i] == '\\' && i+1 < len) {
            if (start[i+1] == '"') { result[out++] = '"'; i++; }
            else if (start[i+1] == 'n') { result[out++] = '\n'; i++; }
            else if (start[i+1] == '\\') { result[out++] = '\\'; i++; }
            else result[out++] = start[i];
        } else {
            result[out++] = start[i];
        }
    }
    result[out] = '\0';
    (*p)++;
    return result;
}

static int read_qa_json(const char* path, char*** out_q, char*** out_a, int max) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* data = (char*)malloc(size + 1);
    if (!data) { fclose(f); return 0; }
    fread(data, 1, size, f);
    data[size] = '\0';
    fclose(f);

    char** questions = (char**)calloc(max, sizeof(char*));
    char** answers = (char**)calloc(max, sizeof(char*));
    int count = 0;

    // 格式: [[q1, a1], [q2, a2], ...]  或  {"data": [[q1, a1], ...]}
    // 跳过外层包裹，找到第一个 [
    const char* p = data;
    p = skip_ws(p);
    if (*p == '{') {
        // 跳过 {"data": 或类似外层对象
        while (*p && *p != '[') p++;
    }
    p = skip_ws(p);
    if (*p == '[') {
        p++; // skip outer [
        p = skip_ws(p);

        while (*p && count < max) {
            if (*p != '[') break;  // 不是内层数组
            p++; // skip inner [
            p = skip_ws(p);

            char* q = parse_json_string(&p);
            p = skip_ws(p);
            if (*p == ',') p++;
            p = skip_ws(p);
            char* a = parse_json_string(&p);
            p = skip_ws(p);
            if (*p == ']') p++; // skip inner ]

            if (q && a) {
                questions[count] = q;
                answers[count] = a;
                count++;
            } else {
                if (q) free(q);
                if (a) free(a);
            }

            p = skip_ws(p);
            if (*p == ',') p++;
            p = skip_ws(p);
        }
    }

    free(data);
    *out_q = questions;
    *out_a = answers;
    return count;
}

// ==================== 汉字提取 ====================

static int extract_chars(const char* text, char out[MAX_CHARS][8]) {
    int count = 0;
    const char* p = text;
    while (*p && count < MAX_CHARS) {
        int len = 1;
        unsigned char c = (unsigned char)*p;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        if (len > 1) {
            strncpy(out[count], p, len);
            out[count][len] = '\0';
            count++;
        }
        p += len;
    }
    return count;
}

// ==================== QA 共现建边 ====================

static void build_edges_from_qa(MasterTopology* master, const char* qa_path, int epochs) {
    printf("\n[QE 2/4] 读取 QA 数据...\n");
    char** questions = NULL;
    char** answers = NULL;
    int qa_count = read_qa_json(qa_path, &questions, &answers, MAX_QA);
    printf("  %d 条 QA 对\n", qa_count);
    if (qa_count == 0) return;

    printf("[QE 3/4] 基于共现建边 (%d epoch)...\n", epochs);
    time_t start = time(NULL);
    int total_edges = 0;

    for (int ep = 0; ep < epochs; ep++) {
        int upd = 0;
        for (int i = 0; i < qa_count; i++) {
            char input_chars[MAX_CHARS][8];
            char reply_chars[MAX_CHARS][8];
            int ic = extract_chars(questions[i], input_chars);
            int rc = extract_chars(answers[i], reply_chars);
            if (ic == 0 || rc == 0) continue;

            // 预查找所有节点（避免在热循环内重复 hash_lookup）
            ReasoningNode* in_nodes[MAX_CHARS];
            ReasoningNode* out_nodes[MAX_CHARS];
            SubTopology* vocab_sub = NULL;

            for (int t = 0; t < master->sub_topo_count; t++) {
                SubTopology* st = master->sub_topologies[t];
                if (st && st->type == TOPO_VOCABULARY) { vocab_sub = st; break; }
            }
            if (!vocab_sub || !vocab_sub->net) continue;

            for (int ci = 0; ci < ic; ci++)
                in_nodes[ci] = node_hash_find(vocab_sub->node_hash, input_chars[ci]);
            for (int rj = 0; rj < rc; rj++)
                out_nodes[rj] = node_hash_find(vocab_sub->node_hash, reply_chars[rj]);

            for (int ci = 0; ci < ic; ci++) {
                if (!in_nodes[ci]) continue;
                for (int rj = 0; rj < rc; rj++) {
                    if (!out_nodes[rj] || in_nodes[ci] == out_nodes[rj]) continue;
                    int from_id = in_nodes[ci]->node_id;
                    int to_id = out_nodes[rj]->node_id;
                    // 先查是否已有边，有则累加权重，无则新建
                    int found = 0;
                    for (int e = 0; e < in_nodes[ci]->connection_count; e++) {
                        if (in_nodes[ci]->connections[e] == out_nodes[rj]) {
                            in_nodes[ci]->connection_weights[e] += 0.3f;
                            if (in_nodes[ci]->connection_weights[e] > 5.0f)
                                in_nodes[ci]->connection_weights[e] = 5.0f;
                            found = 1;
                            break;
                        }
                    }
                    if (!found) {
                        huarong_net_add_connection(vocab_sub->net, from_id, to_id, 0.8f);
                    }
                    // 反向边同理
                    found = 0;
                    for (int e = 0; e < out_nodes[rj]->connection_count; e++) {
                        if (out_nodes[rj]->connections[e] == in_nodes[ci]) {
                            out_nodes[rj]->connection_weights[e] += 0.2f;
                            if (out_nodes[rj]->connection_weights[e] > 5.0f)
                                out_nodes[rj]->connection_weights[e] = 5.0f;
                            found = 1;
                            break;
                        }
                    }
                    if (!found) {
                        huarong_net_add_connection(vocab_sub->net, to_id, from_id, 0.6f);
                    }
                    upd++;
                }
            }
        }
        total_edges += upd;
    }

    double elapsed = difftime(time(NULL), start);
    printf("  ✓ 完成: %d 条边 (共现法), 耗时 %.0f 秒\n", total_edges, elapsed);

    for (int i = 0; i < qa_count; i++) free(questions[i]);
    for (int i = 0; i < qa_count; i++) free(answers[i]);
    free(questions); free(answers);
}

// ==================== 主函数 ====================

int main(int argc, char* argv[]) {
    const char* state_file = argc > 1 ? argv[1] : "pivotmind_state.dat";
    const char* input_text = argc > 2 ? argv[2] : "你好";
    int epochs = argc > 3 ? atoi(argv[3]) : 1;
    int max_output = 50;

    // 初始化随机数
    init_random();

    printf("╔══════════════════════════════════════════╗\n");
    printf("║      玄枢 对话测试工具 v3              ║\n");
    printf("╚══════════════════════════════════════════╝\n\n");

    // 1. 创建主拓扑
    printf("[1/5] 创建多拓扑认知网络...\n");
    MasterTopology* master = master_topology_create(12);
    if (!master) { printf("错误: 无法创建主拓扑\n"); return 1; }

    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 6000, 10);
    master_add_sub_topology(master, TOPO_SEMANTIC, "语义拓扑", 2000, 9);
    master_add_sub_topology(master, TOPO_EMOTION, "情绪拓扑", 500, 8);
    master_add_sub_topology(master, TOPO_SYNTAX, "语法拓扑", 500, 7);
    master_add_sub_topology(master, TOPO_CONTEXT, "上下文拓扑", 500, 6);
    master_add_sub_topology(master, TOPO_DOMAIN, "领域拓扑", 500, 5);
    master_add_sub_topology(master, TOPO_PRAGMA, "语用拓扑", 500, 4);
    master_add_sub_topology(master, TOPO_CULTURE, "文化拓扑", 500, 3);
    master_add_sub_topology(master, TOPO_CONCEPT, "概念拓扑", 6000, 9);
    printf("     ✓ 认知网络就绪 (9 个拓扑)\n");

    // 2. 加载拓扑状态
    printf("[2/5] 加载拓扑状态...\n");
    if (access(state_file, F_OK) != 0) {
        printf("错误: 状态文件 %s 不存在\n", state_file);
        master_topology_destroy(master); return 1;
    }
    int loaded = master_load_state(master, state_file);
    if (loaded < 0) {
        printf("错误: 无法加载状态 (返回=%d)\n", loaded);
        master_topology_destroy(master); return 1;
    }
    printf("     ✓ 已加载拓扑状态 (%d 节点)\n", loaded);

    // 3. 加载/初始化特征向量
    printf("[3/5] 加载特征向量...\n");
    int feat_loaded = load_features(master, "features.bin");
    int features_were_reinit = 0;
    if (feat_loaded > 0) {
        printf("     ✓ 已加载特征向量 (%d 节点)\n", feat_loaded);
    } else {
        int initted = init_random_features(master);
        printf("     ✓ 已初始化特征向量 (%d 节点)\n", initted);
        features_were_reinit = 1;
    }

    // 4. 加载/重建跨拓扑连接
    printf("[4/5] 加载跨拓扑连接...\n");
    int cross_loaded = load_cross_edges(master, "cross_edges.bin");
    if (cross_loaded > 0 && !features_were_reinit) {
        printf("     ✓ 已加载跨拓扑连接 (%d 条)\n", cross_loaded);
    } else {
        int rebuilt = rebuild_cross_connections(master);
        printf("     ✓ 已重建跨拓扑连接 (%d 条)\n", rebuilt);
        save_cross_edges(master, "cross_edges.bin");
    }

    // 4.5 跳过特征相似度建边（不产生语义边），直接由 QA 共现或旧状态提供边

    // [可选] QA 共现建边
    if (epochs > 0) {
        const char* qa_path = "data/hermes_knowledge_base.json";
        build_edges_from_qa(master, qa_path, epochs);
    }

    // 保存特征向量 + 跨连接
    save_features(master, "features.bin");

    // 保存状态文件（v3 格式，含边数据）
    master_save_state(master, "pivotmind_state.dat");
    printf("     ✓ 已保存 v3 状态文件\n");

    // 5. 生成回复
    printf("\n[5/5] 生成回复...\n\n");
    printf("输入: \"%s\"\n\n", input_text);

    char* response = master_generate_response(master, input_text, max_output);
    if (response) {
        printf(">>> 输出: \"%s\"\n", response);
        printf("     (长度: %zu 字符)\n\n", strlen(response));
        free(response);
    } else {
        printf(">>> 输出: (空)\n\n");
    }

    // 统计
    int total_edges = 0;
    int total_nodes = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        total_nodes += sub->net->node_count;
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (node) total_edges += node->connection_count;
        }
    }
    printf("统计: %d 节点, %d 内部边, %d 跨连接\n",
           total_nodes, total_edges / 2, master->cross_link_count);

    master_topology_destroy(master);
    printf("\n✓ 测试完成\n");
    return 0;
}
