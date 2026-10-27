/* tools/wemb_probe.c — 邻词分布指纹质量验证
 *
 * 判据：同一词类内部的指纹相似度应明显高于跨类。
 * 若成立 ⇒ 聚类有语义基础（现在用的是 512 维随机噪声，等于随机分类）。
 *
 * 用法: ./wemb_probe <state_file>     （只作占位，指纹不依赖 state）
 */
#include "pivotmind_paths.h"
#include "word_embed.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { const char* name; const char* words[8]; } Group;

static Group G[] = {
    { "副词",  {"很", "不", "都", "非常", "已经", NULL} },
    { "代词",  {"我", "你", "他", "这", "什么", NULL} },
    { "助词",  {"的", "了", "着", "吗", "吧", NULL} },
    { "名词",  {"人", "山", "水", "天", "地", NULL} },
    { "动词",  {"吃", "看", "跑", "想", "说", NULL} },
    { "形容词",{"大", "好", "美", "快", "新", NULL} },
    { "介词",  {"在", "从", "到", "对", "用", NULL} },
    { "数词",  {"一", "十", "百", "个", "次", NULL} },
};

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    clock_t t0 = clock();
    int n = wemb_ensure();
    double sec = (double)(clock() - t0) / CLOCKS_PER_SEC;
    int nw = 0, nd = 0, rd = 0;
    wemb_stats(&nw, &nd, &rd);
    printf("[指纹] ensure=%d 词=%d 维=%d ready=%d 耗时=%.2fs\n", n, nw, nd, rd, sec);
    if (!n) { printf("指纹不可用\n"); return 1; }

    const char* fw[24];
    int nf = wemb_feature_words(fw, 24);
    printf("[特征词前%d] ", nf);
    for (int i = 0; i < nf; i++) printf("%s", fw[i]);
    printf("\n\n");

    int ng = (int)(sizeof(G) / sizeof(G[0]));

    printf("=== 类内平均相似度 ===\n");
    double intra[16] = {0};
    for (int g = 0; g < ng; g++) {
        double s = 0; int c = 0;
        for (int i = 0; G[g].words[i]; i++)
            for (int j = i + 1; G[g].words[j]; j++) {
                float v = wemb_similar(G[g].words[i], G[g].words[j]);
                if (v >= 0) { s += v; c++; }
            }
        intra[g] = c ? s / c : 0;
        printf("  %-8s %.3f（%d 对）\n", G[g].name, intra[g], c);
    }

    printf("\n=== 跨类平均相似度（应明显低于类内）===\n");
    double inter = 0; int ic = 0;
    for (int a = 0; a < ng; a++)
        for (int b = a + 1; b < ng; b++)
            for (int i = 0; G[a].words[i]; i++)
                for (int j = 0; G[b].words[j]; j++) {
                    float v = wemb_similar(G[a].words[i], G[b].words[j]);
                    if (v >= 0) { inter += v; ic++; }
                }
    inter = ic ? inter / ic : 0;
    double sum_in = 0; int gi = 0;
    for (int g = 0; g < ng; g++) if (intra[g] > 0) { sum_in += intra[g]; gi++; }
    double mean_in = gi ? sum_in / gi : 0;
    printf("  跨类平均 %.3f · 类内平均 %.3f · 比值 %.2fx ⇒ %s\n",
           inter, mean_in, inter > 0 ? mean_in / inter : 0.0,
           (mean_in > inter * 1.5) ? "区分度成立" : "区分度不足");

    printf("\n=== 单点抽样（看是否符合直觉）===\n");
    struct { const char* a; const char* b; const char* why; } P[] = {
        {"很", "太", "同为副词"},
        {"很", "山", "副词 vs 名词"},
        {"吃", "看", "同为动词"},
        {"吃", "的", "动词 vs 助词"},
        {"一", "个", "数词 vs 量词"},
        {"人", "山", "同为名词"},
    };
    for (int i = 0; i < (int)(sizeof(P) / sizeof(P[0])); i++) {
        float v = wemb_similar(P[i].a, P[i].b);
        printf("  %s ~ %s = %.3f（%s）\n", P[i].a, P[i].b, (double)v, P[i].why);
    }
    return 0;
}
