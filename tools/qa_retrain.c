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
#include "dict_loader.h"
#include "charpair.h"
#include "corpus_filter.h"
#include "word_prob.h"
#include "word_ctx.h"
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
 * 落点：word_emerge 建在概念拓扑并 cross-link 回字（架构线「字拓扑保持纯单字」），
 *   但对话生成的候选池是词汇拓扑 ⇒ 只跑 word_emerge 会得到"没有词"的词表。
 *   这里把词建进词汇拓扑，并双向连回组成字。
 *
 * 判据（CX-40 修：熵改成【词整体语料实测】）：
 *   ① 次数 ≥ mc            ② PMI ≥ mp
 *   ③ 词整体 min(左熵,右熵) ≥ me —— 由 word_ctx 回扫语料实测
 *
 * 🔴 **曾经有过第 ④ 条「两侧都出现边界即放行」，实测后撤掉了**（2026-10-27）：
 *   · 放宽的理由是「吵架」——探针在小黄鸡 47 万对上测出它 min 熵 0.90（nats），
 *     看似会被误杀。但全量 907k 对上词整体实测 min 熵 = **3.197 bit**，
 *     它本来就能靠 ③ 过；理由本身不成立。
 *   · 而 ④ 的实际效果是**放过高频切片**：`用太`(min熵 0.432)、`的原`(0.687)
 *     两侧都偶有边界（高频对必然如此），全靠 ④ 混进词表，且它们
 *     wp_is_wordlike = 0（两条判据当场打架）。
 *   ⇒ 收紧成只看 ③。「能否独立出现」（lbound/rbound）保留为**诊断量**，不做放行条件。
 *
 * 门槛可用环境变量覆盖（与 word_emerge 同名，便于统一调）：
 *   PIVOTMIND_EMERGE_MINCOUNT(20) / MINPMI(2.5) / MINENT(2.0) / MAX(60000)
 */
typedef struct { char a[18], b[18]; unsigned int c; } PairRec;   /* 与 word_prob 同口径，省内存 */
static PairRec* g_pairs = NULL;
static int g_npairs = 0;

/* 候选（过了 ①② 的字对 / 或 EM 白名单）—— 由 word_ctx 回扫实测
 * CX-45 第三步：词长从固定 2 放开到 2~3，故用 parts[] 存组成字 */
typedef struct {
    char     word[16];
    char     parts[3][4];
    int      nparts;
    uint32_t count;
    double   pmi;
} Cand;
static Cand* g_cand = NULL;
static int   g_ncand = 0;

static void _collect_pair(const char* a, const char* b, uint32_t c, void* ud) {
    int* np = (int*)ud;
    if (!np || *np >= 1200000) return;
    snprintf(g_pairs[*np].a, sizeof(g_pairs[0].a), "%s", a);
    snprintf(g_pairs[*np].b, sizeof(g_pairs[0].b), "%s", b);
    g_pairs[*np].c = c;
    (*np)++;
}

/* ---------------- EM 词表白名单模式（CX-45 第一步，2026-10-28） ----------------
 *
 * 判据来源：tools/unigram_lm.py 离线跑 Unigram LM + EM（全量 3477.6 万汉字）得到的
 * 词表 + 概率。实测 held-out logP/char 比「PMI≥2.5 + min熵≥2.0」高 5.7~7.4%
 * （同长度上限、同规模对比）。白名单命中的字对直接晋升，不再跑 PMI 与词整体熵。
 *
 * 开关：默认读 data/word_lm_vocab.tsv（相对 PIVOTMIND_HOME）；
 *   PIVOTMIND_EM_VOCAB=<path> 指定别的文件；PIVOTMIND_EM_VOCAB=0 强制回落到旧判据。
 * 文件格式：`词 \t 概率 \t 期望计数 \t 长度`（首行表头可省；也接受一行一个词）。
 *
 * 为什么要留回落：判据换轨必须是可逆的 —— 词表换错会让整条生成链变差，
 * 而旧路径是已验证过的退路。 */
static DictTable* g_em = NULL;
static int        g_em_n = 0;
/* 词表里的词（2~3 字），按原样存一份：第三步的词长放开后，
 * 候选不再只能来自字对表（字对表只有 2 字），只能来自词表本身 */
