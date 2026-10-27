/* tools/temp_probe.c — 温度旋钮的作用域探针
 *
 * 用法: ./temp_probe <state_file> [reps]
 *
 * 为什么要有这个：线上「温度按意图熵调度」改完之后，用「同一输入多次回复的
 * distinct 数」做 A/B，两组差异只有 4.90 vs 5.10 —— 因为那条链路上混了
 * PFE 多步推理（不受温度影响）、QA 记忆、门控回溯，被测行为不纯。
 *
 * 这里把变量收到只剩一个：直接调 cingulate_diffusion_evaluate，
 * 固定输入、固定状态，只改 temperature，各跑 reps 次。
 *
 * 三个判据（前两个是自检，不成立就说明探针本身有问题）：
 *   ① temp=0 时多次运行必须完全一致（无扰动 ⇒ 确定性）
 *   ② temp 越大，同温度内多次运行的 distinct 越大（扰动幅度 = 方差）
 *   ③ temp 越大，与 temp=0 基准序列的一致率越低
 */
#include "pivotmind_paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multi_topology.h"
#include "huarong_topology.h"
#include "cingulate.h"
#include "emergent_pos.h"
#include "common.h"

#define MAX_INPUTS 10
#define MAX_REPS   16
#define MAX_TEMPS  6

static MasterTopology* load_master(const char* path) {
    MasterTopology* master = master_topology_create(16);
    if (!master) return NULL;
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 60000, 10);
    master_add_sub_topology(master, TOPO_SEMANTIC,   "语义拓扑", 20000, 9);
    master_add_sub_topology(master, TOPO_EMOTION,    "情绪拓扑", 8000, 8);
    master_add_sub_topology(master, TOPO_SYNTAX,     "语法拓扑", 4000, 7);
    master_add_sub_topology(master, TOPO_CONTEXT,    "上下文拓扑", 4000, 6);
    master_add_sub_topology(master, TOPO_DOMAIN,     "领域拓扑", 4000, 5);
    master_add_sub_topology(master, TOPO_PRAGMA,     "语用拓扑", 4000, 4);
    master_add_sub_topology(master, TOPO_CULTURE,    "文化拓扑", 4000, 3);
    master_add_sub_topology(master, TOPO_CONCEPT,    "概念拓扑", 20000, 9);
    master_add_sub_topology(master, TOPO_MASTER,     "主拓扑", 100, 0);
    master_add_sub_topology(master, TOPO_TEMPLATE,   "模板拓扑", 8000, 8);
    master_add_sub_topology(master, TOPO_VISUAL,     "视觉拓扑", 8000, 7);
    if (master_load_state(master, path) < 0) {
        master_topology_destroy(master);
        return NULL;
    }
    return master;
}

