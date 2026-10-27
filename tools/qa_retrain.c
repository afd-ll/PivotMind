/**
 * qa_retrain.c — 从 QA 对重训（只喂 QA，不喂书库；入口过滤繁体/文言/无实义）
 *
 * 用法: ./build/bin/qa_retrain <qa_file> [max_pairs]
 *   qa_file 格式：每行 `问|答`（小黄鸡 pipe 格式）
 *   max_pairs 0/省略 = 全部
 *
 * 输出（全部走路径 SSOT，故须设 PIVOTMIND_HOME）：
 *   $PIVOTMIND_HOME/data/pivotmind_state.dat · features.bin · charpairs.bin
 *
 * 为什么要有这个工具：
 *   线上 54k 节点的状态是多年多轮喂料叠出来的，里面有 57% 的垃圾节点
 *   （非法 UTF-8 2039 / 纯符号 2172 / 中英混 4146），来自当时的无判据造词。
 *   逐节点删不可逆、要连边连引用，风险高；**从零重训**天然干净，
 *   且入口这一次就带上过滤（corpus_filter），旧状态原样备份留底。
 *
 * 建边口径与 corpus_train 一致：
 *   前向 +0.8（上限 5.0）· 反向 +0.6（上限 3.0），命中已有边则累加并微涨置信度。
 */

#include "pivotmind_paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "multi_topology.h"
#include "huarong_topology.h"
#include "feature_io.h"
#include "charpair.h"
#include "corpus_filter.h"
#include "word_prob.h"
#include "node_hash.h"
#include "constants.h"

/* 🔴 边权的口径（2026-10-27 修）
 *
 * 原来：每次共现 +0.8（正向）/ +0.6（反向），上限 5.0 / 3.0。
 * 问题：`huarong_net_add_connection` 新建时把 weight clamp 到 [0,1]，
 * 而后续 `+= 0.8` 不 clamp ⇒ 实测线上边权只剩 **0.3 / 0.5 / 1.0 三档**，
 * 完全没有连续强度。下游却全按 [0,1] 的连续语义写阈值：
 *   · 交集分 `ew >= 0.3`      ⇒ 几乎每条边都过关（判据退化成「是否相邻」）
 *   · 相关性闸门 `rel < 0.3`  ⇒ 恒不触发
 *   · 联想回退窗口 `0.3 ≤ w < 0.6` ⇒ 几乎没有边落进去
 * 实测后果：「山中落雪」的候选里 `不错` 拿到 rel=1.000（与某锚点边权满分），
 * 排到第二位，压过真正相关的「散落 / 雪花」。
 *
 * 现在：权重当【共现计数】用（每次 +1，不封顶），全部学完后统一按
 *   w' = log(1+w) / log(1+max_w)
 * 压缩到 [0,1]。这样既保序又连续，下游的 0.3 / 0.6 阈值恢复原本语义。
 */
#define QA_W_FWD 1.0f          /* = 每共现一次 +1 次计数 */
#define QA_W_REV 1.0f
#define QA_MAX_FWD 1.0e9f      /* 不封顶，最后统一归一化 */
#define QA_MAX_REV 1.0e9f
#define MAX_LINE 4096

static long g_created_nodes = 0;
static long g_edges = 0;

/* 把累加出来的【共现计数】压缩成 [0,1] 连续权重：w' = log(1+w)/log(1+max)。
 * 保序、连续、有区分度；下游 0.3 / 0.6 这类阈值从此有原本语义。 */
static void normalize_edges(SubTopology* vocab) {
    if (!vocab || !vocab->net) return;
    HuarongTopologyNet* net = vocab->net;
    float mx = 1.0f;
    long ne = 0;
    for (int i = 0; i < net->node_count; i++) {
        ReasoningNode* n = net->nodes[i];
        if (!n || !n->edges) continue;
        for (int e = 0; e < n->edge_count; e++) {
            float w = n->edges[e].weight;
            if (w > mx) mx = w;
            ne++;
        }
    }
    if (ne == 0) return;
    double den = log(1.0 + (double)mx);
    if (den < 1e-9) return;
    long hi = 0, mid = 0, lo = 0;
    for (int i = 0; i < net->node_count; i++) {
        ReasoningNode* n = net->nodes[i];
        if (!n || !n->edges) continue;
        for (int e = 0; e < n->edge_count; e++) {
            double w = (double)n->edges[e].weight;
            if (w <= 0.0) { n->edges[e].weight = 0.0f; lo++; continue; }
            float nw = (float)(log(1.0 + w) / den);
            if (nw > 1.0f) nw = 1.0f;
            n->edges[e].weight = nw;
            if (nw >= 0.6f) hi++; else if (nw >= 0.3f) mid++; else lo++;
        }
    }
    printf("[重训] 边权归一化: %ld 条边 · max计数=%.0f ⇒ [0,1] 连续"
           "（≥0.6: %ld · 0.3–0.6: %ld · <0.3: %ld）\n", ne, (double)mx, hi, mid, lo);
}

