/**
 * @file corpus_train.c
 * @brief 语料训练工具 — 处理整个书库+QA，一次性全量建边
 *
 * 用法: ./build/bin/corpus_train [state_file] [epochs]
 *       默认: pivotmind_state.dat 1
 *
 * 处理流程:
 *   1. 加载已有状态
 *   2. 遍历 ~/本地书库/*.txt — 相邻字符共现建边（语序学习）
 *   3. 遍历 data/hermes_knowledge_base.json — QA 共现建边（对话学习）
 *   4. 保存 v3 状态文件
 *
 * 边权重策略:
 *   - 前向边 (char_N → char_N+1): 累加 +0.3/次, 上限 5.0
 *   - 反向边 (char_N+1 → char_N): 累加 +0.1/次, 上限 3.0
 *     这样走边时会偏向顺着语序走
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

#include "multi_topology.h"
#include "common.h"
#include "feature_io.h"
#include "cross_edge_io.h"

#define MAX_CHARS 4096
#define MAX_QA 100000
#define MAX_LINE 65536
#define CORPUS_DIR "~/本地书库"
#define QA_PATH "data/hermes_knowledge_base.json"
#define BOOK_WEIGHT_FWD 0.3f
#define BOOK_WEIGHT_REV 0.1f
#define QA_WEIGHT_FWD 0.8f
#define QA_WEIGHT_REV 0.6f
#define MAX_EDGE_WEIGHT_FWD 5.0f
#define MAX_EDGE_WEIGHT_REV 3.0f

// ==================== 汉字提取 ====================
// 从文本中提取所有中文字符到数组中
static int extract_chinese_chars(const char* text, char out[MAX_CHARS][8]) {
    int count = 0;
    const char* p = text;
    while (*p && count < MAX_CHARS) {
        int len = 1;
        unsigned char c = (unsigned char)*p;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        if (len == 3) {  // 严格中文字符（3字节 UTF-8）
            strncpy(out[count], p, len);
            out[count][len] = '\0';
            count++;
        }
        p += len;
    }
    return count;
}

// ==================== 添加或累加边 ====================
static void add_or_strengthen(HuarongTopologyNet* net, ReasoningNode* from, ReasoningNode* to,
                               float add_weight, float max_weight) {
    if (!from || !to || from == to) return;
    // 查已有边 → 累加
    for (int e = 0; e < from->connection_count; e++) {
        if (from->connections[e] == to) {
            from->connection_weights[e] += add_weight;
            if (from->connection_weights[e] > max_weight)
                from->connection_weights[e] = max_weight;
            return;
        }
    }
    // 新建边
    huarong_net_add_connection(net, from->node_id, to->node_id, add_weight);
}

// ==================== 处理单本书 ====================
static int process_book_file(MasterTopology* master, SubTopology* vocab_sub,
                             const char* filepath, const char* book_name) {
    FILE* f = fopen(filepath, "rb");
    if (!f) { printf("  ⚠ 无法打开: %s\n", filepath); return 0; }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* data = (char*)malloc(size + 1);
    if (!data) { fclose(f); return 0; }
    size_t read = fread(data, 1, size, f);
    data[read] = '\0';
    fclose(f);

    // 逐行处理（避免跨行建边，保持段落独立性）
    int edges = 0;
    int char_count = 0;
    char* line = data;
    while (*line) {
        char* next = strchr(line, '\n');
        if (next) *next = '\0';

        // 跳过空行和太短的行
        size_t line_len = strlen(line);
        if (line_len >= 4 && line_len < MAX_LINE) {
            char chars[MAX_CHARS][8];
            int cc = extract_chinese_chars(line, chars);
            char_count += cc;

            // 相邻字符建边
            for (int i = 1; i < cc; i++) {
                ReasoningNode* n1 = node_hash_find(vocab_sub->node_hash, chars[i-1]);
                ReasoningNode* n2 = node_hash_find(vocab_sub->node_hash, chars[i]);
                // 节点不存在则新建
                if (!n1) {
                    n1 = huarong_net_add_node(vocab_sub->net, chars[i-1], NULL, 0);
                    if (n1) node_hash_add(vocab_sub->node_hash, n1);
                }
                if (!n2) {
                    n2 = huarong_net_add_node(vocab_sub->net, chars[i], NULL, 0);
                    if (n2) node_hash_add(vocab_sub->node_hash, n2);
                }
                if (!n1 || !n2) continue;

                // 前向: char_N → char_N+1（语序）
                add_or_strengthen(vocab_sub->net, n1, n2,
                                  BOOK_WEIGHT_FWD, MAX_EDGE_WEIGHT_FWD);
                // 反向: char_N+1 → char_N（弱化）
                add_or_strengthen(vocab_sub->net, n2, n1,
                                  BOOK_WEIGHT_REV, MAX_EDGE_WEIGHT_REV);
                edges++;
            }
        }
        line = next ? (next + 1) : (line + strlen(line));
    }

    free(data);
    printf("  ✓ %s: %d 中文字, %d 条边\n", book_name, char_count, edges);
    return edges;
}

// ==================== 处理书库目录 ====================
static int process_corpus_dir(MasterTopology* master, SubTopology* vocab_sub,
                               const char* dir_path) {
    // 展开 ~/
    char real_path[1024];
    if (dir_path[0] == '~') {
        const char* home = getenv("HOME");
        if (!home) home = "/home/cx";
        snprintf(real_path, sizeof(real_path), "%s%s", home, dir_path + 1);
    } else {
        snprintf(real_path, sizeof(real_path), "%s", dir_path);
    }

    DIR* dir = opendir(real_path);
    if (!dir) {
        printf("  ⚠ 无法打开目录: %s\n", real_path);
        return 0;
    }

    int total_edges = 0;
    int file_count = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        // 只处理 .txt 文件
        size_t len = strlen(entry->d_name);
        if (len < 4 || strcmp(entry->d_name + len - 4, ".txt") != 0) continue;

        char filepath[1024];
        snprintf(filepath, sizeof(filepath), "%s/%s", real_path, entry->d_name);

        struct stat st;
        if (stat(filepath, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        // 跳过太小的文件（<1KB 可能是空文件或临时文件）
        if (st.st_size < 1024) {
            printf("  ⚠ 跳过过小: %s (%ld bytes)\n", entry->d_name, (long)st.st_size);
            continue;
        }

        printf("[书库] %s (%ld KB)...\n", entry->d_name, (long)(st.st_size / 1024));
        int book_edges = process_book_file(master, vocab_sub, filepath, entry->d_name);
        total_edges += book_edges;
        file_count++;
    }
    closedir(dir);
    printf("  共处理 %d 个文件, %d 条边\n", file_count, total_edges);
    return total_edges;
}

// ==================== JSON 解析 ====================
// 复用 test_dialog 中的 JSON 解析
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

    const char* p = data;
    p = skip_ws(p);
    if (*p == '{') {
        while (*p && *p != '[') p++;
    }
    p = skip_ws(p);
    if (*p == '[') {
        p++;
        p = skip_ws(p);
        while (*p && count < max) {
            if (*p != '[') break;
            p++;
            p = skip_ws(p);
            char* q = parse_json_string(&p);
            p = skip_ws(p);
            if (*p == ',') p++;
            p = skip_ws(p);
            char* a = parse_json_string(&p);
            p = skip_ws(p);
            if (*p == ']') p++;
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

#define MAX_MSG_CHARS 256

static int extract_chars(const char* text, char out[MAX_MSG_CHARS][8]) {
    int count = 0;
    const char* p = text;
    while (*p && count < MAX_MSG_CHARS) {
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

// ==================== 处理 QA 数据 ====================
static int process_qa_data(MasterTopology* master, SubTopology* vocab_sub,
                           const char* qa_path, int epochs) {
    printf("\n[QA] 读取 QA 数据...\n");
    char** questions = NULL;
    char** answers = NULL;
    int qa_count = read_qa_json(qa_path, &questions, &answers, MAX_QA);
    printf("  %d 条 QA 对\n", qa_count);
    if (qa_count == 0) return 0;

    int total_edges = 0;
    printf("[QA] 共现建边 (%d epoch)...\n", epochs);

    for (int ep = 0; ep < epochs; ep++) {
        int upd = 0;
        for (int i = 0; i < qa_count; i++) {
            char q_chars[MAX_MSG_CHARS][8];
            char a_chars[MAX_MSG_CHARS][8];
            int qc = extract_chars(questions[i], q_chars);
            int ac = extract_chars(answers[i], a_chars);
            if (qc == 0 || ac == 0) continue;

            ReasoningNode* q_nodes[MAX_MSG_CHARS];
            ReasoningNode* a_nodes[MAX_MSG_CHARS];
            for (int ci = 0; ci < qc; ci++)
                q_nodes[ci] = node_hash_find(vocab_sub->node_hash, q_chars[ci]);
            for (int rj = 0; rj < ac; rj++)
                a_nodes[rj] = node_hash_find(vocab_sub->node_hash, a_chars[rj]);

            for (int ci = 0; ci < qc; ci++) {
                if (!q_nodes[ci]) continue;
                for (int rj = 0; rj < ac; rj++) {
                    if (!a_nodes[rj] || q_nodes[ci] == a_nodes[rj]) continue;
                    // 问题→回答: 强连接
                    add_or_strengthen(vocab_sub->net, q_nodes[ci], a_nodes[rj],
                                      QA_WEIGHT_FWD, MAX_EDGE_WEIGHT_FWD);
                    // 回答→问题: 弱连接（让路径能走回去）
                    add_or_strengthen(vocab_sub->net, a_nodes[rj], q_nodes[ci],
                                      QA_WEIGHT_REV, MAX_EDGE_WEIGHT_REV);
                    upd++;
                }
            }
        }
        total_edges += upd;
        if ((ep + 1) % 10 == 0)
            printf("  epoch %d/%d: +%d 边\n", ep + 1, epochs, upd);
    }

    printf("  ✓ QA 建边完成: %d 条边\n", total_edges);

    for (int i = 0; i < qa_count; i++) free(questions[i]);
    for (int i = 0; i < qa_count; i++) free(answers[i]);
    free(questions); free(answers);
    return total_edges;
}

// ==================== 主函数 ====================

int main(int argc, char* argv[]) {
    const char* state_file = argc > 1 ? argv[1] : "pivotmind_state.dat";
    int epochs = argc > 2 ? atoi(argv[2]) : 1;

    init_random();
    srand((unsigned)time(NULL));

    printf("╔══════════════════════════════════════════╗\n");
    printf("║      玄枢 语料训练工具 v1              ║\n");
    printf("╚══════════════════════════════════════════╝\n\n");

    // 1. 创建主拓扑
    printf("[1/5] 创建多拓扑认知网络...\n");
    MasterTopology* master = master_topology_create(12);
    if (!master) { printf("错误: 无法创建主拓扑\n"); return 1; }

    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 12000, 10);
    master_add_sub_topology(master, TOPO_SEMANTIC, "语义拓扑", 4000, 9);
    master_add_sub_topology(master, TOPO_EMOTION, "情绪拓扑", 1000, 8);
    master_add_sub_topology(master, TOPO_SYNTAX, "语法拓扑", 1000, 7);
    master_add_sub_topology(master, TOPO_CONTEXT, "上下文拓扑", 1000, 6);
    master_add_sub_topology(master, TOPO_DOMAIN, "领域拓扑", 1000, 5);
    master_add_sub_topology(master, TOPO_PRAGMA, "语用拓扑", 1000, 4);
    master_add_sub_topology(master, TOPO_CULTURE, "文化拓扑", 1000, 3);
    master_add_sub_topology(master, TOPO_CONCEPT, "概念拓扑", 6000, 9);
    printf("     ✓ 认知网络就绪 (9 个拓扑)\n");

    // 2. 加载已有状态
    printf("[2/5] 加载拓扑状态...\n");
    if (access(state_file, F_OK) == 0) {
        int loaded = master_load_state(master, state_file);
        if (loaded > 0) printf("     ✓ 已加载 %d 节点\n", loaded);
        else printf("     ⚠ 状态加载失败，使用空拓扑\n");
    } else {
        printf("     ⚠ 状态文件 %s 不存在，使用空拓扑\n", state_file);
    }

    // 3. 获取词汇拓扑
    SubTopology* vocab_sub = NULL;
    for (int t = 0; t < master->sub_topo_count; t++) {
        if (master->sub_topologies[t] && master->sub_topologies[t]->type == TOPO_VOCABULARY) {
            vocab_sub = master->sub_topologies[t];
            break;
        }
    }
    if (!vocab_sub) { printf("错误: 找不到词汇拓扑\n"); master_topology_destroy(master); return 1; }

    // 4. 处理书库（相邻字符共现 → 语序学习）
    printf("\n[3/5] 处理书库语料...\n");
    time_t t_start = time(NULL);
    int book_edges = process_corpus_dir(master, vocab_sub, CORPUS_DIR);
    double book_elapsed = difftime(time(NULL), t_start);
    printf("  书库完成: %d 条边, 耗时 %.0f 秒\n\n", book_edges, book_elapsed);

    // 5. 处理 Hermes QA 数据（对话关联学习）
    printf("[4/5] 处理 Hermes 对话数据...\n");
    int qa_edges = 0;
    if (access(QA_PATH, F_OK) == 0) {
        qa_edges = process_qa_data(master, vocab_sub, QA_PATH, epochs);
    } else {
        printf("  ⚠ QA 文件不存在: %s\n", QA_PATH);
    }

    // 6. 初始化特征向量 + 跨拓扑连接
    printf("\n[5/5] 保存状态...\n");
    if (access("features.bin", F_OK) != 0) {
        int initted = init_random_features(master);
        printf("     ✓ 初始化特征向量 (%d 节点)\n", initted);
    } else {
        int loaded = load_features(master, "features.bin");
        printf("     ✓ 加载特征向量 (%d 节点)\n", loaded > 0 ? loaded : 0);
    }

    if (access("cross_edges.bin", F_OK) != 0) {
        int rebuilt = rebuild_cross_connections(master);
        printf("     ✓ 重建跨拓扑连接 (%d 条)\n", rebuilt);
        save_cross_edges(master, "cross_edges.bin");
    }

    save_features(master, "features.bin");
    master_save_state(master, state_file);

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

    printf("\n╔══════════════════════════════════════════╗\n");
    printf("║          训练完成                         ║\n");
    printf("╠══════════════════════════════════════════╣\n");
    printf("║  总节点:  %-6d                       ║\n", total_nodes);
    printf("║  总边数:  %-6d                       ║\n", total_edges / 2);
    printf("║  书库边:  %-6d                       ║\n", book_edges);
    printf("║  QA 边:   %-6d                       ║\n", qa_edges);
    printf("║  耗时:    %-6.0f 秒                    ║\n", book_elapsed);
    printf("╚══════════════════════════════════════════╝\n");

    double total_elapsed = difftime(time(NULL), t_start);
    printf("\n总耗时: %.0f 秒\n", total_elapsed);

    master_topology_destroy(master);
    return 0;
}
