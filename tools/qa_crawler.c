/* qa_crawler.c — QA 自动训练（fork+exec curl 版）
 *
 * 绕过 libcurl 超时问题，用 fork/exec 执行 curl 命令行，
 * 父进程 waiter 超时保证不会无限卡死。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

#include "multi_topology.h"
#include "perception.h"
#include "memory_system.h"
#include "autonomic_learner.h"
#include "dream_engine.h"

static const char* g_queries[] = {
    "聊天","对话","问答","访谈","采访",
    "什么是","怎么用","如何学习","入门教程","基础知识",
    "美食做法","旅游攻略","健康知识","养生","育儿经验",
    "人工智能","编程入门","计算机基础","软件开发","数据分析",
    "学习方法","考试技巧","英语学习","数学思维","阅读方法",
    "历史故事","哲学思考","经济学","心理学","社会学",
    "做饭","摄影技巧","理财方法","投资入门","买房经验",
    "天文知识","物理常识","化学实验","生物世界","地理百科",
    "职业规划","人际关系","情绪管理","时间管理","创业经验",
    "电影推荐","好书推荐","音乐欣赏","绘画入门","设计思维",
};
#define Q_COUNT (int)(sizeof(g_queries)/sizeof(g_queries[0]))

static char* curl_search(const char* query) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "/tmp/qa_crawl_%d.html", (int)getpid());

    pid_t p = fork();
    if (p < 0) return NULL;
    if (p == 0) {
        char qurl[512];
        snprintf(qurl, sizeof(qurl),
                 "https://weixin.sogou.com/weixin?type=2&query=%s", query);
        execlp("curl", "curl", "-sL", "--max-time", "8", "--connect-timeout", "4",
               "-A", "Mozilla/5.0", "-o", tmp, qurl, NULL);
        _exit(1);
    }

    int st;
    time_t t0 = time(NULL);
    while (time(NULL) - t0 < 10) {
        if (waitpid(p, &st, WNOHANG) == p) break;
        usleep(100000);
    }
    if (time(NULL) - t0 >= 10) { kill(p, SIGKILL); waitpid(p, NULL, 0); return NULL; }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return NULL;

    FILE* f = fopen(tmp, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > 262144) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char* buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f);
    buf[sz] = 0;
    fclose(f);
    return buf;
}

int main(int argc, char* argv[]) {
    const char* qf = NULL;
    int rounds = 2, save_every = 10, delay_s = 5;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--queries") && i+1 < argc) qf = argv[++i];
        else if (!strcmp(argv[i], "--rounds") && i+1 < argc) rounds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--save-every") && i+1 < argc) save_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--delay") && i+1 < argc) delay_s = atoi(argv[++i]);
    }

    const char** qs = g_queries;
    int qc = Q_COUNT;
    char* custom[512]; int cc = 0;
    if (qf) {
        FILE* f = fopen(qf, "r"); if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f) && cc < 512) {
                line[strcspn(line, "\r\n")] = 0;
                if (line[0]) custom[cc++] = strdup(line);
            } fclose(f);
            qs = (const char**)custom; qc = cc;
        }
    }

    printf("=== QA Crawler === %d queries x %d rounds ===\n\n", qc, rounds);

    MasterTopology* topo = master_topology_create(16);
    master_add_sub_topology(topo, TOPO_VOCABULARY,"词汇拓扑",30000,10);
    master_add_sub_topology(topo, TOPO_SEMANTIC, "语义拓扑", 4000, 9);
    master_add_sub_topology(topo, TOPO_EMOTION,  "情绪拓扑", 1000, 8);
    master_add_sub_topology(topo, TOPO_SYNTAX,   "语法拓扑", 1000, 7);
    master_add_sub_topology(topo, TOPO_CONTEXT,  "上下文拓扑",1000,6);
    master_add_sub_topology(topo, TOPO_DOMAIN,   "领域拓扑", 1000, 5);
    master_add_sub_topology(topo, TOPO_PRAGMA,   "语用拓扑", 1000, 4);
    master_add_sub_topology(topo, TOPO_CULTURE,  "文化拓扑", 1000, 3);
    master_add_sub_topology(topo, TOPO_CONCEPT,  "概念拓扑", 6000, 9);
    master_add_sub_topology(topo, TOPO_MASTER,   "主拓扑",   100,  0);

    MemorySystem* mem = memory_system_create(64, 256, 1024);
    AutonomicState astate;
    memset(&astate, 0, sizeof(astate));
    autonomic_state_init(&astate);

    long tpairs = 0, tqueries = 0;
    for (int rd = 0; rd < rounds; rd++) {
        printf("\n--- Round %d/%d ---\n", rd+1, rounds);
        for (int qi = 0; qi < qc; qi++) {
            const char* q = qs[qi];
            tqueries++;
            printf("[%ld] '%s' ... ", tqueries, q); fflush(stdout);

            char* html = curl_search(q);
            if (!html) { printf("fetch fail\n"); sleep(delay_s); continue; }

            char questions[64][512], answers[64][2048];
            int pc = perception_extract_qa_pairs(html, questions, answers, 64);
            free(html);

            if (pc == 0) { printf("no QA\n"); sleep(delay_s); continue; }

            for (int i = 0; i < pc; i++) {
                autonomic_learn_from_dialog(topo, questions[i], answers[i], &astate, NULL, mem);
                dream_enqueue_qa(questions[i], answers[i]);  /* 入队梦境重放 */
            }
            tpairs += pc;
            printf("+%d QA\n", pc);

            if (tqueries % save_every == 0)
                master_save_state(topo, "pivotmind_state.dat");

            sleep(delay_s);
        }
    }

    autonomic_state_destroy(&astate);
    int nodes = master_save_state(topo, "pivotmind_state.dat");
    printf("\n=== Done: %ld QA pairs, %ld queries, %d nodes ===\n", tpairs, tqueries, nodes);

    memory_system_destroy(mem);
    master_topology_destroy(topo);
    for (int i = 0; i < cc; i++) free(custom[i]);
    return 0;
}
