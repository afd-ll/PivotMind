/**
 * tools/seed_builder.c
 * PivotMind v0.2 种子模型构建器
 * 
 * 构建完整的种子模型:
 * 1. 拓扑网络种子（概念节点 + 关联）
 * 2. 记忆系统种子（Q&A响应模式）
 * 3. 保存为 pivotmind_state.dat
 *
 * 编译: gcc -Iinclude -I. -Ilibs -std=gnu99 -O2 -fopenmp -D_USE_MATH_DEFINES -pthread
 *        -o build/bin/seed_builder tools/seed_builder.c
 *        src/tensor.c ... src/concept_processor.c -lm
 * 用法: ./build/bin/seed_builder [knowledge_base.json]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>

#include "multi_topology.h"
#include "string_pool.h"
#include "memory_system.h"
#include "active_learner.h"
#include "dialog_system.h"
#include "huarong_topology.h"
#include "node_hash.h"
#include "common.h"

// 最大支持
#define MAX_CONCEPTS 50000
#define MAX_LINE 4096
#define MAX_PAIRS 5000

// ========== 基础种子概念 ==========
static const char* base_concepts[] = {
    "我", "你", "是", "的", "了", "在", "有", "和", "不", "好",
    "这", "那", "什么", "怎么", "为什么", "如何", "可以", "能",
    "会", "要", "想", "知道", "请问", "谢谢", "再见", "hello",
    "hi", "yes", "no", "ok", "嗯", "哦", "啊", "吗", "吧",
    "对", "错", "是", "不是", "好", "不好", "大", "小", "多", "少",
    "高兴", "开心", "难过", "生气", "好奇", "困惑", "感谢",
    "神经网络", "深度学习", "机器学习", "人工智能",
    "数据", "算法", "模型", "训练", "学习", "编程", "代码",
    "玄枢", "PivotMind", "溯智系统", "拓扑网络",
    "概念", "推理", "记忆", "联想", "理解", "知识",
    "人", "时间", "世界", "问题", "答案", "原因", "结果",
};
static const int base_count = sizeof(base_concepts) / sizeof(base_concepts[0]);

// 基础跨拓扑关联
static const char* base_links[][3] = {
    {"你好", "再见", "0.5"},
    {"神经网络", "深度学习", "0.8"},
    {"机器学习", "深度学习", "0.7"},
    {"人工智能", "机器学习", "0.8"},
    {"人工智能", "神经网络", "0.7"},
    {"数据", "算法", "0.6"},
    {"训练", "学习", "0.8"},
    {"模型", "训练", "0.7"},
    {"高兴", "开心", "0.8"},
    {"难过", "生气", "0.5"},
    {"好奇", "困惑", "0.6"},
    {"感谢", "谢谢", "0.8"},
    {"对不起", "没关系", "0.6"},
    {"玄枢", "PivotMind", "0.9"},
    {"玄枢", "溯智系统", "0.9"},
    {"PivotMind", "溯智系统", "0.8"},
    {"拓扑网络", "玄枢", "0.7"},
    {"知识", "学习", "0.7"},
    {"知识", "概念", "0.6"},
    {"推理", "联想", "0.7"},
    {"问题", "答案", "0.6"},
    {"原因", "结果", "0.6"},
};
static const int base_link_count = sizeof(base_links) / sizeof(base_links[0]);

// ========== 工具函数 ==========

// 跳过 UTF-8 BOM
static void skip_bom(FILE* fp) {
    unsigned char bom[3];
    if (fread(bom, 1, 3, fp) == 3) {
        if (!(bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF)) {
            fseek(fp, 0, SEEK_SET);
        }
    } else {
        fseek(fp, 0, SEEK_SET);
    }
}

// 简单的 JSON 问答对解析
static int parse_knowledge_json(const char* filepath,
                                 char questions[][MAX_LINE],
                                 char answers[][MAX_LINE],
                                 int max_pairs) {
    FILE* fp = fopen(filepath, "rb");
    if (!fp) { printf("[种子] 无法打开: %s\n", filepath); return 0; }

    skip_bom(fp);

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize <= 0) { fclose(fp); return 0; }

    // 去掉 \r 方便处理
    char* content = (char*)malloc(fsize + 1);
    if (!content) { fclose(fp); return 0; }

    size_t actual = fread(content, 1, fsize, fp);
    content[actual] = '\0';
    fclose(fp);

    // 去掉所有 \r
    int wi = 0;
    for (size_t i = 0; i < actual; i++) {
        if (content[i] != '\r') content[wi++] = content[i];
    }
    content[wi] = '\0';

    int count = 0;
    char* p = content;

    // 先跳过外层的第一个 [
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    while (*p && (*p <= ' ')) p++;

    while (*p && count < max_pairs) {
        // 找内层 [
        while (*p && *p != '[') p++;
        if (!*p) break;
        p++; // 跳过 [

        // 跳过空白和换行
        while (*p && (*p <= ' ')) p++;
        if (!*p || *p == ']') continue;

        // 应该到引号了
        if (*p != '"') continue;
        p++; // 跳过 "

        // 读 question
        int qi = 0;
        while (*p && *p != '"' && qi < MAX_LINE - 1) {
            if (*p == '\\' && *(p+1)) p++;
            questions[count][qi++] = *p;
            p++;
        }
        questions[count][qi] = '\0';
        if (*p == '"') p++;

        // 找下一个 "
        while (*p && *p != '"') p++;
        if (!*p) break;
        p++; // 跳过 "

        // 读 answer
        int ai = 0;
        while (*p && *p != '"' && ai < MAX_LINE - 1) {
            if (*p == '\\' && *(p+1)) p++;
            answers[count][ai++] = *p;
            p++;
        }
        answers[count][ai] = '\0';

        count++;
    }

    free(content);
    return count;
}

// 添加单个概念到各层拓扑
static float* make_random_feat() {
    float* f = malloc(NODE_FEATURE_DIM * sizeof(float));
    for (int i = 0; i < NODE_FEATURE_DIM; i++)
        f[i] = ((float)rand() / RAND_MAX - 0.5f) * 0.1f;
    return f;
}

static void add_concept(MasterTopology* master, const char* concept) {
    if (!master || !concept || !*concept) return;
    int len = strlen(concept);

    // 多字词 → 概念拓扑 (topo 8)
    if (len >= 2) {
        SubTopology* ct = master_get_sub_topology(master, 8);
        if (ct && ct->net) {
            ReasoningNode* n = node_hash_find(ct->node_hash, concept);
            if (!n) {
                float* feat = make_random_feat();
                n = huarong_net_add_node(ct->net, concept, feat, NODE_FEATURE_DIM);
                free(feat);
                if (n) { node_hash_add(ct->node_hash, n); n->activation = 0.8f; }
            }
        }
    }

    // 词汇拓扑（逐字）+ 语义拓扑
    for (int i = 0; i < len; ) {
        int char_len = 1;
        if ((concept[i] & 0xE0) == 0xC0) char_len = 2;
        else if ((concept[i] & 0xF0) == 0xE0) char_len = 3;
        else if ((concept[i] & 0xF8) == 0xF0) char_len = 4;

        if (i + char_len > len) break;

        char ch[8]; memcpy(ch, concept + i, char_len); ch[char_len] = '\0';

        for (int t = 0; t <= 1; t++) {
            SubTopology* sub = master_get_sub_topology(master, t);
            if (!sub || !sub->net) continue;
            ReasoningNode* n = node_hash_find(sub->node_hash, ch);
            if (!n) {
                float* feat = make_random_feat();
                n = huarong_net_add_node(sub->net, ch, feat, NODE_FEATURE_DIM);
                free(feat);
                if (n) {
                    node_hash_add(sub->node_hash, n);
                    n->activation = (t == 0) ? 0.4f : 0.5f;
                }
            }
        }
        i += char_len;
    }
}

// 在拓扑中查找概念节点
typedef struct { SubTopology* sub; int node_id; } NodeLoc;
static int find_concept_nodes(MasterTopology* master, const char* concept,
                               NodeLoc* out, int max_out) {
    int count = 0;
    for (int t = 0; t < master->sub_topo_count && count < max_out; t++) {
        SubTopology* sub = master_get_sub_topology(master, t);
        if (!sub || !sub->net || !sub->node_hash) continue;
        ReasoningNode* n = node_hash_find(sub->node_hash, concept);
        if (n) {
            out[count].sub = sub;
            out[count].node_id = n->node_id;
            count++;
        }
    }
    return count;
}

// 建立概念关联
static void link_concepts(MasterTopology* master, const char* c1, const char* c2, float weight) {
    NodeLoc nodes1[4], nodes2[4];
    int n1 = find_concept_nodes(master, c1, nodes1, 4);
    int n2 = find_concept_nodes(master, c2, nodes2, 4);
    for (int i = 0; i < n1 && i < n2; i++) {
        master_add_cross_link(master,
            nodes1[i].sub->topo_id, nodes1[i].node_id,
            nodes2[i].sub->topo_id, nodes2[i].node_id,
            weight, "seed");
    }
}

// 将 QString 存入 memory（模拟教学机制的存储格式）
static void seed_memory(MemorySystem* memory, const char* question, const char* answer) {
    if (!memory || !question || !answer) return;

    // 存储完整问答对
    char key[MAX_LINE];
    snprintf(key, sizeof(key), "response:%s", question);
    char* data = strdup(answer);
    memory_store(memory, key, data, strlen(answer) + 1,
                 MEMORY_TYPE_STRING, 0.9f);

    // 也从问题中提取关键词单独存
    char q_copy[MAX_LINE];
    strncpy(q_copy, question, MAX_LINE - 1);

    // 提取两个字符以上的词
    const char* delimiters = "的了吗呢啊吧么个是和在的有要不这与那也";
    char* token = strtok(q_copy, delimiters);
    while (token) {
        if (strlen(token) >= 2) {
            char k2[MAX_LINE];
            snprintf(k2, sizeof(k2), "response:%s", token);
            MemoryEntry* existing = memory_retrieve(memory, k2);
            if (!existing) {
                char* d2 = strdup(answer);
                memory_store(memory, k2, d2, strlen(answer) + 1,
                             MEMORY_TYPE_STRING, 0.7f);
            }
        }
        token = strtok(NULL, delimiters);
    }
}

// ========== 主函数 ==========
int main(int argc, char* argv[]) {
    printf("╔═══════════════════════════════════════════╗\n");
    printf("║     PivotMind v0.2 种子模型构建器        ║\n");
    printf("╚═══════════════════════════════════════════╝\n\n");

    const char* kb_file = argc > 1 ? argv[1] : "data/knowledge_base.json";

    // 1. 创建系统
    printf("[1/5] 创建系统组件...\n");
    MasterTopology* master = master_topology_create(9);
    if (!master) { printf("错误\n"); return 1; }

    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 1000, 10);
    master_add_sub_topology(master, TOPO_SEMANTIC, "语义拓扑", 500, 9);
    master_add_sub_topology(master, TOPO_EMOTION, "情绪拓扑", 200, 8);
    master_add_sub_topology(master, TOPO_SYNTAX, "语法拓扑", 300, 7);
    master_add_sub_topology(master, TOPO_CONTEXT, "上下文拓扑", 300, 6);
    master_add_sub_topology(master, TOPO_DOMAIN, "领域拓扑", 200, 5);
    master_add_sub_topology(master, TOPO_PRAGMA, "语用拓扑", 150, 4);
    master_add_sub_topology(master, TOPO_CULTURE, "文化拓扑", 150, 3);
    master_add_sub_topology(master, TOPO_CONCEPT, "概念拓扑", 500, 9);

    MemorySystem* memory = memory_system_create(100, 500, 2000);
    if (!memory) { printf("错误: memory\n"); return 1; }

    printf("   ✓ 系统就绪\n");

    // 2. 拓扑种子
    printf("[2/5] 灌入基础概念 (%d 个)...\n", base_count);
    for (int i = 0; i < base_count; i++) add_concept(master, base_concepts[i]);

    printf("[3/5] 加载知识库: %s\n", kb_file);
    static char questions[MAX_PAIRS][MAX_LINE];
    static char answers[MAX_PAIRS][MAX_LINE];
    int pair_count = parse_knowledge_json(kb_file, questions, answers, MAX_PAIRS);
    printf("   ✓ %d 条问答对\n", pair_count);

    // 从问答对中提取概念并灌入拓扑
    for (int i = 0; i < pair_count; i++) {
        char* all_texts[] = { questions[i], answers[i] };
        for (int s = 0; s < 2; s++) {
            char tmp[MAX_LINE];
            strncpy(tmp, all_texts[s], MAX_LINE - 1);
            char* t = strtok(tmp, "，。！？、；：""''（）【】《》?.,!;:()[]{}\"' ");
            while (t) {
                if (strlen(t) >= 2) add_concept(master, t);
                t = strtok(NULL, "，。！？、；：""''（）【】《》?.,!;:()[]{}\"' ");
            }
        }
    }

    // 3. 建立基础关联
    printf("[4/5] 建立关联...\n");
    for (int i = 0; i < base_link_count; i++) {
        link_concepts(master, base_links[i][0], base_links[i][1],
                      atof(base_links[i][2]));
    }
    // 问答对关联：问句关键词 ↔ 答句关键词
    for (int i = 0; i < pair_count && i < 500; i++) {
        char q1[MAX_LINE], a1[MAX_LINE];
        strncpy(q1, questions[i], MAX_LINE - 1);
        strncpy(a1, answers[i], MAX_LINE - 1);

        char* qk = strtok(q1, "的了吗呢啊吧么个是和在的有要不这与那也");
        char* ak = strtok(a1, "的了吗呢啊吧么个是和在的有要不这与那也");
        if (qk && strlen(qk) >= 2 && ak && strlen(ak) >= 2) {
            link_concepts(master, qk, ak, 0.5f);
        }
    }

    // 4. 记忆种子
    printf("[5/5] 灌入记忆种子...\n");
    for (int i = 0; i < pair_count; i++) {
        seed_memory(memory, questions[i], answers[i]);

        // 也存一些常见问法的变体
        char qq[MAX_LINE];
        strncpy(qq, questions[i], MAX_LINE - 1);
        char* prefixes[] = {"什么是", "如何", "怎么", "怎样", "为什么"};
        char* content = NULL;
        for (int p = 0; p < 5; p++) {
            char* found = strstr(qq, prefixes[p]);
            if (found) { content = found + strlen(prefixes[p]); break; }
        }
        if (content && strlen(content) >= 2) {
            int cl = strlen(content);
            while (cl > 0 && strchr("的了吗呢？。！，", content[cl-1])) cl--;
            content[cl] = '\0';
            if (strlen(content) >= 2) {
                char k[MAX_LINE];
                snprintf(k, sizeof(k), "response:%s", content);
                MemoryEntry* existing = memory_retrieve(memory, k);
                if (!existing) {
                    char* d = strdup(answers[i]);
                    memory_store(memory, k, d, strlen(answers[i]) + 1,
                                 MEMORY_TYPE_STRING, 0.8f);
                }
            }
        }
    }

    // 5. 统计
    printf("\n=== 统计 ===\n");
    int total_nodes = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master_get_sub_topology(master, t);
        if (sub && sub->net) {
            printf("   %s: %d 节点\n", sub->name, sub->net->node_count);
            total_nodes += sub->net->node_count;
        }
    }
    printf("   共 %d 节点, %d 跨拓扑链接, 记忆种子: %d 条\n",
           total_nodes, master->cross_link_count, pair_count);

    // 6. 保存状态
    const char* state_file = "pivotmind_state.dat";
    printf("\n保存到 %s...\n", state_file);
    master_save_state(master, state_file);
    
    // 保存记忆种子
    const char* mem_file = "memory_seed.dat";
    memory_save_seed(memory, mem_file);

    // 7. 清理
    memory_system_destroy(memory);
    master_topology_destroy(master);

    printf("\n✓ 种子模型构建完成！\n");
    return 0;
}