static void add_or_strengthen(HuarongTopologyNet* net, ReasoningNode* from, ReasoningNode* to,
                              float add_weight, float max_weight) {
    if (!from || !to || from == to) return;
    for (int e = 0; e < from->edge_count; e++) {
        if (from->edges[e].target == to) {
            from->edges[e].weight += add_weight;
            if (from->edges[e].weight > max_weight) from->edges[e].weight = max_weight;
            from->edges[e].confidence += 0.02f;
            if (from->edges[e].confidence > 1.0f) from->edges[e].confidence = 1.0f;
            from->edges[e].motivational_bias += 0.01f;
            if (from->edges[e].motivational_bias > 1.0f) from->edges[e].motivational_bias = 1.0f;
            return;
        }
    }
    if (huarong_net_add_connection(net, from->node_id, to->node_id, add_weight) == 0) g_edges++;
}

/* 把一段文本按汉字逐字建节点 + 相邻建边（不跨句） */
static void learn_chars(SubTopology* vocab, const char* text) {
    if (!vocab || !vocab->net || !text) return;
    int prev = -1;
    const char* p = text;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        int len = (c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : ((c & 0xF8) == 0xF0 ? 4 : 0));
        if (len != 3) { p += (len ? len : 1); prev = -1; continue; }
        unsigned int cp = ((unsigned int)(p[0] & 0x0F) << 12)
                        | ((unsigned int)(p[1] & 0x3F) << 6)
                        |  (unsigned int)(p[2] & 0x3F);
        if (cp < 0x4E00 || cp > 0x9FFF) { p += 3; prev = -1; continue; }

        char ch[4] = {0};
        memcpy(ch, p, 3);
        int id = huarong_net_find_concept(vocab->net, ch);
        if (id < 0) {
            float feat[PM_NODE_FEATURE_DIM];
            for (int i = 0; i < PM_NODE_FEATURE_DIM; i++)
                feat[i] = ((float)rand() / RAND_MAX - 0.5f) * 0.1f;
            ReasoningNode* nd = huarong_net_add_node(vocab->net, ch, feat, PM_NODE_FEATURE_DIM);
            if (!nd) { p += 3; prev = -1; continue; }
            node_hash_add(vocab->node_hash, nd);
            id = nd->node_id;
            g_created_nodes++;
        }
        if (prev >= 0) {
            ReasoningNode* a = (prev < vocab->net->node_count) ? vocab->net->nodes[prev] : NULL;
            ReasoningNode* b = (id < vocab->net->node_count) ? vocab->net->nodes[id] : NULL;
            add_or_strengthen(vocab->net, a, b, QA_W_FWD, QA_MAX_FWD);
            add_or_strengthen(vocab->net, b, a, QA_W_REV, QA_MAX_REV);
        }
        prev = id;
        p += 3;
    }
}

/* 第二阶段：字对 → 词节点（进词汇拓扑，生成侧才用得上）
 *
 * 判据与 word_emerge 同一套（PMI + 左右熵 + 次数），区别只是落点在词汇拓扑：
 *   · word_emerge 建在概念拓扑并 cross-link 回字（架构线「字拓扑保持纯单字」），
 *     但对话生成的候选池是词汇拓扑 ⇒ 只跑 word_emerge 会得到"没有词"的词表。
 *   · 这里按同样的统计门槛把词建进词汇拓扑，并双向连回组成字。
 * 门槛可用环境变量覆盖（与 word_emerge 同名，便于统一调）：
 *   PIVOTMIND_EMERGE_MINCOUNT(20) / MINPMI(2.5) / MINENT(2.0) / MAX(20000)
 */
typedef struct { char a[18], b[18]; unsigned int c; } PairRec;   /* 与 word_prob 同口径，省内存 */
static PairRec* g_pairs = NULL;
static int g_npairs = 0;

