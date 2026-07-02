/**
 * @file batch_learn.c
 * @brief 批量喂入工具 — 将 QA 数据集灌入自主学习器
 *
 * 读取 hermes_knowledge_base.json（格式：[["问","答"],["问","答"],...]）
 * 对每对 (问, 答) 调用 autonomic_learn_from_dialog()
 * 通过自主学习器的同时激活机制自动建边涨置信度
 *
 * 编译: gcc -std=gnu99 -O2 -Iinclude -I. -Ilibs -D_USE_MATH_DEFINES -pthread
 *        -o build/bin/batch_learn tools/batch_learn.c src/ *.c -lm -fopenmp
 *        (排除 network_tool.c)
 * 用法: ./build/bin/batch_learn [状态文件] [QA文件] [epochs]
 *       默认: pivotmind_state.dat  data/hermes_knowledge_base.json  1
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <omp.h>

#include "multi_topology.h"
#include "autonomic_learner.h"
#include "feature_io.h"
#include "cross_edge_io.h"
#include "template_builder.h"
#include "path_encoding.h"
#include "node_hash.h"
#include "dict_loader.h"
#include "cognitive_controller.h"
#include "pivotmind_version.h"

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

// ==================== 拓扑健康指标统计 ====================

/**
 * 收集拓扑健康指标（仅在调试输出中使用，不修改任何全局状态）
 *
 * 输出参数说明：
 *   topo_names       — 各拓扑名称（内部指针，不分配）
 *   topo_edges       — 各拓扑边数
 *   topo_count       — 实际写入的子拓扑数
 *   conf_low         — 置信度 < 0.3 的边占比
 *   conf_med         — 置信度 0.3~0.7 的边占比
 *   conf_high        — 置信度 > 0.7 的边占比
 *   sat_ratio        — 权重达到上限的边占比
 *   zero_degree_ratio— 无连接的孤立节点占比
 *   avg_degree       — 平均节点度数（总边数/总节点数，仅含内部边）
 */
static void collect_topo_health(MasterTopology* master,
                                const char** topo_names, int* topo_edges,
                                int* topo_count,
                                float* conf_low, float* conf_med, float* conf_high,
                                float* sat_ratio, float* zero_degree_ratio,
                                float* avg_degree) {
    int t_count = 0;
    int total_edges_all = 0;
    int total_nodes_all = 0;
    int low_c = 0, med_c = 0, high_c = 0;
    int sat_edges = 0;
    int zero_deg_nodes = 0;

    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;

        // 保护读取连接数组不被并发写线程（boost_connection_weighted 等）损坏
        pthread_mutex_lock(&sub->net->mutex);

        const char* name = sub->name ? sub->name : "未知";
        int te = 0;

        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (!node) continue;
            total_nodes_all++;

            if (node->edge_count == 0) {
                zero_deg_nodes++;
            }

            for (int e = 0; e < node->edge_count; e++) {
                te++;
                total_edges_all++;

                float c = node->edges[e].confidence;
                if (c < 0.3f) low_c++;
                else if (c < 0.7f) med_c++;
                else high_c++;

                // 检查权重是否接近上限 (5.0)
                if (node->edges[e].weight >= 4.95f) {
                    sat_edges++;
                }
            }
        }

        if (topo_names && t_count < 20) topo_names[t_count] = name;
        if (topo_edges) topo_edges[t_count] = te;
        t_count++;
        pthread_mutex_unlock(&sub->net->mutex);
    }

    if (topo_count) *topo_count = t_count;

    int total_c = low_c + med_c + high_c;
    if (conf_low) *conf_low = total_c > 0 ? (float)low_c / total_c * 100.0f : 0;
    if (conf_med) *conf_med = total_c > 0 ? (float)med_c / total_c * 100.0f : 0;
    if (conf_high) *conf_high = total_c > 0 ? (float)high_c / total_c * 100.0f : 0;

    if (sat_ratio) *sat_ratio = total_c > 0 ? (float)sat_edges / total_c * 100.0f : 0;
    if (zero_degree_ratio) *zero_degree_ratio = total_nodes_all > 0 ? (float)zero_deg_nodes / total_nodes_all * 100.0f : 0;
    if (avg_degree) *avg_degree = total_nodes_all > 0 ? (float)total_edges_all / total_nodes_all : 0;
}

