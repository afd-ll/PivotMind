/**
 * @file hebbian_pretrain.c
 * @brief Hebbian 语义预训练工具
 *
 * 读 QA 数据，对每对（输入字, 回复字）调用 hebbian_update()，
 * 让经常同时出现的字在 24 维向量空间里互相靠近。
 *
 * 用法: ./build/bin/hebbian_pretrain [状态文件] [QA文件] [epochs]
 *       默认: pivotmind_state.dat  data/hermes_knowledge_base.json  10
 *
 * 原理：
 *   字 A 和字 B 在同一组 QA 中同时出现 → 它们的特征向量互相拉近
 *   多次拉近后，同类上下文中的字自然聚类 → 语义空间浮现
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "multi_topology.h"
#include "node_hash.h"
#include "common.h"

#define MAX_CHARS 256
#define MAX_QA 100000
#define HEBBIAN_LR 0.01f

// ==================== JSON 简易解析（复用 batch_learn 逻辑） ====================

static const char* skip_ws(const char* p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static char* parse_json_string(const char** p) {
    *p = skip_ws(*p);
    if (**p != '"') return NULL;
    (*p)++;
    int len = 0;
    const char* tmp = *p;
    while (*tmp && *tmp != '"') {
        if (*tmp == '\\') { tmp++; if (*tmp) tmp++; }
        else tmp++;
        len++;
    }
    if (*tmp != '"') return NULL;
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
    if (**p == '"') (*p)++;
    return result;
}

static int read_qa_json(const char* path, char*** out_q, char*** out_a, int max_qa) {
    FILE* fp = fopen(path, "rb");
    if (!fp) { printf("  × 无法打开: %s\n", path); return 0; }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    rewind(fp);
    char* data = (char*)malloc(fsize + 1);
    fread(data, 1, fsize, fp);
    fclose(fp);
    data[fsize] = '\0';

    char** questions = (char**)calloc(max_qa, sizeof(char*));
    char** answers = (char**)calloc(max_qa, sizeof(char*));
    int count = 0;

    const char* p = data;
    p = skip_ws(p);
    if (*p != '[') { free(data); return 0; }
    p++;

    while (*p && count < max_qa) {
        p = skip_ws(p);
        if (*p == ']') break;
        if (*p != '[') { p++; continue; }
        p++;

        char* q = parse_json_string(&p);
        p = skip_ws(p);
        if (*p == ',') p++;
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

// ==================== 单字提取（去重 + 过滤非中文） ====================

static int extract_chars(const char* text, char chars[MAX_CHARS][8]) {
    int count = 0;
    for (const char* p = text; *p && count < MAX_CHARS; ) {
        int len = 1;
        if (((unsigned char)*p & 0xE0) == 0xC0) len = 2;
        else if (((unsigned char)*p & 0xF0) == 0xE0) len = 3;
        else if (((unsigned char)*p & 0xF8) == 0xF0) len = 4;
        // 只保留中文字符（3字节UTF-8）
        if (len == 3) {
            // 去重
            int dup = 0;
            for (int i = 0; i < count; i++) {
                if (memcmp(chars[i], p, 3) == 0) { dup = 1; break; }
            }
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

// ==================== 初始化节点特征向量 ====================

static void init_features(ReasoningNode* node) {
    if (!node) return;
    if (!node->features) {
        node->features = (float*)calloc(NODE_FEATURE_DIM, sizeof(float));
        node->feature_dim = NODE_FEATURE_DIM;
        // 随机初始化 [-0.1, 0.1]
        for (int d = 0; d < NODE_FEATURE_DIM; d++) {
            node->features[d] = ((float)rand() / RAND_MAX - 0.5f) * 0.2f;
        }
    }
}

// ==================== 主函数 ====================

int main(int argc, char* argv[]) {
    const char* state_path = argc > 1 ? argv[1] : "pivotmind_state.dat";
    const char* qa_path = argc > 2 ? argv[2] : "data/hermes_knowledge_base.json";
    int epochs = argc > 3 ? atoi(argv[3]) : 10;
    if (epochs < 1) epochs = 1;

    srand((unsigned int)time(NULL));

    printf("╔═══════════════════════════════════════════╗\n");
    printf("║    Hebbian 语义预训练 v1.0               ║\n");
    printf("╚═══════════════════════════════════════════╝\n\n");

    // 1. 加载拓扑
    printf("[1/4] 加载拓扑...\n");
    MasterTopology* master = master_topology_create(10);
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 8000, 10);
    master_add_sub_topology(master, TOPO_SEMANTIC, "语义拓扑", 6000, 9);
    master_add_sub_topology(master, TOPO_EMOTION, "情绪拓扑", 2000, 8);
    master_add_sub_topology(master, TOPO_SYNTAX, "语法拓扑", 500, 7);
    master_add_sub_topology(master, TOPO_CONTEXT, "上下文拓扑", 500, 6);
    master_add_sub_topology(master, TOPO_DOMAIN, "领域拓扑", 500, 5);
    master_add_sub_topology(master, TOPO_PRAGMA, "语用拓扑", 500, 4);
    master_add_sub_topology(master, TOPO_CULTURE, "文化拓扑", 500, 3);
    master_add_sub_topology(master, TOPO_CONCEPT, "概念拓扑", 8000, 9);

    FILE* test = fopen(state_path, "rb");
    if (!test) { printf("  × 找不到状态文件: %s\n", state_path); return 1; }
    fclose(test);
    int loaded = master_load_state(master, state_path);
    if (loaded <= 0) {
        printf("  × 加载失败，状态文件可能格式不兼容\n");
        master_topology_destroy(master);
        return 1;
    }
    printf("  ✓ 已加载 %d 节点\n", loaded);

    // 2. 初始化所有节点的特征向量
    printf("\n[2/4] 初始化特征向量...\n");
    int total_inited = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (node && !node->features) {
                init_features(node);
                total_inited++;
            }
        }
    }
    printf("  ✓ 已初始化 %d 个节点特征向量\n", total_inited);

    // 3. 读取 QA 数据
    printf("\n[3/4] 读取 QA 数据...\n");
    char** questions = NULL;
    char** answers = NULL;
    int qa_count = read_qa_json(qa_path, &questions, &answers, MAX_QA);
    printf("  ✓ %d 条 QA 对\n", qa_count);

    // 4. Hebbian 预训练
    printf("\n[4/4] Hebbian 预训练 (%d epoch)...\n", epochs);
    time_t start = time(NULL);
    int total_pairs = 0;

    for (int ep = 0; ep < epochs; ep++) {
        int updates = 0;
        if (epochs > 1 && ep % 10 == 0 && ep > 0) {
            double elapsed = difftime(time(NULL), start);
            printf("  epoch %d/%d  %ds\n", ep, epochs, (int)elapsed);
        }

        for (int i = 0; i < qa_count; i++) {
            char input_chars[MAX_CHARS][8];
            char reply_chars[MAX_CHARS][8];
            int ic = extract_chars(questions[i], input_chars);
            int rc = extract_chars(answers[i], reply_chars);

            if (ic == 0 || rc == 0) continue;

            // 对每对 (输入字, 回复字) 做 Hebbian 更新
            for (int ci = 0; ci < ic; ci++) {
                for (int rj = 0; rj < rc; rj++) {
                    // 在词汇拓扑中查找两个节点
                    ReasoningNode* n1 = NULL;
                    ReasoningNode* n2 = NULL;
                    for (int t = 0; t < master->sub_topo_count; t++) {
                        SubTopology* sub = master->sub_topologies[t];
                        if (!sub || !sub->net || !sub->node_hash) continue;
                        if (!n1) n1 = node_hash_find(sub->node_hash, input_chars[ci]);
                        if (!n2) n2 = node_hash_find(sub->node_hash, reply_chars[rj]);
                    }

                    if (n1 && n2 && n1->features && n2->features && n1 != n2) {
                        hebbian_update(n1->features, n2->features,
                                       NODE_FEATURE_DIM, HEBBIAN_LR);
                        updates++;
                    }
                }
            }
        }
        total_pairs += updates;

        // 每 epoch 结束后：归一化所有向量（保持数值稳定）
        for (int t = 0; t < master->sub_topo_count; t++) {
            SubTopology* sub = master->sub_topologies[t];
            if (!sub || !sub->net) continue;
            for (int n = 0; n < sub->net->node_count; n++) {
                ReasoningNode* node = sub->net->nodes[n];
                if (!node || !node->features) continue;
                float norm = 0;
                for (int d = 0; d < NODE_FEATURE_DIM; d++)
                    norm += node->features[d] * node->features[d];
                norm = sqrtf(norm);
                if (norm > 1e-6f) {
                    for (int d = 0; d < NODE_FEATURE_DIM; d++)
                        node->features[d] /= norm;
                }
            }
        }
    }

    double total_time = difftime(time(NULL), start);
    printf("  ✓ 完成: %d 次 Hebbian 更新, 耗时 %.0f 秒\n", total_pairs, total_time);

    // 4.1 输出语义聚类统计
    {
        SubTopology* vocab = NULL;
        for (int t = 0; t < master->sub_topo_count; t++) {
            if (master->sub_topologies[t] &&
                master->sub_topologies[t]->type == TOPO_VOCABULARY) {
                vocab = master->sub_topologies[t];
                break;
            }
        }
        if (vocab && vocab->net && vocab->node_hash) {
            // 选几个词看它们的最近邻
            const char* test_words[] = {"天", "学", "开", "大", NULL};
            for (int tw = 0; test_words[tw]; tw++) {
                ReasoningNode* src = node_hash_find(vocab->node_hash, test_words[tw]);
                if (!src || !src->features) continue;
                // 找最相似的5个节点
                typedef struct { float sim; const char* name; } SimItem;
                SimItem top5[5] = {{0, NULL}};
                for (int n = 0; n < vocab->net->node_count; n++) {
                    ReasoningNode* cand = vocab->net->nodes[n];
                    if (!cand || !cand->features || cand == src) continue;
                    float sim = cosine_similarity(src->features, cand->features,
                                                  NODE_FEATURE_DIM);
                    // 插入排序
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
    }

    // 5. 保存状态
    printf("\n保存状态...\n");
    char bak[520];
    snprintf(bak, 519, "%s.hebbian_bak", state_path);
    remove(bak);
    rename(state_path, bak);
    int saved = master_save_state(master, state_path);
    printf("  ✓ 已保存 %d 节点到 %s\n", saved, state_path);

    // 清理
    for (int i = 0; i < qa_count; i++) {
        free(questions[i]);
        free(answers[i]);
    }
    free(questions);
    free(answers);
    master_topology_destroy(master);

    printf("\n╔═══════════════════════════════════════════╗\n");
    printf("║  完成！语义向量已训练                      ║\n");
    printf("╚═══════════════════════════════════════════╝\n");
    return 0;
}