#define EM_WORDS_MAX 60000
static char g_em_words[EM_WORDS_MAX][16];
static int  g_em_cnt[EM_WORDS_MAX];
static int  g_em_wn = 0;

static const char* em_vocab_path(void) {
    const char* v = getenv("PIVOTMIND_EM_VOCAB");
    if (v && v[0]) {
        if (v[0] == '0' && v[1] == '\0') return NULL;   /* =0 强制关闭 */
        return v;
    }
    return "data/word_lm_vocab.tsv";
}

static int load_em_vocab(void) {
    const char* path = em_vocab_path();
    if (!path) return 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;                       /* 文件不在 ⇒ 静默回落旧判据 */
    g_em = dict_table_create(131072);
    if (!g_em) { fclose(f); return 0; }

    char line[512];
    int first = 1;
    while (fgets(line, sizeof(line), f)) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
        if (len <= 0) continue;
        /* 表头（以 ASCII 开头，例如「词\tP\t...」）跳过 */
        if (first) {
            first = 0;
            if (!(((unsigned char)line[0]) & 0x80)) continue;
        }
        double prob = 0.0, cnt = 0.0;
        char* tab = strchr(line, '\t');
        if (tab) {
            *tab = 0;
            sscanf(tab + 1, "%lf\t%lf", &prob, &cnt);   /* 第2列概率 · 第3列期望计数 */
        }
        (void)prob;
        if (!line[0]) continue;
        if (dict_table_insert(g_em, line, (int)(cnt > 0 ? cnt : 1.0), "") == 0) g_em_n++;
        /* 只收 2~3 汉字的词（9 字节 = 3 汉字）；纯 ASCII 或更长的跳过 */
        size_t wl = strlen(line);
        if ((wl == 6 || wl == 9) && g_em_wn < EM_WORDS_MAX) {
            snprintf(g_em_words[g_em_wn], sizeof(g_em_words[0]), "%s", line);
            g_em_cnt[g_em_wn] = (int)(cnt > 0 ? cnt : 0);
            g_em_wn++;
        }
    }
    fclose(f);
    printf("[重训] EM 词表已装载：%s · %d 条（其中 2~3 字词 %d 个；判据换成白名单）\n",
           path, g_em_n, g_em_wn);
    return g_em_n > 0;
}

/* 白名单模式下的候选：直接取词表里的词（2~3 字）。
 * 计数不在这里判 —— 交给 measure_candidates 用 word_ctx 在语料上实测，
 * 词表里出现了但语料里数不够的，晋升阶段会被 count<mc 挡掉。 */
static int collect_candidates_em(void) {
    if (!g_em || g_em_wn <= 0) return 0;
    if (!g_cand) {
        g_cand = (Cand*)calloc(WCTX_MAX_CANDS, sizeof(Cand));
        if (!g_cand) return 0;
    }
    g_ncand = 0;
    int n2 = 0, n3 = 0;
    for (int i = 0; i < g_em_wn; i++) {
        const char* w = g_em_words[i];
        size_t wl = strlen(w);
        int np = (wl == 6) ? 2 : ((wl == 9) ? 3 : 0);
        if (np == 0) continue;
        if (g_ncand >= WCTX_MAX_CANDS) {
            printf("[重训] !! 白名单候选超上限 %d，丢弃 %d 个\n", WCTX_MAX_CANDS, g_em_wn - i);
            break;
        }
        Cand* cd = &g_cand[g_ncand++];
        snprintf(cd->word, sizeof(cd->word), "%s", w);
        cd->nparts = np;
        for (int k = 0; k < np; k++) {
            snprintf(cd->parts[k], sizeof(cd->parts[0]), "%.3s", w + k * 3);
        }
        cd->count = (uint32_t)g_em_cnt[i];   /* EM 期望计数（2 字词随后由实测覆盖语义） */
        cd->pmi = 0.0;
        if (np == 2) n2++; else n3++;
    }
    printf("[重训] 白名单候选 %d（2 字 %d · 3 字 %d），计数待语料实测\n", g_ncand, n2, n3);
    return g_ncand;
}

