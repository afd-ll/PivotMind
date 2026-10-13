/**
 * probe_emerge_run.c — 离线跑一轮「词涌现」（只读+可选存盘），用于验证段二
 *
 * 用法: probe_emerge_run <state.dat> [out_state.dat]
 *   charpairs.bin 从 $PIVOTMIND_HOME/data/ 读。
 */
#include <stdio.h>
#include <stdlib.h>
#include "multi_topology.h"
#include "word_emerge.h"

int main(int argc, char** argv) {
    if (argc < 2) { printf("用法: probe_emerge_run <state.dat> [out_state.dat]\n"); return 1; }
    MasterTopology* m = master_topology_create(11);
    master_add_sub_topology(m, TOPO_VOCABULARY, "词汇", 100000, 10);
    master_add_sub_topology(m, TOPO_SEMANTIC,   "语义", 50000, 9);
    master_add_sub_topology(m, TOPO_EMOTION,    "情绪", 4000, 8);
    master_add_sub_topology(m, TOPO_SYNTAX,     "语法", 1000, 7);
    master_add_sub_topology(m, TOPO_CONTEXT,    "上下文", 1000, 6);
    master_add_sub_topology(m, TOPO_DOMAIN,     "领域", 1000, 5);
    master_add_sub_topology(m, TOPO_PRAGMA,     "语用", 1000, 4);
    master_add_sub_topology(m, TOPO_CULTURE,    "文化", 1000, 3);
    master_add_sub_topology(m, TOPO_CONCEPT,    "概念", 50000, 9);
    master_add_sub_topology(m, TOPO_MASTER,     "主", 100, 0);
    master_add_sub_topology(m, TOPO_TEMPLATE,   "模板", 20000, 8);
    int n = master_load_state(m, argv[1]);
    if (n <= 10) { printf("× 状态加载失败 (%d)\n", n); return 1; }
    printf("✓ 状态 %d 节点\n", n);
    unsigned mc; double mp, me; int mx;
    word_emerge_thresholds(&mc, &mp, &me, &mx);
    printf("判据：次数>=%u · PMI>=%.2f · min熵>=%.2f · 单轮上限=%d\n", mc, mp, me, mx);
    int c = word_emerge_tick(m);
    printf("⇒ 本轮新建词节点 = %d\n", c);
    if (argc > 2 && c > 0) {
        int saved = master_save_state(m, argv[2]);
        printf("已存盘 %s (%d)\n", argv[2], saved);
    }
    master_topology_destroy(m);
    return 0;
}
