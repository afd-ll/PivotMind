/**
 * quick_chat.c — 快速对话测试
 * 加载训练状态 + 词典，立即回答
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multi_topology.h"
#include "autonomic_learner.h"
#include "dialog_system.h"
#include "dict_loader.h"

int main(int argc, char** argv) {
    const char* state_path = argc > 1 ? argv[1] : "pivotmind_state.dat";
    const char* question   = argc > 2 ? argv[2] : "你好，介绍下你自己";

    setbuf(stdout, NULL);
    printf("=== 玄枢对话测试 ===\n\n状态: %s\n问题: %s\n\n", state_path, question);

    MasterTopology* master = master_topology_create(11);
    master_add_sub_topology(master, TOPO_VOCABULARY, "", 30000, 10);
    master_add_sub_topology(master, TOPO_SEMANTIC,   "", 12000, 9);
    master_add_sub_topology(master, TOPO_EMOTION,    "", 4000, 8);
    master_add_sub_topology(master, TOPO_SYNTAX,     "", 1000, 7);
    master_add_sub_topology(master, TOPO_CONTEXT,    "", 1000, 6);
    master_add_sub_topology(master, TOPO_DOMAIN,     "", 1000, 5);
    master_add_sub_topology(master, TOPO_PRAGMA,     "", 1000, 4);
    master_add_sub_topology(master, TOPO_CULTURE,    "", 1000, 3);
    master_add_sub_topology(master, TOPO_CONCEPT,    "", 12000, 9);
    master_add_sub_topology(master, TOPO_MASTER,     "", 100, 0);
    master_add_sub_topology(master, TOPO_TEMPLATE,   "", 4000, 8);

    FILE* df = fopen("data/jieba_dict.txt", "r");
    if (df) { fclose(df); DictTable* d = dict_table_create(524288); dict_load_jieba(d,"data/jieba_dict.txt"); master->ext_dict = d; }

    printf("加载状态...\n");
    int loaded = master_load_state(master, state_path);
    printf("%d 节点\n\n", loaded);
    if (loaded <= 10) { printf("× 状态加载异常\n"); return 1; }

    master_get_thread_pool(master);

    printf("初始化对话...\n");
    MemorySystem* mem = memory_system_create(100, 500, 2000);
    DialogSystem* ds = dialog_system_create(master, mem, NULL, NULL);
    if (!ds) { printf("× 失败\n"); return 1; }

    printf("生成回答...\n");
    fflush(stdout);
    DialogReasoning* reasoning = NULL;
    fprintf(stderr, ">>> calling dialog_process...\n"); fflush(stderr);
    char* resp = dialog_process(ds, question, &reasoning);
    fprintf(stderr, ">>> resp = %p (%s)\n", (void*)resp, resp ? resp : "NULL"); fflush(stderr);
    fprintf(stderr, ">>> AI: %s\n", resp ? resp : "(无回答)"); fflush(stderr);
    printf("\n>>> AI: %s\n\n", resp ? resp : "(空)");
    fflush(stdout);
    if (reasoning) {
        printf("推理: 深度=%d 激活=%.2f 质量=%.2f 联想=%d 链长=%d\n",
               reasoning->path_depth, reasoning->avg_activation,
               reasoning->knowledge_quality, reasoning->assoc_count,
               reasoning->chain_length);
    }

    if (resp) free(resp);
    dialog_system_destroy(ds);
    memory_system_destroy(mem);
    master_topology_destroy(master);
    return 0;
}