static void thresholds(unsigned* mc, double* mp, double* me, int* mx) {
    unsigned c = 20; double p = 2.5, e = 2.0; int x = 60000;   /* 实测候选 31,167 个：20000 的帽子会砍掉一半 */
    const char* v;
    if ((v = getenv("PIVOTMIND_EMERGE_MINCOUNT"))) c = (unsigned)atoi(v);
    if ((v = getenv("PIVOTMIND_EMERGE_MINPMI")))    p = atof(v);
    if ((v = getenv("PIVOTMIND_EMERGE_MINENT")))    e = atof(v);
    if ((v = getenv("PIVOTMIND_EMERGE_MAX")))       x = atoi(v);
    *mc = c; *mp = p; *me = e; *mx = x;
}

/* 高频豁免门槛（CX-42）：次数 ≥ 本值 ⇒ 免 PMI 检查。
 * 默认 0 = 关闭（先 A/B，确认没把切片放进来再决定要不要翻默认）。
 * 为什么需要：PMI 对「两个都极高频的字」天然偏低 —— 实测「你好」25,258 次，
 * P(你)≈1.4e-2 · P(好)≈7e-3 ⇒ PMI ≈ 1.23 bit < 2.5，直接进不了候选集，
 * 于是连「词整体熵」都轮不到测。这类词恰恰是最高频的真词。 */
static unsigned long long pmi_free_threshold(void) {
    const char* v = getenv("PIVOTMIND_EMERGE_PMIFREE");
    if (!v || !v[0]) return 0ULL;
    return strtoull(v, NULL, 10);
}

/* ① ② 扫一遍字对表，凑出候选集。只收「两个中文单字」的对。 */
static int collect_candidates(unsigned mc, double mp) {
    CharPairTable* cp = cpt_global();
    if (!cp) return 0;
    if (!g_pairs) {
        g_pairs = (PairRec*)calloc(1200000, sizeof(PairRec));   /* 实测 91.9 万对：40 万缓冲会抽样掉近 6 成候选 */
        if (!g_pairs) return 0;
    }
    if (!g_cand) {
        g_cand = (Cand*)calloc(WCTX_MAX_CANDS, sizeof(Cand));
        if (!g_cand) return 0;
    }
    g_npairs = 0;
    cpt_foreach(cp, _collect_pair, &g_npairs);
    if (g_npairs <= 0) return 0;

    g_ncand = 0;
    unsigned long long pmi_free = pmi_free_threshold();
    for (int i = 0; i < g_npairs; i++) {
        const char* A = g_pairs[i].a;
        const char* B = g_pairs[i].b;
        if (strlen(A) != 3 || strlen(B) != 3) continue;   /* 只晋升中文单字对 */
        if (g_pairs[i].c < mc) continue;
        double pmi = wp_pmi(A, B);
        /* 高频免 PMI：切片风险由后面的「词整体熵」闸兜住，不靠 PMI 挡 */
        if (pmi < mp && (unsigned long long)g_pairs[i].c < pmi_free) continue;

        if (g_ncand >= WCTX_MAX_CANDS) {
            /* 显式记账，绝不静默截断（v0.6.12 三个上限叠起来砍掉 44% 且不报错） */
            printf("[重训] !! 候选超上限 %d，丢弃 %d 个（需要就调大 WCTX_MAX_CANDS）\n",
                   WCTX_MAX_CANDS, g_npairs - i);
            break;
        }
        Cand* cd = &g_cand[g_ncand++];
        snprintf(cd->word, sizeof(cd->word), "%s%s", A, B);
        cd->nparts = 2;
        snprintf(cd->parts[0], sizeof(cd->parts[0]), "%s", A);
        snprintf(cd->parts[1], sizeof(cd->parts[1]), "%s", B);
        cd->count = g_pairs[i].c;
        cd->pmi = pmi;
    }
    return g_ncand;
}

/* ③ 回扫语料，测出每个候选的【词整体】左右邻分布。
 * 分批复扫：峰值内存与批大小成正比，与语料规模无关。 */
