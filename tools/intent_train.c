/* tools/intent_train.c — 意图概率模型训练 + 自检
 *
 * 流程：读「问|答」语料 → 用现有 recognize_intent() 自举打标 → 统计 P(字|意图)
 *      → 存 data/intent_model.bin → 对几条例句打印概率分布（自检）
 *
 * 用法: ./intent_train <corpus_pipe> [max_lines=0 全部]
 */
#include "pivotmind_paths.h"
#include "intent_router.h"
#include "dialog_system.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* IN( int i) {
    static const char* n[] = { "未知", "查询", "解释", "比较", "定义",
                               "如何", "闲聊", "学习", "测试", "反馈" };
    return (i >= 0 && i < 10) ? n[i] : "?";
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "用法: %s <corpus_pipe> [max_lines]\n", argv[0]); return 2; }
    int maxl = (argc > 2) ? atoi(argv[2]) : 0;

    int used = ir_train(argv[1], maxl);
    if (used < 0) { fprintf(stderr, "训练失败\n"); return 1; }
    if (ir_save(NULL) != 0) { fprintf(stderr, "存盘失败\n"); return 1; }

    int ni = 0, nv = 0, rd = 0;
    float H = 0;
    ir_stats(&ni, &nv, &H, &rd);
    printf("\n[模型] 意图 %d · 词表 %d · ready=%d\n", ni, nv, rd);

    /* 自检：这些句子的意图应当符合直觉 */
    const char* tests[] = {
        "为什么要睡觉", "什么是爱", "怎么学习编程", "你好啊",
        "猫和狗哪个好", "我想学英语", "这个方法对吗", NULL
    };
    printf("\n[自检] 输入 → top2 概率（熵越低越确定）\n");
    for (int i = 0; tests[i]; i++) {
        IntentDist d;
        int n = ir_classify(tests[i], &d);
        if (n <= 0) { printf("  %-14s → 模型不可用\n", tests[i]); continue; }
        printf("  %-14s → ", tests[i]);
        for (int k = 0; k < n; k++)
            printf("%s(%.3f) ", IN(d.top[k]), (double)d.g[k]);
        printf(" 熵=%.2fbit", (double)d.entropy);
        /* 混合后的拓扑配比（词汇/语义/情绪/语法/上下文/领域/语用） */
        float w[16] = {0};
        if (ir_mixed_topology_weights(&d, w, 9))
            printf(" | 配比 词%.2f 义%.2f 情%.2f 法%.2f 境%.2f 域%.2f 用%.2f",
                   (double)w[0], (double)w[1], (double)w[2], (double)w[3],
                   (double)w[4], (double)w[5], (double)w[6]);
        printf("\n");
    }

    float b[10];
    ir_biases(b, 10);
    printf("\n[负载均衡 bias]（照 MoE auxiliary-loss-free，过载减/欠载加）\n  ");
    for (int i = 1; i < 10; i++) printf("%s=%+.4f ", IN(i), (double)b[i]);
    printf("\n");
    return 0;
}