/* 把序列压成一个字符串，用于比较一致性 */
static void seq_key(const GeneratedSequence* s, char* buf, size_t n) {
    size_t off = 0;
    buf[0] = 0;
    for (int i = 0; i < s->count && off + 8 < n; i++) {
        const char* w = s->words[i] ? s->words[i] : "?";
        off += (size_t)snprintf(buf + off, n - off, "%s%s", i ? "|" : "", w);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "用法: %s <state_file> [reps]\n", argv[0]);
        return 2;
    }
    /* 库内部的扩散日志走 printf(stdout)，会把结果表冲散 ⇒ stdout 丢弃，结果走 stderr。
     * TEMP_PROBE_VERBOSE=1 可保留原始日志（调试探针自身时用）。 */
    /* 库内部的扩散日志全走 stderr（LOG_INFO 与 printf 都落到那里），重定向隔离不了
     * ⇒ 结果单独写文件。路径可用 TEMP_PROBE_OUT 覆盖。 */
    const char* outp = getenv("TEMP_PROBE_OUT");
    if (!outp || !outp[0]) outp = "/tmp/tp_result.txt";
    FILE* out = fopen(outp, "w");
    if (!out) { fprintf(stderr, "cannot write %s", outp); return 1; }

    int reps = (argc > 2) ? atoi(argv[2]) : 5;
    if (reps < 1) reps = 1;
    if (reps > MAX_REPS) reps = MAX_REPS;

    MasterTopology* master = load_master(argv[1]);
    if (!master) { fprintf(stderr, "加载失败: %s\n", argv[1]); return 1; }

    /* 温度档位：0 = 无扰动（自检基线）；0.06/0.15/0.32 = 熵调度的两端与中性点 */
    const float temps[MAX_TEMPS] = { 0.00f, 0.06f, 0.15f, 0.32f, 0.45f, 0.60f };
    const int n_temp = 6;

    const char* inputs[MAX_INPUTS] = {
        "什么是爱", "盐", "山中落雪", "电脑屏幕坏了", "人工智能是什么",
        "时间过得真快", "我不开心", "学习有什么用", "朋友吵架了", "这本书好看吗"
    };
    const int n_in = 10;

    fprintf(out, "[探针] 状态=%s reps=%d\n", argv[1], reps);
    fprintf(out, "[探针] 判据① temp=0 必须多次完全一致（确定性自检）\n\n");

    /* 每个 (输入, 温度) 下：去重后的序列数、与 baseline(temp=0 第一次) 的一致次数、平均总分 */
    fprintf(out, "%-16s %6s %8s %8s %9s %9s\n",
           "输入", "温度", "distinct", "候选数", "平均总分", "与基准同");
    for (int ii = 0; ii < n_in; ii++) {
        char baseline[512] = {0};
        int  base_n = 0;
        for (int ti = 0; ti < n_temp; ti++) {
            char keys[MAX_REPS][512];
            int  nk = 0;
            int  cnt_sum = 0, valid = 0, same_as_base = 0;
            double score_sum = 0.0;

            for (int r = 0; r < reps; r++) {
                GeneratedSequence seq = {0};
                int n = cingulate_diffusion_evaluate(master, inputs[ii], temps[ti], NULL, &seq);
                if (n < 2) continue;                     /* 无产出，不计入 */
                valid++;
                cnt_sum += seq.count;
                score_sum += (double)seq.total_score;

                char k[512];
                seq_key(&seq, k, sizeof(k));
                if (ti == 0 && r == 0) {
                    snprintf(baseline, sizeof(baseline), "%s", k);
                    base_n = seq.count;
                } else if (baseline[0] && strcmp(k, baseline) == 0) {
                    same_as_base++;
                }
                int dup = 0;
                for (int j = 0; j < nk; j++) if (strcmp(keys[j], k) == 0) { dup = 1; break; }
                if (!dup && nk < MAX_REPS) snprintf(keys[nk++], 512, "%s", k);
            }

            fprintf(out, "%-16s %6.2f %8d %8.1f %9.3f %9s\n",
                   inputs[ii], (double)temps[ti], nk,
                   valid ? (double)cnt_sum / valid : 0.0,
                   valid ? score_sum / valid : 0.0,
                   ti == 0 ? "基准" : (valid ? "" : "无产出"));
            if (ti > 0 && valid > 0) {
                /* 补一列「与基准同」的比例，单独打一行更清楚 */
                fprintf(out, "%-16s %6s %8s %8s %9s %6d/%-2d\n", "", "", "", "", "", same_as_base, valid);
            }
        }
        (void)base_n;
        fprintf(out, "\n");
    }

    fprintf(out, "[探针] 读法：\n");
    fprintf(out, "  · temp=0 那几行的 distinct 必须 = 1（否则扰动没关掉/状态不确定）\n");
    fprintf(out, "  · distinct 随温度单调上升 ⇒ 温度确实是「搜索方差」的旋钮\n");
    fprintf(out, "  · 「与基准同」随温度下降 ⇒ 温度越高越偏离无扰动的最优排序\n");
    fprintf(out, "  · 平均总分随温度下降 ⇒ 扰动是有代价的（破坏原始边权排序）\n");

    fflush(out);
    fclose(out);
    fprintf(stderr, "[probe] result -> %s", outp);
    master_topology_destroy(master);
    return 0;
}
