/* pos_sep.c — 测量 512 维特征对「词类」到底有没有区分度
 *
 * 为什么测这个：pos_cover 显示种子扩到 868 后，纯指纹覆盖率仍是 0.05%（阈值 0.50），
 * 降到 0.10 则覆盖率 65% 但种子自洽率只有 31.6%（十类随机=10%）⇒ 假覆盖。
 * 所以问题在更下面：特征本身是否携带词类信息。
 *
 * 判据：类内余弦分布 vs 类间余弦分布。
 *   Cohen's d = (mean_in - mean_out) / sqrt((sd_in^2 + sd_out^2)/2)
 *   d > 1.5  可区分，值得修质心算法（改用 medoid / k-means）
 *   d < 0.8  不可区分，特征对词类无信息，得换表示
 *
 * 用法: pos_sep <state_file> [每类抽样数, 默认40]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "common.h"
#include "multi_topology.h"
#include "cognitive_controller.h"
#include "emergent_pos.h"

#define MAXS 60

static float feat[POS_COUNT][MAXS][PM_NODE_FEATURE_DIM];
static int   cnt[POS_COUNT];
static const char* nm[POS_COUNT];

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
    if (master_load_state(master, path) < 0) { master_topology_destroy(master); return NULL; }
    return master;
}

static float cos_sim(const float* a, const float* b) {
    double dot = 0, na = 0, nb = 0;
    for (int i = 0; i < PM_NODE_FEATURE_DIM; i++) {
        dot += (double)a[i] * b[i];
        na  += (double)a[i] * a[i];
        nb  += (double)b[i] * b[i];
    }
    if (na <= 1e-12 || nb <= 1e-12) return 0.0f;
    return (float)(dot / (sqrt(na) * sqrt(nb)));
}

/* 累积统计量 */
typedef struct { double n, s1, s2; float mn, mx; } Stat;
static void st_add(Stat* t, float v) {
    if (t->n == 0) { t->mn = t->mx = v; }
    if (v < t->mn) t->mn = v;
    if (v > t->mx) t->mx = v;
    t->n += 1; t->s1 += v; t->s2 += (double)v * v;
}
static double st_mean(const Stat* t) { return t->n ? t->s1 / t->n : 0; }
static double st_sd(const Stat* t) {
    if (t->n < 2) return 0;
    double m = st_mean(t);
    double v = (t->s2 - t->n * m * m) / (t->n - 1);
    return v > 0 ? sqrt(v) : 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "用法: %s <state_file> [每类抽样数]\n", argv[0]); return 2; }
    int want = (argc >= 3) ? atoi(argv[2]) : 40;
    if (want > MAXS) want = MAXS;

    MasterTopology* master = load_master(argv[1]);
    if (!master) { fprintf(stderr, "加载失败\n"); return 1; }

    SubTopology* vocab = NULL;
    for (int t = 0; t < master->sub_topo_count; t++) {
        if (master->sub_topologies[t] && master->sub_topologies[t]->type == TOPO_VOCABULARY) {
            vocab = master->sub_topologies[t]; break;
        }
    }
    if (!vocab || !vocab->net) { printf("vocab 缺失\n"); return 1; }

    EmergentPOS* ep = emergent_pos_create("zh");
    if (!ep) { printf("create 失败\n"); return 1; }
    for (int t = POS_NOUN; t < POS_COUNT; t++) nm[t] = ep->anchors[t].label_cn;

    /* 按种子标签分桶取特征 */
    memset(cnt, 0, sizeof(cnt));
    int nc = vocab->net->node_count;
    for (int i = 0; i < nc; i++) {
        ReasoningNode* nd = vocab->net->nodes[i];
        if (!nd || !nd->concept || !nd->features) continue;
        POSTag st = emergent_pos_seed_tag(ep, nd->concept);
        if (st <= POS_UNKNOWN || st >= POS_COUNT) continue;
        if (cnt[st] >= want) continue;
        /* 跳过全零特征（全零和任何东西余弦都是 0，会把分布拉低） */
        int nz = 0;
        for (int d = 0; d < PM_NODE_FEATURE_DIM; d++) if (nd->features[d] != 0.0f) { nz = 1; break; }
        if (!nz) continue;
        memcpy(feat[st][cnt[st]], nd->features, PM_NODE_FEATURE_DIM * sizeof(float));
        cnt[st]++;
    }

    printf("\n=== 特征对词类的区分度 ===\n");
    printf("抽样上限 %d/类，实取：", want);
    for (int t = POS_NOUN; t < POS_COUNT; t++) printf("%s=%d ", nm[t], cnt[t]);
    printf("\n\n");

    /* 类内 */
    Stat in_s = {0}, out_s = {0};
    printf("类内余弦（同类种子两两）:\n");
    for (int t = POS_NOUN; t < POS_COUNT; t++) {
        Stat s = {0};
        for (int i = 0; i < cnt[t]; i++)
            for (int j = i + 1; j < cnt[t]; j++) {
                float v = cos_sim(feat[t][i], feat[t][j]);
                st_add(&s, v); st_add(&in_s, v);
            }
        printf("  %-10s n=%5.0f  mean=%.3f  sd=%.3f  min=%.3f max=%.3f\n",
               nm[t], s.n, st_mean(&s), st_sd(&s), s.mn, s.mx);
    }

    /* 类间 */
    printf("\n类间余弦（不同类种子两两）:\n");
    for (int a = POS_NOUN; a < POS_COUNT; a++) {
        for (int b = a + 1; b < POS_COUNT; b++) {
            if (!cnt[a] || !cnt[b]) continue;
            Stat s = {0};
            for (int i = 0; i < cnt[a]; i++)
                for (int j = 0; j < cnt[b]; j++) {
                    float v = cos_sim(feat[a][i], feat[b][j]);
                    st_add(&s, v); st_add(&out_s, v);
                }
            printf("  %-6s x %-6s mean=%.3f sd=%.3f\n", nm[a], nm[b], st_mean(&s), st_sd(&s));
        }
    }

    double mi = st_mean(&in_s), mo = st_mean(&out_s);
    double si = st_sd(&in_s),  so = st_sd(&out_s);
    double pooled = sqrt((si * si + so * so) / 2.0);
    double d = pooled > 1e-9 ? (mi - mo) / pooled : 0;

    printf("\n--- 汇总 ---\n");
    printf("类内 : n=%.0f  mean=%.4f  sd=%.4f  min=%.3f max=%.3f\n", in_s.n, mi, si, in_s.mn, in_s.mx);
    printf("类间 : n=%.0f  mean=%.4f  sd=%.4f  min=%.3f max=%.3f\n", out_s.n, mo, so, out_s.mn, out_s.mx);
    printf("Cohen's d = %.3f   ", d);
    if (d >= 1.5)      printf("⇒ 可区分，问题在质心算法（改 medoid / k-means 有救）\n");
    else if (d >= 0.8) printf("⇒ 弱可分，勉强能用，需要更强特征或半监督传播\n");
    else               printf("⇒ 不可区分，512 维特征对词类无信息，必须换表示\n");
    /* 重叠率：类内有多少落在类间分布的区间里 */
    printf("类内最小值 %.3f vs 类间均值 %.3f（若 类内min < 类间mean 说明大面积重叠）\n", in_s.mn, mo);

    emergent_pos_destroy(ep);
    master_topology_destroy(master);
    return 0;
}
