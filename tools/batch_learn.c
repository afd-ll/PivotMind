/**
 * @file batch_learn.c
 * @brief 批量喂入工具 — 将 QA 数据集灌入自主学习器
 *
 * 读取 hermes_knowledge_base.json（格式：[["问","答"],["问","答"],...]）
 * 对每对 (问, 答) 调用 autonomic_learn_from_dialog()
 * 通过自主学习器的同时激活机制自动建边涨置信度
 *
 * 编译: gcc -std=gnu99 -O2 -Iinclude -I. -Ilibs -D_USE_MATH_DEFINES -pthread
 *        -o build/bin/batch_learn tools/batch_learn.c src/*.c -lm -fopenmp
 *        (排除 network_tool.c)
 * 用法: ./build/bin/batch_learn [状态文件] [QA文件] [epochs]
 *       默认: pivotmind_state.dat  data/hermes_knowledge_base.json  1
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "multi_topology.h"
#include "autonomic_learner.h"
#include "node_hash.h"

// ==================== JSON 简易解析 ====================

/**
 * 跳过空白字符
 */
static const char* skip_ws(const char* p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

/**
 * 解析 JSON 字符串（"..."，处理转义）
 * 返回解析后的字符串（需要 free），p 更新到结束引号之后
 */
static char* parse_json_string(const char** p) {
    *p = skip_ws(*p);
    if (**p != '"') return NULL;
    (*p)++; // 跳过开引号

    // 先计算长度
    int len = 0;
    const char* tmp = *p;
    while (*tmp && *tmp != '"') {
        if (*tmp == '\\') { tmp++; if (*tmp) tmp++; }
        else tmp++;
        len++;
    }
    if (*tmp != '"') return NULL;

    // 分配并复制
    char* result = (char*)malloc(len + 1);
    if (!result) return NULL;

    int pos = 0;
    while (**p && **p != '"') {
        if (**p == '\\') {
            (*p)++;
            if (**p == '"') result[pos++] = '"';
            else if (**p == '\\') result[pos++] = '\\';
            else if (**p == 'n') result[pos++] = '\n';
            else { result[pos++] = '\\'; result[pos++] = **p; }
            (*p)++;
        } else {
            result[pos++] = **p;
            (*p)++;
        }
    }
    result[pos] = '\0';

    if (**p == '"') (*p)++; // 跳过闭引号
    return result;
}

/**
 * 从 JSON 文件中读取 QA 对
 * 格式: [["问1","答1"],["问2","答2"],...]
 * @return QA 对数
 */
static int read_qa_json(const char* path,
                        char*** out_questions,
                        char*** out_answers,
                        int max_pairs) {
    // 读取整个文件
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        printf("[错误] 无法打开: %s\n", path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char* data = (char*)malloc(fsize + 1);
    if (!data) { fclose(fp); return -1; }
    long got = fread(data, 1, fsize, fp);
    data[got] = '\0';
    fclose(fp);

    // 预分配
    char** questions = (char**)calloc(max_pairs, sizeof(char*));
    char** answers = (char**)calloc(max_pairs, sizeof(char*));
    if (!questions || !answers) {
        free(data); free(questions); free(answers);
        return -1;
    }

    // 解析
    const char* p = skip_ws(data);
    int count = 0;

    if (*p == '[') p++; // 跳过外层 [

    while (*p && count < max_pairs) {
        p = skip_ws(p);
        if (*p == ']') break; // 结束
        if (*p == ',') { p++; continue; } // 逗号分隔

        // 期待内层 [
        if (*p == '[') {
            p++; // 跳过 [
            p = skip_ws(p);
            
            // 解析问题
            char* q = parse_json_string(&p);
            if (!q) { p++; continue; }

            p = skip_ws(p);
            if (*p == ',') p++;
            p = skip_ws(p);

            // 解析回答
            char* a = parse_json_string(&p);
            if (!a) { free(q); p++; continue; }

            // 跳过空白和逗号、]
            p = skip_ws(p);
            while (*p && *p != ']') p++;
            if (*p == ']') p++;

            // 保存
            if (strlen(q) > 0 && strlen(a) > 0) {
                questions[count] = q;
                answers[count] = a;
                count++;
                if (count % 100 == 0) {
                    printf("  已解析 %d 条 QA\n", count);
                }
            } else {
                free(q);
                free(a);
            }
        } else {
            p++;
        }
    }

    *out_questions = questions;
    *out_answers = answers;

    printf("  共解析 %d 条 QA\n", count);

    // 如果数据不消耗全部，释放多余的
    free(data);
    return count;
}

// ==================== 主函数 ====================

int main(int argc, char* argv[]) {
    const char* state_path = argc > 1 ? argv[1] : "pivotmind_state.dat";
    const char* qa_path = argc > 2 ? argv[2] : "data/hermes_knowledge_base.json";
    int epochs = argc > 3 ? atoi(argv[3]) : 1;
    if (epochs < 1) epochs = 1;
    if (epochs > 100) epochs = 100;

    setbuf(stdout, NULL);

    printf("╔═══════════════════════════════════════════╗\n");
    printf("║    玄枢 批量学习工具 v1.0                 ║\n");
    printf("╚═══════════════════════════════════════════╝\n\n");

    // 1. 加载或创建拓扑
    printf("[1/4] 加载拓扑...\n");
    MasterTopology* master = master_topology_create(10);

    // 创建所需的子拓扑
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 8000, 10);
    master_add_sub_topology(master, TOPO_CONCEPT, "概念拓扑", 8000, 9);

    // 尝试加载已有状态
    int loaded = 0;
    FILE* test = fopen(state_path, "rb");
    if (test) {
        fclose(test);
        int n = master_load_state(master, state_path);
        if (n > 0) {
            printf("  ✓ 已加载 %d 个节点\n", n);
            loaded = 1;
        }
    }

    if (!loaded) {
        printf("  - 未找到已有状态，从空拓扑开始\n");
    }

    // 统计现有节点和边
    int total_nodes = 0, total_edges = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        total_nodes += sub->net->node_count;
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (node) total_edges += node->connection_count;
        }
    }
    printf("  当前: %d 节点, %d 条内部边\n", total_nodes, total_edges);

    // 2. 读取 QA 数据
    printf("\n[2/4] 读取 QA 数据...\n");
    char** questions = NULL;
    char** answers = NULL;
    int qa_count = read_qa_json(qa_path, &questions, &answers, 100000);
    if (qa_count <= 0) {
        printf("  × QA 数据为空\n");
        master_topology_destroy(master);
        free(questions);
        free(answers);
        return 1;
    }
    printf("  ✓ %d 条 QA 对\n", qa_count);

    // 3. 批量学习
    printf("\n[3/4] 批量学习 (%d epoch)...\n", epochs);

    // 初始化自主学习状态
    AutonomicState state;
    autonomic_state_init(&state);

    // 批量喂入时关掉中间刷盘（最后一次性保存），防止每50次更新就写一次2MB+文件
    state.flush_threshold = 100000000;
    state.idle_flush_seconds = 86400;  // 空闲也等一天

    time_t start_time = time(NULL);
    int total_pairs = 0;

    for (int ep = 0; ep < epochs; ep++) {
        int epoch_pairs = 0;
        if (epochs > 1) printf("\n  ── Epoch %d/%d ──\n", ep + 1, epochs);

        for (int i = 0; i < qa_count; i++) {
            autonomic_learn_from_dialog(master,
                                        questions[i],
                                        answers[i],
                                        &state);
            epoch_pairs++;

            // 每 200 条打印进度
            if ((i + 1) % 200 == 0 || i == qa_count - 1) {
                double elapsed = difftime(time(NULL), start_time);
                int edges_here = 0;
                float avg_conf_here = 0;
                autonomic_get_edge_stats(master, &edges_here, &avg_conf_here);
                printf("  [%d/%d] %.0fs  边=%d  平均置信度=%.3f  更新=%d\n",
                       i + 1, qa_count, elapsed,
                       edges_here, avg_conf_here, state.pending_updates);
            }
        }

        total_pairs += epoch_pairs;
    }

    double total_time = difftime(time(NULL), start_time);

    // 4. 保存
    printf("\n[4/4] 保存拓扑状态...\n");

    // 一次性保存（关掉了中间刷盘，在这里显式保存以确保完整性）
    {
        char path[512];
        const char* state_path = argc > 1 ? argv[1] : "pivotmind_state.dat";
        snprintf(path, 511, "%s", state_path);

        FILE* existing = fopen(path, "rb");
        if (existing) {
            fclose(existing);
            char bak[520];
            snprintf(bak, 519, "%s.bak", path);
            remove(bak);
            rename(path, bak);
        }

        int saved = master_save_state(master, path);
        if (saved > 0) {
            printf("  ✓ 已保存 %d 节点到 %s\n", saved, path);
        } else {
            printf("  × 保存失败\n");
        }
    }

    // 统计
    int final_edges = 0;
    float final_avg_conf = 0;
    autonomic_get_edge_stats(master, &final_edges, &final_avg_conf);

    // 词汇拓扑统计
    int vocab_edges = 0;
    float vocab_conf = 0;
    SubTopology* vocab = NULL;
    for (int t = 0; t < master->sub_topo_count; t++) {
        if (master->sub_topologies[t] &&
            master->sub_topologies[t]->type == TOPO_VOCABULARY) {
            vocab = master->sub_topologies[t];
            break;
        }
    }
    if (vocab && vocab->net) {
        int e = 0;
        float c = 0;
        for (int n = 0; n < vocab->net->node_count; n++) {
            ReasoningNode* node = vocab->net->nodes[n];
            if (!node) continue;
            for (int ec = 0; ec < node->connection_count; ec++) {
                e++;
                c += node->connection_confidences[ec];
            }
        }
        vocab_edges = e;
        vocab_conf = e > 0 ? c / e : 0;
    }

    printf("\n╔═══════════════════════════════════════════╗\n");
    printf("║  完成！                                   ║\n");
    printf("╠═══════════════════════════════════════════╣\n");
    printf("║  处理: %d 条 QA x %d epoch = %d 次         ║\n", qa_count, epochs, total_pairs);
    printf("║  耗时: %.0f 秒                            ║\n", total_time);
    printf("║  词汇拓扑节点: %d                          ║\n", vocab ? vocab->net->node_count : 0);
    printf("║  词汇拓扑边: %d                            ║\n", vocab_edges);
    printf("║  平均置信度: %.3f                          ║\n", vocab_conf);
    printf("║  总边(含跨拓扑): %d                        ║\n", final_edges);
    printf("╚═══════════════════════════════════════════╝\n");

    // 清理
    autonomic_state_destroy(&state);
    for (int i = 0; i < qa_count; i++) {
        free(questions[i]);
        free(answers[i]);
    }
    free(questions);
    free(answers);
    master_topology_destroy(master);

    return 0;
}