static void measure_candidates(const char* qa_file, long max_pairs, long accepted_total) {
    const int CHUNK = 16384;
    if (g_ncand <= 0) return;

    int nchunk = (g_ncand + CHUNK - 1) / CHUNK;
    printf("[重训] 词级实测：%d 个候选 · 分 %d 批复扫（%ld 行已收）\n", g_ncand, nchunk, accepted_total);
    time_t t0 = time(NULL);

    for (int base = 0; base < g_ncand; base += CHUNK) {
        int n = g_ncand - base;
        if (n > CHUNK) n = CHUNK;

        const char** words = (const char**)malloc((size_t)n * sizeof(char*));
        if (!words) return;
        for (int i = 0; i < n; i++) words[i] = g_cand[base + i].word;
        int acc = wctx_begin(words, n);
        free(words);
        if (acc <= 0) continue;

        FILE* f = fopen(qa_file, "rb");
        if (!f) { fprintf(stderr, "!! 实测复扫打不开 %s\n", qa_file); return; }

        char line[MAX_LINE];
        long accepted = 0;
        while (fgets(line, sizeof(line), f)) {
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
            if (!cf_accept_qa(q, a, &why)) continue;

            wctx_scan_text(q);
            wctx_scan_text(a);
            accepted++;
            if (max_pairs > 0 && accepted >= max_pairs) break;
        }
        fclose(f);
        wctx_finish();
        fprintf(stderr, "[重训] 实测 %d/%d · 收到候选 %d · %lds\n",
                base + n, g_ncand, acc, (long)(time(NULL) - t0));
    }
}

