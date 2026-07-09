/* qa_crawler.c — QA 自动采集训练工具
 *
 * 用法: qa_crawler [--queries queries.txt] [--rounds N] [--save-every N]
 *
 * 1. 搜索指定关键词列表
 * 2. 从搜索结果中提取 QA 对
 * 3. 喂入自主学习的 Hebbian 管线
 * 4. 定期保存拓扑状态
 *
 * 默认内置 50 个中文对话领域关键词
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "multi_topology.h"
#include "perception.h"
#include "memory_system.h"
#include "active_learner.h"
#include "web_fetch.h"
#include "node_hash.h"

/* 默认搜索关键词 — 涵盖日常对话、技术、文化、科普等领域 */
static const char* g_default_queries[] = {
    /* 日常对话 */
    "聊天", "对话", "问答", "访谈", "采访",
    /* 技术科普 */
    "什么是", "怎么用", "如何学习", "入门教程", "基础知识",
    /* 文化生活 */
    "美食做法", "旅游攻略", "健康知识", "养生", "育儿经验",
    /* 科技AI */
    "人工智能", "编程入门", "计算机基础", "软件开发", "数据分析",
    /* 学习教育 */
    "学习方法", "考试技巧", "英语学习", "数学思维", "阅读方法",
    /* 社科人文 */
    "历史故事", "哲学思考", "经济学", "心理学", "社会学",
    /* 实用技能 */
    "做饭", "摄影", "理财", "投资", "买房经验",
    /* 自然科学 */
    "天文知识", "物理常识", "化学实验", "生物世界", "地理百科",
    /* 人生指南 */
    "职业规划", "人际关系", "情绪管理", "时间管理", "创业经验",
    /* 艺术文学 */
    "电影推荐", "好书推荐", "音乐欣赏", "绘画入门", "设计思维",
};

#define QUERY_COUNT (int)(sizeof(g_default_queries) / sizeof(g_default_queries[0]))
#define MAX_CUSTOM_QUERIES 512

