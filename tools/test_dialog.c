/**
 * @file test_dialog.c  (v2)
 * @brief 对话测试工具 — 加载状态 + Hebbian 语义训练 + 测试对话
 *
 * 用法: ./build/bin/test_dialog [state_file] [input_text] [hebbian_epochs]
 *       默认: pivotmind_state.dat "你好" 0 (不跑 Hebbian)
 *       跑 Hebbian: ./build/bin/test_dialog pivotmind_state.dat "你好" 50
 *
 * Hebbian 训练直接内联在工具里，不依赖独立的 hebbian_pretrain 工具。
 * 训练后的特征向量保存到 features.bin + cross_edges.bin。
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
#define HEBBIAN_LR 0.01f

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

    const char* p = data;
    p = skip_ws(p);
    if (*p == '[') p++;
    while (*p && count < max) {
        p = skip_ws(p);
        if (*p == '[') p++;
        p = skip_ws(p);
        char* q = parse_json_string(&p);
        p = skip_ws(p);
        if (*p == ',') p++;
        p = skip_ws(p);
        char* a = parse_json_string(&p);
        p = skip_ws(p);
        if (*p == ']') p++;
        p = skip_ws(p);
        if (*p == ',') p++;
        if (q && a && strlen(q) > 0 && strlen(a) > 0) {
            questions[count] = q;
            answers[count] = a;
            count++;
        } else {
            free(q); free(a);
        }
    }
    free(data);
    *out_q = questions;
    *out_a = answers;
    return count;
}

// ==================== 单字提取 ====================

static int extract_chars(const char* text, char chars[MAX_CHARS][8]) {
    int count = 0;
    for (const char* p = text; *p && count < MAX_CHARS; ) {
        int len = 1;
        if (((unsigned char)*p & 0xE0) == 0xC0) len = 2;
        else if (((unsigned char)*p & 0xF0) == 0xE0) len = 3;
        else if (((unsigned char)*p & 0xF8) == 0xF0) len = 4;
        if (len == 3) {
            int dup = 0;
            for (int i = 0; i < count; i++)
                if (memcmp(chars[i], p, 3) == 0) { dup = 1; break; }
            if (!dup) {
                memcpy(chars[count], p, 3);
                chars[count][3] = '\0';
                count++;
            }
        }
        p += len;
    }
    return count;
}

// ==================== Hebbian 训练 ====================

static void run_hebbian(MasterTopology* master, const char* qa_path, int epochs) {
    printf("\n[He 2/4] 读取 QA 数据...\n");
    char** questions = NULL;
    char** answers = NULL;
    int qa_count = read_qa_json(qa_path, &questions, &answers, MAX_QA);
    printf("  %d 条 QA 对\n", qa_count);
    if (qa_count == 0) return;

    printf("[He 3/4] Hebbian 语义预训练 (%d epoch)...\n", epochs);
    time_t start = time(NULL);
    int total_pairs = 0;

    for (int ep = 0; ep < epochs; ep++) {
        int updates = 0;
        for (int i = 0; i < qa_count; i++) {
            char input_chars[MAX_CHARS][8];
            char reply_chars[MAX_CHARS][8];
            int ic = extract_chars(questions[i], input_chars);
            int rc = extract_chars(answers[i], reply_chars);
            if (ic == 0 || rc == 0) continue;

            for (int ci = 0; ci < ic; ci++) {
                for (int rj = 0; rj < rc; rj++) {
                    ReasoningNode* n1 = NULL;
                    ReasoningNode* n2 = NULL;
                    SubTopology* vocab_sub = NULL;
                    for (int t = 0; t < master->sub_topo_count && (!n1 || !n2); t++) {
                        SubTopology* st = master->sub_topologies[t];
                        if (!st || !st->net || !st->node_hash) continue;
                        if (!n1) n1 = node_hash_find(st->node_hash, input_chars[ci]);
                        if (!n2) n2 = node_hash_find(st->node_hash, reply_chars[rj]);
                        if (st->type == TOPO_VOCABULARY) vocab_sub = st;
                    }

                    if (n1 && n2 && n1->features && n2->features && n1 != n2 && vocab_sub && vocab_sub->net) {
                        // 正样本：只把回复字往输入字拉（不对称更新防止坍缩）
                        float lr = HEBBIAN_LR;
                        for (int d = 0; d < NODE_FEATURE_DIM; d++) {
                            float diff = n1->features[d] - n2->features[d];
                            n2->features[d] += lr * diff;
                        }

                        // 负采样：随机选3个节点，把回复字推离它们
                        for (int neg_try = 0; neg_try < 3; neg_try++) {
                            int neg_idx = rand() % vocab_sub->net->node_count;
                            ReasoningNode* neg = vocab_sub->net->nodes[neg_idx];
                            if (neg && neg->features && neg != n1 && neg != n2) {
                                float neg_lr = -HEBBIAN_LR * 0.2f;
                                for (int d = 0; d < NODE_FEATURE_DIM; d++) {
                                    float diff = n2->features[d] - neg->features[d];
                                    n2->features[d] -= neg_lr * diff;
                                    neg->features[d] += neg_lr * diff;
                                }
                                break;
                            }
                        }
                        updates++;
                    }
                }
            }
        }
        total_pairs += updates;
    }

    double elapsed = difftime(time(NULL), start);
    printf("  ✓ 完成: %d 次 Hebbian 更新, 耗时 %.0f 秒\n", total_pairs, elapsed);

    // 输出语义聚类
    printf("\n[He 4/4] 语义聚类统计...\n");
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net || sub->type != TOPO_VOCABULARY) continue;
        const char* test_words[] = {"天", "学", "开", "大", "我", "好", NULL};
        for (int tw = 0; test_words[tw]; tw++) {
            ReasoningNode* src = node_hash_find(sub->node_hash, test_words[tw]);
            if (!src || !src->features) continue;
            typedef struct { float sim; const char* name; } SimItem;
            SimItem top5[5] = {{0, NULL}};
            for (int nid = 0; nid < sub->net->node_count; nid++) {
                ReasoningNode* cand = sub->net->nodes[nid];
                if (!cand || !cand->features || cand == src || !cand->concept) continue;
                float sim = cosine_similarity(src->features, cand->features, NODE_FEATURE_DIM);
                for (int r = 0; r < 5; r++) {
                    if (sim > top5[r].sim) {
                        for (int s = 4; s > r; s--) top5[s] = top5[s-1];
                        top5[r].sim = sim;
                        top5[r].name = cand->concept;
                        break;
                    }
                }
            }
            printf("  「%s」最近邻:", test_words[tw]);
            for (int r = 0; r < 5 && top5[r].name; r++)
                printf(" %s(%.3f)", top5[r].name, top5[r].sim);
            printf("\n");
        }
    }

    for (int i = 0; i < qa_count; i++) free(questions[i]);
    for (int i = 0; i < qa_count; i++) free(answers[i]);
    free(questions); free(answers);
}

// ==================== 主函数 ====================

int main(int argc, char* argv[]) {
    const char* state_file = argc > 1 ? argv[1] : "pivotmind_state.dat";
    const char* input_text = argc > 2 ? argv[2] : "你好";
    int hebbian_epochs = argc > 3 ? atoi(argv[3]) : 0;
    int max_output = 50;

    // 初始化随机数
    init_random();

    printf("╔══════════════════════════════════════════╗\n");
    printf("║      玄枢 对话测试工具 v2              ║\n");
    printf("╚══════════════════════════════════════════╝\n\n");

    // 1. 创建主拓扑
    printf("[1/5] 创建多拓扑认知网络...\n");
    MasterTopology* master = master_topology_create(9);
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

    // [可选] Hebbian 语义预训练
    if (hebbian_epochs > 0) {
        const char* qa_path = "data/hermes_knowledge_base.json";
        run_hebbian(master, qa_path, hebbian_epochs);
    }

    // 保存特征向量 + 跨连接
    save_features(master, "features.bin");

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
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (node) total_edges += node->connection_count;
        }
    }
    printf("统计: 9857 节点, %d 内部边, %d 跨连接\n",
           total_edges / 2, master->cross_link_count);

    master_topology_destroy(master);
    printf("\n✓ 测试完成\n");
    return 0;
}
