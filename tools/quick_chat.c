/**
 * quick_chat.c — 快速对话测试
 * 加载训练状态 + 词典，立即回答
 */
#include "pivotmind_paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multi_topology.h"
#include "autonomic_learner.h"
#include "dialog_system.h"
#include "dict_loader.h"

int main(int argc, char** argv) {
    const char* state_path = argc > 1 ? argv[1] : pm_file(PM_FILE_STATE);
    const char* question   = argc > 2 ? argv[2] : "你好，介绍下你自己";

    setbuf(stdout, NULL);

    /* 旧扁平布局审计（v0.5.33）：交互式工具，只告警不拒绝。 */
    (void)pm_legacy_layout_guard("quick_chat", 0);
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

    {
        /* 路径 SSOT：词典落点是 <home>/data/jieba_dict.txt，不再以 cwd 为基准。
         * 原实现 fopen 失败后【静默跳过】—— 词典缺失会让分词退化为逐字，必须说出来。 */
        const char* dict_path = pm_asset(PM_ASSET_JIEBA_DICT);
        FILE* df = (dict_path != NULL) ? fopen(dict_path, "r") : NULL;
        if (df) {
            fclose(df);
            DictTable* d = dict_table_create(524288);
            dict_load_jieba(d, dict_path);
            master->ext_dict = (struct ExternalDict*)d;
            printf("词典已加载: %s\n", dict_path);
        } else {
            fprintf(stderr, "[quick_chat] ⚠ 词典不存在: %s（逐字模式，分词质量会下降）\n",
                    (dict_path != NULL) ? dict_path : "<pm_asset 返回 NULL>");
        }
    }

    printf("加载状态...\n");
    /* P2：「缺文件」与「文件损坏」语义要分开说：
     *   缺   ⇒ 首次运行 / 空脑，属正常路径（RC=0），但必须显式告知，别让人以为失忆；
     *   损坏 ⇒ multi_topology 已 fail-loud（RC=1）。
     * multi_topology 对「打不开」也打 ERROR，故这里补一句上下文。 */
    int state_present = 0;
    {
        FILE* probe = (state_path != NULL) ? fopen(state_path, "rb") : NULL;
        if (probe != NULL) {
            fclose(probe);
            state_present = 1;
        } else {
            printf("提示: 状态文件不存在（%s）—— 将以【空脑】启动（首次运行属正常）\n",
                   (state_path != NULL) ? state_path : "?");
        }
    }
    int loaded = master_load_state(master, state_path);
    printf("%d 节点\n\n", loaded);
    if (loaded <= 10) {
        /* 「文件在、却几乎没加载出东西」才是真异常；
         * 「文件不在」是首次运行，上面已经说明过，不该再按异常收场（RC 语义要与提示一致）。 */
        if (state_present) { printf("× 状态加载异常\n"); return 1; }
        printf("（首次运行：空脑启动，属正常路径，不按加载异常处理）\n");
    }

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