int main(int argc, char* argv[]) {
    /* 解析参数 */
    const char* queries_file = NULL;
    int max_rounds = 3;       /* 遍历查询列表几轮 */
    int save_every = 10;      /* 每 N 个查询保存一次 */
    int delay_between = 3;    /* 查询间隔秒数 */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--queries") == 0 && i + 1 < argc)
            queries_file = argv[++i];
        else if (strcmp(argv[i], "--rounds") == 0 && i + 1 < argc)
            max_rounds = atoi(argv[++i]);
        else if (strcmp(argv[i], "--save-every") == 0 && i + 1 < argc)
            save_every = atoi(argv[++i]);
        else if (strcmp(argv[i], "--delay") == 0 && i + 1 < argc)
            delay_between = atoi(argv[++i]);
    }

    /* 加载查询列表 */
    const char** queries = g_default_queries;
    int query_count = QUERY_COUNT;

    char* custom_queries[MAX_CUSTOM_QUERIES];
    int custom_count = 0;

    if (queries_file) {
        FILE* f = fopen(queries_file, "r");
        if (!f) { fprintf(stderr, "无法打开 %s，使用默认列表\n", queries_file); }
        else {
            char line[256];
            while (fgets(line, sizeof(line), f) && custom_count < MAX_CUSTOM_QUERIES) {
                line[strcspn(line, "\r\n")] = '\0';
                if (line[0]) {
                    custom_queries[custom_count] = strdup(line);
                    custom_count++;
                }
            }
            fclose(f);
            queries = (const char**)custom_queries;
            query_count = custom_count;
        }
    }

    if (query_count <= 0) {
        fprintf(stderr, "无查询词\n");
        return 1;
    }

    printf("╔══════════════════════════════════════╗\n");
    printf("║  QA 自动采集训练工具                ║\n");
    printf("╠══════════════════════════════════════╣\n");
    printf("║  查询词数: %-25d ║\n", query_count);
    printf("║  训练轮数: %-25d ║\n", max_rounds);
    printf("║  保存间隔: %-25d ║\n", save_every);
    printf("║  请求间隔: %-25d ║\n", delay_between);
    printf("╚══════════════════════════════════════╝\n\n");

    /* 初始化系统 */
    printf("[1/4] 创建拓扑...\n");
    MasterTopology* topo = master_topology_create(16);
    if (!topo) { fprintf(stderr, "拓扑创建失败\n"); return 1; }

    /* 初始化所有子拓扑（autonomic_learn_from_dialog 依赖词汇拓扑） */
    master_add_sub_topology(topo, TOPO_VOCABULARY, "词汇拓扑", 30000, 10);
    master_add_sub_topology(topo, TOPO_SEMANTIC,   "语义拓扑",  4000,  9);
    master_add_sub_topology(topo, TOPO_EMOTION,    "情绪拓扑",  1000,  8);
    master_add_sub_topology(topo, TOPO_SYNTAX,     "语法拓扑",  1000,  7);
    master_add_sub_topology(topo, TOPO_CONTEXT,    "上下文拓扑", 1000,  6);
    master_add_sub_topology(topo, TOPO_DOMAIN,     "领域拓扑",  1000,  5);
    master_add_sub_topology(topo, TOPO_PRAGMA,     "语用拓扑",  1000,  4);
    master_add_sub_topology(topo, TOPO_CULTURE,    "文化拓扑",  1000,  3);
    master_add_sub_topology(topo, TOPO_CONCEPT,    "概念拓扑",  6000,  9);
    master_add_sub_topology(topo, TOPO_MASTER,     "主拓扑",     100,  0);

    printf("[2/4] 创建记忆系统...\n");
    MemorySystem* mem = memory_system_create(64, 256, 1024);
    if (!mem) { master_topology_destroy(topo); return 1; }

    printf("[3/4] 初始化网络...\n");
    CrawlPolicy policy = CRAWL_POLICY_DEFAULT;
    policy.respect_robots = 0;
    policy.request_delay_ms = 3000;
    web_fetch_init(&policy);

    printf("[4/4] 创建感知皮层 + 5 搜索引擎...\n");
    ActiveLearner* learner = (ActiveLearner*)calloc(1, sizeof(ActiveLearner));
    Perception* p = perception_create(topo, mem, learner, NULL);
    if (!p) {
        fprintf(stderr, "感知皮层创建失败\n");
        memory_system_destroy(mem);
        master_topology_destroy(topo);
        return 1;
    }

    /* 训练循环 */
    long total_pairs = 0;
    long total_queries = 0;
    time_t start_time = time(NULL);

    for (int round = 0; round < max_rounds; round++) {
        printf("\n=== 第 %d/%d 轮 ===\n", round + 1, max_rounds);

        for (int qi = 0; qi < query_count; qi++) {
            const char* query = queries[qi];
            printf("[%ld] 搜索 '%s' ... ", total_queries + 1, query);
            fflush(stdout);

            int learned = perception_search_and_learn_qa(p, query, 3);
            total_queries++;

            if (learned > 0) {
                total_pairs += learned;
                printf("✓ 学到 %d 对 QA\n", learned);
            } else {
                printf("- 无 QA 对\n");
            }

            /* 定期保存 */
            if (total_queries % save_every == 0 && total_queries > 0) {
                printf("  [保存] 写入 pivotmind_state.dat (%ld 对, %ld 查询)\n",
                       total_pairs, total_queries);
                master_save_state(topo, "pivotmind_state.dat");
            }

            /* 间隔等待 */
            if (qi < query_count - 1 || round < max_rounds - 1)
                sleep(delay_between);
        }
    }

    /* 最终保存 */
    printf("\n[最终保存] 共 %ld 对 QA, %ld 次查询\n", total_pairs, total_queries);
    master_save_state(topo, "pivotmind_state.dat");
    save_features(topo, "features.bin");

    /* 统计 */
    time_t elapsed = time(NULL) - start_time;
    printf("\n╔══════════════════════════════════════╗\n");
    printf("║  训练完成                           ║\n");
    printf("╠══════════════════════════════════════╣\n");
    printf("║  QA 对: %-28ld ║\n", total_pairs);
    printf("║  搜索次数: %-25ld ║\n", total_queries);
    printf("║  耗时: %-30ld ║\n", (long)elapsed);
    printf("║  状态文件: pivotmind_state.dat     ║\n");
    printf("║  特征文件: features.bin             ║\n");
    printf("╚══════════════════════════════════════╝\n");

    /* 清理 */
    perception_destroy(p);
    memory_system_destroy(mem);
    master_topology_destroy(topo);
    free(learner);
    for (int i = 0; i < custom_count; i++) free(custom_queries[i]);

    return total_pairs > 0 ? 0 : 1;
}