/* ③④ 双闸门晋升（判据见本段头注释）；em_mode=1 时改按白名单，不跑熵 */
static int promote_measured(SubTopology* vocab, unsigned mc, double me, int mx, int em_mode) {
    if (!vocab || !vocab->net || !g_cand) return 0;

    int created = 0, no_stat = 0, low_ent = 0, bad_parts = 0;
    for (int i = 0; i < g_ncand && created < mx; i++) {
        const char* word = g_cand[i].word;
        int np = g_cand[i].nparts;
        if (np < 2 || np > 3) continue;

        WctxStat st;
        int have = wctx_query(word, &st);
        if (!em_mode) {
            if (!have) { no_stat++; continue; }   /* 语料里没再出现 ⇒ 不晋级 */
            if (st.count < mc) continue;

            /* 只用 ③。lbound/rbound 只进诊断，不做放行条件（见本段头注释：④ 已撤） */
            double mn = st.hl < st.hr ? st.hl : st.hr;
            if (mn < me) { low_ent++; continue; }
        } else if (np == 2) {
            /* 白名单模式：EM 已定成员资格，这里只核对「语料里真出现过」+ 计数 */
            if (!have || st.count < mc) { no_stat++; continue; }
        } else {
            /* 🔴 3 字词：word_ctx 的数据模型是硬编码二元组（wctx_finish 用 a+b 还原词面），
             * 测不了三字词 ⇒ 改用 EM 期望计数门控（离线已按 >=200 筛过，阈值加倍保守） */
            if (g_cand[i].count < mc * 10) { no_stat++; continue; }
        }

        if (huarong_net_find_concept(vocab->net, word) >= 0) continue;

        /* 特征 = 组成字特征均值；任一组成字不是节点 ⇒ 跳过（连不上边） */
        ReasoningNode* parts[3] = {NULL, NULL, NULL};
        int ok = 1;
        for (int k = 0; k < np; k++) {
            int idx = huarong_net_find_concept(vocab->net, g_cand[i].parts[k]);
            if (idx < 0 || idx >= vocab->net->node_count) { ok = 0; break; }
            parts[k] = vocab->net->nodes[idx];
            if (!parts[k] || !parts[k]->features) { ok = 0; break; }
        }
        if (!ok) { bad_parts++; continue; }

        float feat[PM_NODE_FEATURE_DIM];
        for (int d = 0; d < PM_NODE_FEATURE_DIM; d++) {
            float s = 0.0f;
            for (int k = 0; k < np; k++) s += parts[k]->features[d];
            feat[d] = s / (float)np;
        }
        ReasoningNode* wn = huarong_net_add_node(vocab->net, word, feat, PM_NODE_FEATURE_DIM);
        if (!wn) continue;
        node_hash_add(vocab->node_hash, wn);
        /* 词 ↔ 组成字（双向），让扩散能从词走回字、也能从字走到词 */
        for (int k = 0; k < np; k++) {
            add_or_strengthen(vocab->net, wn, parts[k], QA_W_FWD, QA_MAX_FWD);
            add_or_strengthen(vocab->net, parts[k], wn, QA_W_REV, QA_MAX_REV);
        }
        created++;
    }
    printf("[重训] 晋升闸门：通过 %d · 熵不足 %d · 语料未复现 %d · 组成字缺失 %d\n",
           created, low_ent, no_stat, bad_parts);
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

    /* 🔴 从零重训：必须先清空字对表。
     * 否则 cpt_global() 会把目标目录里已有的 charpairs.bin 读进来，本轮统计再叠加
     * ⇒ 同一目录连跑两次结果不同（实测 候选 38,562 → 54,884、词 16,993 → 11,586），
     * 而且工具自己一声不报。这条在 22:22 的一次重跑里真实发生过。 */
    cpt_global_reset_empty();

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

    /* 第二阶段：字对 → 词
     * ①② 字对表筛候选（次数 / PMI）→ ③ 回扫语料测【词整体】熵 → ④ 双闸门晋升 */
    unsigned mc; double mp, me; int mx;
    thresholds(&mc, &mp, &me, &mx);
    int em = load_em_vocab();
    int cands;
    if (em) {
        cands = collect_candidates_em();
        printf("[重训] 阶段二判据：EM 词表白名单（%d 词 · 次数≥%u）\n", g_em_n, mc);
        /* 白名单只替代「闸门」，不替代「测量」：word_ctx.bin 是 --prob 诊断与
         * 后续判据（成分独立率）的数据源，仍然要按语料实测出来。 */
        measure_candidates(qa_file, max_pairs, accepted);
    } else {
        cands = collect_candidates(mc, mp);
        printf("[重训] 候选（次数≥%u · PMI≥%.2f · 高频免PMI门槛=%llu）：%d 个\n",
               mc, (double)mp, pmi_free_threshold(), cands);
        measure_candidates(qa_file, max_pairs, accepted);
    }
    int words = promote_measured(vocab, mc, me, mx, em);
    if (em) printf("[重训] 词晋升：%d 个（判据：EM 白名单）\n", words);
    else    printf("[重训] 词晋升：%d 个（判据：词整体 min熵 ≥ %.2f）\n", words, me);

    unsigned long long drop[3] = {0, 0, 0};
    wctx_drop_stats(drop);
    if (drop[0] || drop[1] || drop[2])
        printf("[重训] word_ctx 丢弃：邻接 %llu · 候选超限 %llu · 非2token %llu\n",
               drop[0], drop[1], drop[2]);

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
        printf("[重训] 边权归一化：跳过（PIVOTMIND_EDGE_NORM 未设；阈值未重标前不要开）\n");
    }

    /* 存盘：特征 → 状态 → 字对表 */
    save_features(master, pm_file(PM_FILE_FEATURES));
    /* ⚠ 约定是「返回已存节点数」，不是 0/1 —— 原来写 `!= 0` 会**每次成功都报失败**，
     * 属于本仓最忌讳的「会骗人的提示」。全仓同族调用点都用 `>= 0` / `int saved =`。 */
    if (master_save_state(master, pm_file(PM_FILE_STATE)) < 0)
        fprintf(stderr, "!! 状态存盘失败\n");
    cpt_global_shutdown_save();
    /* 词级统计落盘：运行期 wp_entropy/wp_gain/wp_is_wordlike 的多字词分支读它。
     * 缺这个文件时多字词一律「熵未知」⇒ 不会被乱判成合法词（宁缺毋滥）。 */
    if (wctx_save(pm_file(PM_FILE_WORD_CTX)) == 0)
        printf("[重训] 词级统计：%d 条 → %s\n", wctx_size(), pm_file(PM_FILE_WORD_CTX));
    else
        fprintf(stderr, "!! 词级统计存盘失败：%s\n", pm_file(PM_FILE_WORD_CTX));

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