static void _collect_pair(const char* a, const char* b, uint32_t c, void* ud) {
    int* np = (int*)ud;
    if (!np || *np >= 1200000) return;
    snprintf(g_pairs[*np].a, sizeof(g_pairs[0].a), "%s", a);
    snprintf(g_pairs[*np].b, sizeof(g_pairs[0].b), "%s", b);
    g_pairs[*np].c = c;
    (*np)++;
}

static int promote_words(SubTopology* vocab) {
    CharPairTable* cp = cpt_global();
    if (!cp || !vocab || !vocab->net) return 0;

    unsigned mc = 20; double mp = 2.5, me = 2.0; int mx = 60000;   /* 实测候选 31,167 个：20000 的帽子会砍掉一半 */
    const char* e;
    if ((e = getenv("PIVOTMIND_EMERGE_MINCOUNT"))) mc = (unsigned)atoi(e);
    if ((e = getenv("PIVOTMIND_EMERGE_MINPMI")))    mp = atof(e);
    if ((e = getenv("PIVOTMIND_EMERGE_MINENT")))    me = atof(e);
    if ((e = getenv("PIVOTMIND_EMERGE_MAX")))       mx = atoi(e);

    if (!g_pairs) {
        g_pairs = (PairRec*)calloc(1200000, sizeof(PairRec));   /* 实测 91.9 万对：40 万缓冲会抽样掉近 6 成候选 */
        if (!g_pairs) return 0;
    }
    g_npairs = 0;
    cpt_foreach(cp, _collect_pair, &g_npairs);
    if (g_npairs <= 0) return 0;

    int created = 0;
    for (int i = 0; i < g_npairs && created < mx; i++) {
        const char* A = g_pairs[i].a;
        const char* B = g_pairs[i].b;
        if (strlen(A) != 3 || strlen(B) != 3) continue;   /* 只晋升中文单字对 */
        if (g_pairs[i].c < mc) continue;
        double pmi = wp_pmi(A, B);
        if (pmi < mp) continue;
        double hl = wp_entropy(A, 0), hr = wp_entropy(B, 1);
        if ((hl < hr ? hl : hr) < me) continue;

        char word[16];
        snprintf(word, sizeof(word), "%s%s", A, B);
        if (huarong_net_find_concept(vocab->net, word) >= 0) continue;

        /* 特征 = 两字均值 */
        int ia = huarong_net_find_concept(vocab->net, A);
        int ib = huarong_net_find_concept(vocab->net, B);
        if (ia < 0 || ib < 0) continue;
        ReasoningNode* na = (ia < vocab->net->node_count) ? vocab->net->nodes[ia] : NULL;
        ReasoningNode* nb = (ib < vocab->net->node_count) ? vocab->net->nodes[ib] : NULL;
        float feat[PM_NODE_FEATURE_DIM];
        for (int d = 0; d < PM_NODE_FEATURE_DIM; d++) {
            if (na && na->features && nb && nb->features)
                feat[d] = (na->features[d] + nb->features[d]) * 0.5f;
            else
                feat[d] = 0.0f;
        }
        ReasoningNode* wn = huarong_net_add_node(vocab->net, word, feat, PM_NODE_FEATURE_DIM);
        if (!wn) continue;
        node_hash_add(vocab->node_hash, wn);
        /* 词 ↔ 组成字（双向），让扩散能从词走回字、也能从字走到词 */
        if (na) { add_or_strengthen(vocab->net, wn, na, QA_W_FWD, QA_MAX_FWD);
                  add_or_strengthen(vocab->net, na, wn, QA_W_REV, QA_MAX_REV); }
        if (nb) { add_or_strengthen(vocab->net, wn, nb, QA_W_FWD, QA_MAX_FWD);
                  add_or_strengthen(vocab->net, nb, wn, QA_W_REV, QA_MAX_REV); }
        created++;
    }
    return created;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "用法: qa_retrain <qa_file> [max_pairs]\n");
        return 2;
    }
    const char* qa_file = argv[1];
    long max_pairs = (argc > 2) ? atol(argv[2]) : 0;

    srand((unsigned)time(NULL));

    MasterTopology* master = master_topology_create(16);
    if (!master) { fprintf(stderr, "FAIL: master create\n"); return 1; }
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

    SubTopology* vocab = NULL;
    for (int t = 0; t < master->sub_topo_count; t++)
        if (master->sub_topologies[t] && master->sub_topologies[t]->type == TOPO_VOCABULARY)
            { vocab = master->sub_topologies[t]; break; }
    if (!vocab || !vocab->net) { fprintf(stderr, "FAIL: no vocab topo\n"); return 1; }

    FILE* f = fopen(qa_file, "rb");
    if (!f) { fprintf(stderr, "FAIL: 打不开 %s\n", qa_file); return 1; }

    printf("[重训] 输入 %s · 输出 %s\n", qa_file, pm_file(PM_FILE_STATE));
    printf("[重训] 判据：只吃 `问|答`；繁体/文言/无实义 一律拒收\n");

    char line[MAX_LINE];
    long lines = 0, accepted = 0, rejected = 0;
    time_t t0 = time(NULL);
    while (fgets(line, sizeof(line), f)) {
        lines++;
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
        if (len < 3) continue;
        char* bar = strchr(line, '|');
        if (!bar) continue;
        *bar = 0;
        const char* q = line;
        const char* a = bar + 1;
        if (!q[0] || !a[0]) continue;

        int why = 0;
        if (!cf_accept_qa(q, a, &why)) { rejected++; continue; }

        learn_chars(vocab, q);
        learn_chars(vocab, a);
        cp_observe_tokens(q);
        cp_observe_tokens(a);
        accepted++;

        if ((accepted % 50000) == 0) {
            long secs = (long)(time(NULL) - t0);
            fprintf(stderr, "[重训] 进度 %ld 对 · 节点 %ld · 边 %ld · 拒收 %ld · %lds\n",
                    accepted, (long)vocab->net->node_count, g_edges, rejected, secs);
        }
        if (max_pairs > 0 && accepted >= max_pairs) break;
    }
    fclose(f);

    /* 第二阶段：字对 → 词（判据同 word_emerge：次数/PMI/左右熵） */
    int words = promote_words(vocab);
    printf("[重训] 词晋升：%d 个\n", words);

    /* 边权口径归一化：计数 ⇒ [0,1] 连续强度（见 normalize_edges 头部注释）。
     * 🔴 **默认关**（PIVOTMIND_EDGE_NORM=1 才开），原因：改了量纲，所有按旧量纲
     * 标定的阈值都必须同步重标才会自洽 —— 实测只改边权不改阈值时，
     * 多字词节点的 rel 普遍只有 0.2（词节点与单字锚点之间本来就没有直接边，
     * 只有两跳衰减值），而 final 筛选的 `rel < 0.3` 会把 256 个候选全砍光
     * ⇒ 扩散产出 0 ⇒ 退到联想兜底吐一串高频泛用词（比改之前更差）。
     * ⇒ 打开前必须同时重标：final 的 rel 阈值、交集分 ew 阈值、联想回退窗口。
     * 详见工作台 CX-39。 */
    if (getenv("PIVOTMIND_EDGE_NORM") && getenv("PIVOTMIND_EDGE_NORM")[0] == '1') {
        for (int t = 0; t < master->sub_topo_count; t++)
            if (master->sub_topologies[t]) normalize_edges(master->sub_topologies[t]);
    } else {
        printf("[重训] 边权归一化：跳过（PIVOTMIND_EDGE_NORM 未设；阈值未重标前不要开）
");
    }

    /* 存盘：特征 → 状态 → 字对表 */
    save_features(master, pm_file(PM_FILE_FEATURES));
    if (master_save_state(master, pm_file(PM_FILE_STATE)) != 0)
        fprintf(stderr, "!! 状态存盘失败\n");
    cpt_global_shutdown_save();

    unsigned long long cf[5] = {0};
    cf_stats(cf);
    int tk = 0, pr = 0;
    unsigned long long tot = 0;
    wp_stats(&tk, &pr, &tot);

    long secs = (long)(time(NULL) - t0);
    printf("\n[重训] 完成：读 %ld 行 · 收 %ld 对 · 拒 %ld 对 · 耗时 %lds\n",
           lines, accepted, rejected, secs);
    printf("[重训] 拒收分项：繁体 %llu · 文言 %llu · 无实义 %llu · 过长 %llu\n",
           cf[1], cf[2], cf[3], cf[4]);
    printf("[重训] 词汇节点 %ld（本次新建 %ld）· 边 %ld\n",
           (long)vocab->net->node_count, g_created_nodes, g_edges);
    printf("[重训] 字对：token %d · 对 %d · 观测 %llu\n", tk, pr, tot);
    cf_dump_samples(5);

    master_topology_destroy(master);
    return 0;
}