// 跨拓扑重建间隔:
//   - 正常编译: 每5000条QA重建一次（内存充裕的Pi 3B）
//   - 编译时定义 LOW_MEM: 重建延后到训练结束时一次性做（Zero 2W等受限设备）
#ifndef LOW_MEM
#define CROSS_REBUILD_INTERVAL 5000
#else
#define CROSS_REBUILD_INTERVAL 99999999  // 低内存模式：不重建
#endif

// ==================== 主函数 ====================

int main(int argc, char* argv[]) {
    const char* state_path = argc > 1 ? argv[1] : "pivotmind_state.dat";
    const char* qa_path = argc > 2 ? argv[2] : "data/hermes_knowledge_base.json";
    int epochs = argc > 3 ? atoi(argv[3]) : 1;
    if (epochs < 1) epochs = 1;
    // 原先有硬编码上限 100，已移除以支持大规模训练
    // if (epochs > 100) epochs = 100;

    setbuf(stdout, NULL);

    printf("╔═══════════════════════════════════════════╗\n");
    printf("║    玄枢 批量学习工具 v%-14s║\n", PIVOTMIND_VERSION);
    printf("╚═══════════════════════════════════════════╝\n\n");

    // 1. 加载或创建拓扑
    printf("[1/4] 加载拓扑...\n");
    MasterTopology* master = master_topology_create(11);

    // 创建所需的子拓扑（与 build_cross_links 保持一致）
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 30000, 10);
    master_add_sub_topology(master, TOPO_SEMANTIC, "语义拓扑", 12000, 9);
    master_add_sub_topology(master, TOPO_EMOTION, "情绪拓扑", 4000, 8);
    master_add_sub_topology(master, TOPO_SYNTAX, "语法拓扑", 1000, 7);
    master_add_sub_topology(master, TOPO_CONTEXT, "上下文拓扑", 1000, 6);
    master_add_sub_topology(master, TOPO_DOMAIN, "领域拓扑", 1000, 5);
    master_add_sub_topology(master, TOPO_PRAGMA, "语用拓扑", 1000, 4);
    master_add_sub_topology(master, TOPO_CULTURE, "文化拓扑", 1000, 3);
    master_add_sub_topology(master, TOPO_CONCEPT, "概念拓扑", 12000, 9);
    master_add_sub_topology(master, TOPO_MASTER, "主拓扑", 100, 0);   /* 占位：保证 TOPO_TEMPLATE 获得正确的 topo_id=10 */
    master_add_sub_topology(master, TOPO_TEMPLATE, "模板拓扑", 4000, 8);

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

    // 初始化线程池供认知调度使用
    master_get_thread_pool(master);
    printf("  ✓ 线程池已就绪\n");

    // 加载外部词典（词→词性标注，如果存在）
    {
        const char* dict_path = "data/jieba_dict.txt";
        FILE* df = fopen(dict_path, "r");
        if (df) {
            fclose(df);
            DictTable* dt = dict_table_create(524288);
            if (dt && dict_load_jieba(dt, dict_path) > 0) {
                master->ext_dict = (struct ExternalDict*)dt;
                printf("  ✓ 词典已加载 (%d 条), 启用词级建模\n", dict_table_size(dt));
            }
        } else {
            printf("  - 词典未找到 (%s), 回退逐字模式\n", dict_path);
        }
    }

    // 尝试加载或重建跨拓扑连接（调试临时跳过重建以加速）
    {
        int cross_loaded = load_cross_edges(master, "cross_edges.bin");
        if (cross_loaded > 0) {
            printf("  ✓ 加载跨拓扑连接 %d 条\n", cross_loaded);
        } else {
            printf("  - 未找到跨连接文件，自动重建...\n");
            int rebuilt = rebuild_cross_connections(master);
            if (rebuilt > 0) {
                printf("  ✓ 已重建跨拓扑连接 (%d 条)\n", rebuilt);
                save_cross_edges(master, "cross_edges.bin");
            } else {
                printf("  - 警告: 跨拓扑重建失败，训练将在无跨连接状态下运行\n");
            }
        }
    }

    // 统计现有节点和边
    int total_nodes = 0, total_edges = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        total_nodes += sub->net->node_count;
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (node) total_edges += node->edge_count;
        }
    }
    printf("  当前: %d 节点, %d 条内部边\n", total_nodes, total_edges);

    // 尝试从频率表自动构建模板（如有足够数据）
    {
        int built = template_auto_build(master, 500, 100);
        if (built > 0) printf("  ✓ 自动构建 %d 个模板节点\n", built);
    }

    fflush(stdout);

    // 2. 读取 QA 数据
    printf("\n[2/4] 读取 QA 数据...\n");
    char** questions = NULL;
    char** answers = NULL;
    int qa_count = read_qa_json(qa_path, &questions, &answers, 300000);
    if (qa_count <= 0) {
        printf("  × QA 数据为空\n");
        master_topology_destroy(master);
        free(questions);
        free(answers);
        return 1;
    }
    printf("  ✓ %d 条 QA 对\n", qa_count);
    fflush(stdout);

    // 3. 批量学习
    printf("\n[3/4] 批量学习 (%d epoch)...\n", epochs);

    // 初始化自主学习状态
    AutonomicState state;
    autonomic_state_init(&state);

    // 异步刷盘：后台线程定期保存检查点，主线程不阻塞
    // 阈值设 500 万次更新保存一次（约 8 次/epoch，对 292k QA / 20 线程）
    state.flush_threshold = 5000000;
    state.idle_flush_seconds = 60;     // 若持续无更新，1 分钟内保存
    state.last_flush_time = time(NULL);
    autonomic_start_async_flush(&state, master);

    time_t start_time = time(NULL);
    int total_pairs = 0;

    for (int ep = 0; ep < epochs; ep++) {
        int epoch_pairs = 0;
        if (epochs > 1) printf("\n  ── Epoch %d/%d ──\n", ep + 1, epochs);
        fflush(stdout);

#define BATCH_CHUNK_SIZE 200    // OpenMP 每批 QA 数（可调）
        // 每条约 100-1000 次拓扑操作，net->mutex 保护底层写入
        // 不同字在不同 QA 中的碰撞概率极低
        volatile int omp_processed = 0;

        #pragma omp parallel for schedule(dynamic, 200)
        for (int i = 0; i < qa_count; i++) {
            autonomic_learn_from_dialog(master,
                                        questions[i],
                                        answers[i],
                                        &state,
                                        NULL, NULL);

            // 每 2000 条写 stderr 追踪（crash 定位用）
            if (i % 2000 == 0) {
                fprintf(stderr, "T%d:QA:%d\n", omp_get_thread_num(), i);
                fflush(stderr);
            }

            // 原子计数，用于进度追踪
            int done;
            #pragma omp atomic capture
            done = ++omp_processed;

            // 每 2000 条打印进度（临界区保护，只打印一次）
            if (done % 2000 == 0 || done == qa_count) {
                #pragma omp critical
                {
                    double elapsed = difftime(time(NULL), start_time);
                    int edges_here = 0;
                    float avg_conf_here = 0;
                    autonomic_get_edge_stats(master, &edges_here, &avg_conf_here);

                    // 收集拓扑健康指标
                    const char* topo_names[20];
                    int topo_edges[20];
                    int topo_count = 0;
                    float conf_low = 0, conf_med = 0, conf_high = 0;
                    float sat_ratio = 0, zero_deg = 0, avg_deg = 0;
                    collect_topo_health(master, topo_names, topo_edges, &topo_count,
                                        &conf_low, &conf_med, &conf_high,
                                        &sat_ratio, &zero_deg, &avg_deg);

                    // 基础行
                    printf("  [%d/%d] %.0fs  边=%d  平均置信度=%.3f  更新=%d\n",
                           done, qa_count, elapsed,
                           edges_here, avg_conf_here, state.pending_updates);
                    // 置信度分布行
                    printf("        置信分布: [低<0.3] %.0f%%  [中] %.0f%%  [高>0.7] %.0f%%"
                           "  饱和=%.0f%%\n", conf_low, conf_med, conf_high, sat_ratio);
                    // 图结构行
                    printf("        图结构: 平均度=%.1f  孤立节点=%.0f%%  跨拓扑=%d\n",
                           avg_deg, zero_deg, master->cross_link_count);
                    fflush(stdout);
                }
            }

        }

        // 跨拓扑重建（epoch结束后单线程安全执行）
        int before = master->cross_link_count;
        rebuild_cross_connections(master);
        int added = master->cross_link_count - before;
        if (added > 0)
            printf("  → 跨拓扑重建: 新增 %d 条连接 (共 %d)\n", added, master->cross_link_count);

        // Epoch 间竞争衰减 — 防止置信度只涨不跌导致边饱和
        if (epochs > 1) {
            printf("  → Epoch %d/%d 竞争衰减...\n", ep + 1, epochs);
            autonomic_decay_all(master);
        }

        // 清理训练期间延迟释放的扩容旧数组
        huarong_net_cleanup_retired_batch(master);

        // 路径模板: 贪心游走填充频率表 + 自动构建模板
        {
            SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
            if (vocab && vocab->net && vocab->net->node_count > 0) {
                /* 贪心游走: 从活跃节点出发收集路径模式 */
                int walked = 0;
                int max_walks = vocab->net->node_count < 3000 ? vocab->net->node_count : 3000;
                int path_buf[16];
                float score_buf[16];
                unsigned char visited[16384] = {0};
                for (int n = 0; n < max_walks && n < vocab->net->node_count; n++) {
                    if (!vocab->net->nodes[n]) continue;
                    visited[vocab->net->nodes[n]->node_id & 16383] = 0;
                }
                for (int n = 0; n < max_walks && n < vocab->net->node_count; n++) {
                    ReasoningNode* start = vocab->net->nodes[n];
                    if (!start || start->edge_count < 1) continue;
                    int len = topology_walk_greedy(vocab, start->node_id,
                        path_buf, score_buf, 6,
                        visited, 0.3f, master, NULL, NULL);
                    if (len > 0) walked++;
                }
                if (walked > 0) printf("  → 贪心游走: %d 条路径\n", walked);
                int built = template_auto_build(master, 500, 100);
                if (built > 0) printf("  → 模板构建: %d 个新模板\n", built);

                /* 词素合并: 扫描高置信相邻单字对，自动创建词级节点 */
                int merged = 0;
                DictTable* dict = (DictTable*)master->ext_dict;
                for (int n = 0; n < vocab->net->node_count && merged < 200; n++) {
                    ReasoningNode* node = vocab->net->nodes[n];
                    if (!node || !node->concept || !node->edges) continue;
                    int clen = (int)strlen(node->concept);
                    if (clen < 2 || clen > 6) continue;  /* 只处理2-6字节的单字 */
                    for (int c = 0; c < node->edge_count; c++) {
                        if (node->edges[c].confidence < 0.75f) continue;
                        ReasoningNode* nb = node->edges[c].target;
                        if (!nb || !nb->concept || nb->node_id <= node->node_id) continue;
                        int nblen = (int)strlen(nb->concept);
                        if (nblen < 2 || nblen > 6) continue;
                        /* 合并: node->concept + nb->concept */
                        char merged_word[64];
                        snprintf(merged_word, 63, "%s%s", node->concept, nb->concept);
                        if (dict && !dict_table_lookup(dict, merged_word)) {
                            dict_table_insert(dict, merged_word, 10, "n");
                            merged++;
                        }
                    }
                }
                if (merged > 0) printf("  → 词素合并: %d 个新词\n", merged);
            }

            /* POS 句式模板增量构建（基于认知控制器累积的句式模式）
             *
             * 注意合并顺序：B 的 merge 在 A 的 merge 之后执行。
             * B 的模板不会与 A 的被合并（已失活）成员合并，
             * 只会与 A 的幸存者合并。这是有意为之——
             * 失活节点 tpl_pos_len=0 不应再参与后续合并。 */
            {
                CognitiveController* cc = (CognitiveController*)master->cognitive_controller;
                if (cc) {
                    int pos_built = template_build_from_pos_patterns(master, cc, 3);
                    if (pos_built > 0) {
                        printf("  → POS 句式模板: %d 个\n", pos_built);
                        /* 合并 Pipeline B 产出与已有模板（消除同 POS 冗余） */
                        int mg = template_merge_by_pos_structure(master);
                        if (mg > 0) printf("  → 模板合并: %d 组\n", mg);
                    }
                }
            }
        }

        epoch_pairs = omp_processed;
        total_pairs += epoch_pairs;
    }

    double total_time = difftime(time(NULL), start_time);

    // 4. 保存
    printf("\n[4/4] 保存拓扑状态...\n");

    // 重建跨拓扑连接（基于学习后丰富的拓扑数据）
    printf("  重建跨拓扑连接...\n");
    int cross_rebuilt = rebuild_cross_connections(master);
    printf("    跨拓扑连接: %d 条\n", cross_rebuilt);

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

        // 同时保存特征向量（确保 features.bin 与 state 一致）
        int feat_saved = save_features(master, "features.bin");
        if (feat_saved > 0) {
            printf("  ✓ 已保存特征向量 (%d 节点)\n", feat_saved);
        } else {
            printf("  × 特征向量保存失败\n");
        }

        // 保存跨拓扑连接到独立文件
        // 注: master_save_state 已包含跨链接数据（sentinel分界），
        // 此文件为 redundancy 备份，保留以兼容 digital_life 启动时的 cross_edges.bin 加载
        int cross_saved = save_cross_edges(master, "cross_edges.bin");
        if (cross_saved > 0) {
            printf("  ✓ 已保存跨拓扑连接 (%d 条)\n", cross_saved);
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
            for (int ec = 0; ec < node->edge_count; ec++) {
                e++;
                c += node->edges[ec].confidence;
            }
        }
        vocab_edges = e;
        vocab_conf = e > 0 ? c / e : 0;
    }

    // 收集全局拓扑健康指标
    const char* topo_names_rpt[20];
    int topo_edges_rpt[20];
    int topo_count_rpt = 0;
    float conf_low_rpt = 0, conf_med_rpt = 0, conf_high_rpt = 0;
    float sat_ratio_rpt = 0, zero_deg_rpt = 0, avg_deg_rpt = 0;
    collect_topo_health(master, topo_names_rpt, topo_edges_rpt, &topo_count_rpt,
                        &conf_low_rpt, &conf_med_rpt, &conf_high_rpt,
                        &sat_ratio_rpt, &zero_deg_rpt, &avg_deg_rpt);
    int total_internal = 0;
    for (int t = 0; t < topo_count_rpt; t++) total_internal += topo_edges_rpt[t];

    printf("\n╔═══════════════════════════════════════════╗\n");
    printf("║  完成！                                   ║\n");
    printf("╠═══════════════════════════════════════════╣\n");
    printf("║  处理: %d 条 QA x %d epoch = %d 次         ║\n", qa_count, epochs, total_pairs);
    printf("║  耗时: %.0f 秒                            ║\n", total_time);
    printf("║  词汇拓扑节点: %d                          ║\n", vocab ? vocab->net->node_count : 0);
    printf("║  词汇拓扑边: %d                            ║\n", vocab_edges);
    printf("║  平均置信度: %.3f                          ║\n", vocab_conf);
    printf("║  总边(含跨拓扑): %d                        ║\n", final_edges);
    printf("║  跨拓扑连接: %d                              ║\n", master->cross_link_count);
    printf("╠═══════ 拓扑健康度 ═════════════════════════╣\n");
    printf("║  置信度分布: [低] %.0f%% [中] %.0f%% [高] %.0f%%   ║\n",
           conf_low_rpt, conf_med_rpt, conf_high_rpt);
    printf("║  权重饱和: %.0f%% | 孤立节点: %.0f%% | 平均度: %.1f ║\n",
           sat_ratio_rpt, zero_deg_rpt, avg_deg_rpt);
    printf("║  跨拓扑密度: %.1f%% (%d/%d)                  ║\n",
           total_internal > 0 ? (float)master->cross_link_count / (total_internal + master->cross_link_count) * 100.0f : 0.0f,
           master->cross_link_count, total_internal + master->cross_link_count);
    printf("╠═══════ 各拓扑边数 ═════════════════════════╣\n");
    for (int t = 0; t < topo_count_rpt && t < 20; t++) {
        printf("║  %-12s: %-6d                     ║\n",
               topo_names_rpt[t], topo_edges_rpt[t]);
    }
    printf("╚═══════════════════════════════════════════╝\n");

    // 清理
    autonomic_stop_async_flush(&state);
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