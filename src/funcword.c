/**
 * @file funcword.c
 * @brief 虚词分类器 + 职责分离（阶段 0：影子模式，零行为变化）
 *
 * v0.5 架构迭代（08-14 拍板，docs/funcword-classifier-design.md）
 * 目标：让虚词/功能词类从数据中自然涌现，手写表退化为种子/先验。
 * 本文件 = 阶段 0：位置画像累积（喂料+对话双路径）+ 构词轴离线扫描
 *           + 分类器影子模式（只打印分类结果，不改变任何决策）。
 *
 * 设计要点（pro 三轮评审修正版）：
 * - 三类分类器：虚词 = 位置画像(漂浮/锚定) ∧ 无强边 ∧ 不实义构词
 *               高频实词 = 强边 ∨ 加权实义构词参与度高
 *               普通实词 = 其他
 * - 四根轴：位置画像（复用 dist_sig[22..25]）/ 无强边 / 通用度 / 加权实义构词
 * - 分类器输出落 FuncwordSet（名单+滞回+cap），本阶段只影子打印
 * - 学习端职责分离（建边不晋升）属阶段 2，本文件不含
 *
 * ⚠️ 锁纪律（08-14 修正）：
 *  - funcword_master_scan 扫描全程持 master 读锁（与 master_save_state
 *    同模式）——防学习线程写锁下 realloc nodes 数组的悬垂（TOCTOU 血案）。
 *  - 持读锁期间不得调用任何会抢 master 写锁的函数；本文件内部只用
 *    LOG_*（无锁）、FuncwordSet mutex（自持）、huarong_net_find_concept
 *    （net 读锁，master→net 锁序合法），均不抢 master 写锁，安全。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#include "huarong_topology.h"
#include "multi_topology.h"
#include "emergent_pos.h"
#include "error.h"
#include "funcword.h"

/* ==================== FuncwordSet（涌现功能词名单）==================== */

#define FUNCWORD_SET_CAP 256
#define FUNCWORD_CONF_THRESH 3   /* 连续命中 K 次才进集合（滞回防抖动） */
#define FUNCWORD_CONCEPT_MAX 64  /* concept 字符串键缓冲（单字 3 字节，多字也够） */

typedef struct {
    /* v0.5.20 修正：键从 node_id 改为 concept 字符串。
     * node_id 是拓扑内局部 id，跨拓扑（VOCAB/DOMAIN/CONCEPT）会撞车——
     * 阶段 1 落名单必须按 concept 字符串（或 hash）做键。 */
    char   concepts[FUNCWORD_SET_CAP][FUNCWORD_CONCEPT_MAX];
    int    conf[FUNCWORD_SET_CAP];
    int    count;
    pthread_mutex_t lock;        /* 只护集合写，O(1) */
} FuncwordSet;

static FuncwordSet g_funcword_set;
static int g_funcword_initialized = 0;

static void funcword_set_init(void) {
    if (g_funcword_initialized) return;
    memset(&g_funcword_set, 0, sizeof(g_funcword_set));
    pthread_mutex_init(&g_funcword_set.lock, NULL);
    g_funcword_initialized = 1;
}

static int funcword_set_contains(const char* concept) {
    if (!g_funcword_initialized || !concept) return 0;
    for (int i = 0; i < g_funcword_set.count; i++)
        if (strcmp(g_funcword_set.concepts[i], concept) == 0) return 1;
    return 0;
}

/* 集合导出（供阶段1+ 诊断/持久化）——当前影子模式未用，保留接口 */
static int funcword_set_count(void) {
    if (!g_funcword_initialized) return 0;
    return g_funcword_set.count;
}

/* 更新候选 conf：命中 +1，未命中 -1 归零移除 */
static void funcword_set_touch(const char* concept, int hit) {
    if (!concept) return;
    if (!g_funcword_initialized) funcword_set_init();
    pthread_mutex_lock(&g_funcword_set.lock);
    for (int i = 0; i < g_funcword_set.count; i++) {
        if (strcmp(g_funcword_set.concepts[i], concept) == 0) {
            if (hit) { if (g_funcword_set.conf[i] < 100) g_funcword_set.conf[i]++; }
            else {
                g_funcword_set.conf[i]--;
                if (g_funcword_set.conf[i] <= 0) {
                    /* 移除并压缩 */
                    for (int j = i; j < g_funcword_set.count - 1; j++) {
                        memcpy(g_funcword_set.concepts[j],
                               g_funcword_set.concepts[j+1], FUNCWORD_CONCEPT_MAX);
                        g_funcword_set.conf[j] = g_funcword_set.conf[j+1];
                    }
                    g_funcword_set.count--;
                }
            }
            pthread_mutex_unlock(&g_funcword_set.lock);
            return;
        }
    }
    /* 不在集合：新候选 conf 从 1 开始 */
    if (hit && g_funcword_set.count < FUNCWORD_SET_CAP) {
        strncpy(g_funcword_set.concepts[g_funcword_set.count], concept,
                FUNCWORD_CONCEPT_MAX - 1);
        g_funcword_set.concepts[g_funcword_set.count][FUNCWORD_CONCEPT_MAX - 1] = '\0';
        g_funcword_set.conf[g_funcword_set.count] = 1;
        g_funcword_set.count++;
    }
    pthread_mutex_unlock(&g_funcword_set.lock);
}

/* 正式成员判定：conf >= CONF_THRESH 才算（滞回稳定） */
static int funcword_set_is_member(const char* concept) {
    if (!g_funcword_initialized || !concept) return 0;
    for (int i = 0; i < g_funcword_set.count; i++)
        if (strcmp(g_funcword_set.concepts[i], concept) == 0 &&
            g_funcword_set.conf[i] >= FUNCWORD_CONF_THRESH) return 1;
    return 0;
}

/* ==================== 位置画像累积（阶段0：喂料+对话双路径）==================== */

/**
 * 在喂料/对话路径上累积位置画像到 dist_sig[22..23]（句首率/句尾率）。
 * 标签无关（只需 token 位置，不依赖 POS 标注）——打破引导循环。
 * 调用点：train_mode.c 喂料循环 + dialog_generate.c auto_learn_concepts 前置。
 * ⚠️ 必须在 is_stop_word 判断之前调用（虚词的位置信号同样要累积）。
 *
 * v0.5.20 修正（问题 C）：真·率 EMA，废除"只增不减"。
 * 每个 token 出现都更新两个存储槽（dist_sig[22]=句首率, [23]=句尾率）：
 *   命中位置 → +lr*(1-v)（向 1 靠近），未命中位置 → +lr*(0-v)（向 0 衰减）。
 * 句中 token（既非句首也非句尾）也累加 dist_sig_count（样本数），
 * 但 p0/p2 都向 0 衰减。这样 p0/p2 才是真实的"句首/句尾出现率"，
 * 不会再漂移到 1.0 导致 p1=1-p0-p2 变负、位置轴退化。
 * p1（句中率）由 1-p0-p2 派生，不单独存。
 */
void funcword_record_position(ReasoningNode* node, int is_first, int is_last) {
    if (!node) return;
    /* 复用 dist_sig[22..25] 的位置位（v0.5.19 已定义）：
     * [22]=句首率 [23]=句尾率 [24]=动词后 [25]=动词前（后两位本函数不填） */
    float lr = 0.05f;  /* EMA，与 emergent_pos_update_dist_sig 一致 */
    if (is_first) node->dist_sig[22] += lr * (1.0f - node->dist_sig[22]);
    else          node->dist_sig[22] += lr * (0.0f - node->dist_sig[22]);
    if (is_last)  node->dist_sig[23] += lr * (1.0f - node->dist_sig[23]);
    else          node->dist_sig[23] += lr * (0.0f - node->dist_sig[23]);
    node->dist_sig_count++;
}

/* ==================== 构词轴（加权实义构词参与度）==================== */

/**
 * 离线周期扫描：计算每个单字的「加权实义构词参与度」。
 * 只统计「有强边（max_w>0.5）的组合词」——组合词自己是实义词才算。
 * （是→但是/可是 全弱边连词 → 计 0；大→大学/大海 有强边 → 计分）
 * ⚠️ 只扫 VOCAB：组合词（多字词）只在 VOCAB 创建（auto_learn_concepts），
 *    DOMAIN/CONCEPT 没有组合词，构词轴无需跨拓扑。
 */
static void funcword_compound_scan(HuarongTopologyNet* vnet,
                                   float* compound_score_out,  /* [max_nodes] */
                                   int* compound_strong_out) { /* [max_nodes] */
    if (!vnet || !vnet->nodes) return;
    int maxn = vnet->max_nodes;
    for (int i = 0; i < maxn; i++) compound_score_out[i] = 0.0f;
    for (int i = 0; i < maxn; i++) compound_strong_out[i] = 0;

    /* 第一遍：找出「有强边的多字词」（实义组合词） */
    int* is_strong = compound_strong_out;
    for (int i = 0; i < vnet->node_count; i++) {
        ReasoningNode* nd = vnet->nodes[i];
        if (!nd || !nd->concept) continue;
        int len = (int)strlen(nd->concept);
        if (len <= 3) continue;                 /* 只看多字 CJK 词 */
        unsigned char c0 = (unsigned char)nd->concept[0];
        if ((c0 & 0x80) == 0) continue;         /* 非 CJK 跳过 */
        float max_w = 0.0f;
        for (int e = 0; e < nd->edge_count; e++) {
            if (nd->edges && nd->edges[e].weight > max_w)
                max_w = nd->edges[e].weight;
        }
        if (max_w > 0.5f) is_strong[i] = 1;
    }

    /* 第二遍：每个强组合词的组成部分（单字）构词分 +1 */
    for (int i = 0; i < vnet->node_count; i++) {
        ReasoningNode* nd = vnet->nodes[i];
        if (!nd || !nd->concept || !is_strong[i]) continue;
        int len = (int)strlen(nd->concept);
        if (len <= 3) continue;
        /* 每 3 字节一个汉字 */
        for (int b = 0; b + 3 <= len; b += 3) {
            char single[4] = { nd->concept[b], nd->concept[b+1], nd->concept[b+2], 0 };
            int sid = huarong_net_find_concept(vnet, single);
            if (sid >= 0 && sid < maxn)
                compound_score_out[sid] += 1.0f;
        }
    }
}

/* ==================== 跨拓扑聚合（轴②/轴③ 数据源）==================== */

/**
 * 对同一 concept 在 VOCAB+DOMAIN+CONCEPT 三个拓扑各 find_concept 一次，
 * 累加 edge_count、取全局 max_w、mean_w 用合并后总边重算。
 * 根因（08-14 实测）：虚词边主仓在 DOMAIN（是=643/在=516/了=387），
 * VOCAB 里几乎为空（是=0/吗=1）；只扫 VOCAB 会把虚词全读成"低通用度"。
 * 调用前提：已持 master 读锁（master→net 锁序，net 读锁无争用）。
 */
static void funcword_aggregate(HuarongTopologyNet* vnet,
                               HuarongTopologyNet* dnet,
                               HuarongTopologyNet* cnet,
                               const char* concept,
                               FuncwordAgg* out) {
    out->edge_count = 0;
    out->max_w = 0.0f;
    float sum_w = 0.0f;

    HuarongTopologyNet* nets[3] = { vnet, dnet, cnet };
    for (int t = 0; t < 3; t++) {
        HuarongTopologyNet* net = nets[t];
        if (!net) continue;
        int nid = huarong_net_find_concept(net, concept);
        if (nid < 0 || nid >= net->node_count) continue;
        ReasoningNode* nd = net->nodes[nid];
        if (!nd) continue;
        int ec = nd->edge_count;
        out->edge_count += ec;
        for (int e = 0; e < ec; e++) {
            if (!nd->edges) break;
            float w = nd->edges[e].weight;
            sum_w += w;
            if (w > out->max_w) out->max_w = w;
        }
    }
    out->mean_w = (out->edge_count > 0) ? sum_w / out->edge_count : 0.0f;
}

/* ==================== 分类器（阶段0：影子模式）==================== */

static const char* fc_name(FuncClass c) {
    switch (c) {
        case FC_VOID:      return "普通实词";
        case FC_HIGH_FREQ: return "高频实词";
        case FC_FUNCTION:  return "虚词";
        default:           return "?";
    }
}

/* 位置画像判定（漂浮型/锚定型）——复用 dist_sig[22]=句首率 [23]=句尾率 */
static int funcword_pos_is_function(const ReasoningNode* nd) {
    float p0 = nd->dist_sig[22];   /* 句首率 */
    float p2 = nd->dist_sig[23];   /* 句尾率 */
    float p1 = 1.0f - p0 - p2;     /* 句中率（近似） */
    /* 漂浮型：句中居中 + 句首句尾都非零 */
    if (p1 > 0.15f && p0 > 0.15f && p2 > 0.15f) return 1;
    /* 锚定型：极端位置偏置本身就是功能词特征（吗/吧/呢→句尾，这/那→句首） */
    if (p2 > 0.8f || p0 > 0.8f) return 1;
    return 0;
}

/**
 * 分类器主判定（四轴）。
 * 影子模式：只打印，不改变任何决策。
 * 返回分类结果；is_func 输出虚词判定（供后续阶段接入 is_function_word）。
 * agg：跨拓扑聚合统计（NULL 时退化用节点自身边，仅单拓扑/调试场景）。
 */
FuncClass funcword_classify_node(ReasoningNode* nd,
                                 const FuncwordAgg* agg,
                                 const float* compound_score,
                                 int* is_func_out) {
    if (is_func_out) *is_func_out = 0;
    if (!nd || !nd->concept) return FC_VOID;

    int len = (int)strlen(nd->concept);
    unsigned char c0 = (unsigned char)nd->concept[0];

    /* ---- 多字词 ---- */
    if (len > 3) {
        if ((c0 & 0x80) == 0) return FC_VOID;   /* 英文/其他 */
        float max_w = agg ? agg->max_w : 0.0f;
        if (!agg) {
            for (int e = 0; e < nd->edge_count; e++)
                if (nd->edges && nd->edges[e].weight > max_w)
                    max_w = nd->edges[e].weight;
        }
        /* 多字功能词（但是/可是/因为/所以）：无强边 → 虚词 */
        if (max_w <= 0.5f) { if (is_func_out) *is_func_out = 1; return FC_FUNCTION; }
        return FC_HIGH_FREQ;
    }

    /* ---- 单字（3 字节 CJK）---- */
    if (len == 3 && (c0 & 0x80)) {
        /* 轴②/轴③ 用跨拓扑聚合统计（或退化用节点自身边） */
        int ec;
        float max_w, mean_w;
        if (agg) {
            ec = agg->edge_count;
            max_w = agg->max_w;
            mean_w = agg->mean_w;
        } else {
            ec = nd->edge_count;
            max_w = 0.0f;
            float sum_w = 0.0f;
            for (int e = 0; e < ec; e++) {
                if (!nd->edges) break;
                sum_w += nd->edges[e].weight;
                if (nd->edges[e].weight > max_w) max_w = nd->edges[e].weight;
            }
            mean_w = (ec > 0) ? sum_w / ec : 0.0f;
        }

        /* 轴② 无强边：max_w - mean_w < 0.05（全 0.400 弱边=零 Hebbian 强化） */
        int no_strong = (max_w - mean_w < 0.05f);
        /* 轴① 位置画像（需 dist_sig_count>=10 才可信，否则跳过） */
        int pos_func = (nd->dist_sig_count >= 10) ? funcword_pos_is_function(nd) : 0;
        /* 轴④ 构词参与度（需 compound_score 传入；构词轴只扫 VOCAB，按 VOCAB node_id 索引） */
        float comp = (compound_score && nd->node_id >= 0) ?
                     compound_score[nd->node_id] : 0.0f;
        /* 轴③ 通用度 */
        int high_deg = (ec > 100);

        /* 高频实词 = 有强边 ∨ 构词参与度高 */
        if (max_w > 0.5f || comp >= 1.0f) return FC_HIGH_FREQ;

        /* 虚词 = 位置画像(功能) ∧ 无强边 ∧ 不构词 ∧ 高通用度 */
        if (no_strong && !comp && pos_func && high_deg) {
            if (is_func_out) *is_func_out = 1;
            return FC_FUNCTION;
        }
        /* 位置画像未成熟时（dist_sig_count<10），退化为 无强边+不构词+高通用度 */
        if (no_strong && !comp && high_deg) {
            if (is_func_out) *is_func_out = 1;
            return FC_FUNCTION;
        }
        return FC_VOID;
    }

    /* 非 CJK 或非常规长度 */
    return FC_VOID;
}

/* ==================== 影子模式扫描（周期调用，持 master 读锁）==================== */

static int g_scan_count = 0;

/**
 * 周期扫描词汇拓扑（跨拓扑聚合），影子模式打印分类结果。
 * 调用方必须已持 master 读锁（内部只读 + 写集合拿集合自己的 mutex）。
 * 返回值：本次扫描的虚词候选数。
 */
static int funcword_scan_shadow(HuarongTopologyNet* vnet,
                                HuarongTopologyNet* dnet,
                                HuarongTopologyNet* cnet) {
    if (!vnet || !vnet->nodes) return 0;
    int maxn = vnet->max_nodes;
    if (maxn <= 0) return 0;

    /* 构词轴扫描（只扫 VOCAB，锁外读——调用方已持 master 读锁） */
    float* comp_score = (float*)calloc(maxn, sizeof(float));
    int* strong = (int*)calloc(maxn, sizeof(int));
    if (!comp_score || !strong) {
        free(comp_score); free(strong);
        return 0;
    }
    funcword_compound_scan(vnet, comp_score, strong);

    /* 分类 + 影子打印（每 10 次扫描打印一次完整清单） */
    int func_candidates = 0;
    g_scan_count++;
    /* 测试期每次都 verbose（影子模式验证需要看全量输出）；
     * 阶段 1 转正式时改回 %10 限流 */
    int verbose = 1;

    if (verbose)
        LOG_INFO("[分类器] ===== 影子模式扫描 #%d (节点=%d) =====", g_scan_count, vnet->node_count);

    /* 已知虚词对照（_pos_hash 的 PART/PRON/PREP/CONJ 类）——验证用 */
    static const char* known_func[] = {
        "的","了","是","在","和","我","你","他","她","它","这","那","把","被",
        "对","从","向","为","给","到","就","也","都","很","还","要","会","能",
        "吗","吧","呢","啊","哦","嗯","不","一","地","得","着","过","之","所",
        "与","或","但","而","且","上","下","中","里","以","于","由","让","叫",
        NULL
    };
    /* 已知实词对照 */
    static const char* known_content[] = {
        "大","水","小","山","人","学","生","道","国","成","物","时","间",
        "苹果","时间","思想","电脑","手机","天空","工作","学习","火","树",
        NULL
    };

    int known_func_total = 0, known_func_hit = 0;
    int known_content_total = 0, known_content_hit = 0;

    for (int i = 0; i < vnet->node_count; i++) {
        ReasoningNode* nd = vnet->nodes[i];
        if (!nd || !nd->concept) continue;
        int len = (int)strlen(nd->concept);
        if (len != 3) continue;   /* 影子模式先聚焦单字 */
        unsigned char c0 = (unsigned char)nd->concept[0];
        if ((c0 & 0x80) == 0) continue;

        /* 跨拓扑聚合统计（轴②/轴③） */
        FuncwordAgg agg;
        funcword_aggregate(vnet, dnet, cnet, nd->concept, &agg);

        int is_func = 0;
        (void)funcword_classify_node(nd, &agg, comp_score, &is_func);

        /* 影子写集合：候选 conf 累积（不接任何决策，纯影子）——键为 concept 字符串 */
        funcword_set_touch(nd->concept, is_func);
        int is_member = funcword_set_is_member(nd->concept);

        /* 命中统计 */
        for (int k = 0; known_func[k]; k++) {
            if (strcmp(known_func[k], nd->concept) == 0) {
                known_func_total++;
                if (is_func) known_func_hit++;
                break;
            }
        }
        for (int k = 0; known_content[k]; k++) {
            if (strcmp(known_content[k], nd->concept) == 0) {
                known_content_total++;
                if (!is_func) known_content_hit++;
                break;
            }
        }

        if (is_func) func_candidates++;
        if (verbose && is_func) {
            LOG_INFO("[分类器]   %s虚词候选: %s (边%d 最大权%.2f 构词%.0f 句首%.2f 句尾%.2f)",
                     is_member ? "★" : "  ",
                     nd->concept, agg.edge_count, agg.max_w,
                     comp_score[nd->node_id],
                     nd->dist_sig[22], nd->dist_sig[23]);
        }
    }

    if (verbose) {
        LOG_INFO("[分类器] 影子结果: 虚词候选=%d 集合=%d 已知虚词命中=%d/%d 已知实词命中=%d/%d",
                 func_candidates, funcword_set_count(),
                 known_func_hit, known_func_total,
                 known_content_hit, known_content_total);
        /* 关键边界样本特写 */
        const char* edge_samples[] = {"是","了","吗","吧","呢","大","水","人","我","你",NULL};
        for (int k = 0; edge_samples[k]; k++) {
            int nid = huarong_net_find_concept(vnet, edge_samples[k]);
            if (nid < 0 || nid >= vnet->node_count) continue;
            ReasoningNode* nd = vnet->nodes[nid];
            if (!nd) continue;
            FuncwordAgg agg;
            funcword_aggregate(vnet, dnet, cnet, edge_samples[k], &agg);
            int is_func = 0;
            FuncClass fc = funcword_classify_node(nd, &agg, comp_score, &is_func);
            /* 集合成员标记（阶段1 正式接口预演：contains 判定） */
            int in_set = funcword_set_contains(nd->concept);
            LOG_INFO("[分类器]   样本[%s] → %s%s%s (边%d 最大权%.2f 构词%.0f 句首%.2f 句尾%.2f 位置样本%d)",
                     edge_samples[k], fc_name(fc), is_func ? "[虚]" : "",
                     in_set ? "[集合]" : "",
                     agg.edge_count, agg.max_w,
                     comp_score[nd->node_id],
                     nd->dist_sig[22], nd->dist_sig[23], nd->dist_sig_count);
        }
    }

    free(comp_score);
    free(strong);
    return func_candidates;
}

/* ==================== 对外接口（供 gateway 挂周期调用）==================== */

/**
 * 从 MasterTopology 取词汇/领域/概念拓扑并跑影子扫描（跨拓扑聚合）。
 * ⚠️ v0.5.20 锁纪律修正：扫描全程持 master 读锁（与 master_save_state
 *   同模式），废除"读锁取指针→解锁→无锁扫描"——后者会在学习线程
 *   写锁下 realloc net->nodes 时悬垂（use-after-free）。
 * ⚠️ 不能在 master 写锁内调用本函数（会死锁：读锁请求 vs 已持写锁）。
 * ⚠️ 持读锁期间内部不得调用抢 master 写锁的函数（本函数内部安全）。
 */
void funcword_master_scan(MasterTopology* master) {
    if (!master) return;

    pthread_rwlock_rdlock(&master->rwlock);

    SubTopology* vsub = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    SubTopology* dsub = master_get_sub_topology_by_type(master, TOPO_DOMAIN);
    SubTopology* csub = master_get_sub_topology_by_type(master, TOPO_CONCEPT);

    if (!vsub || !vsub->net) {
        pthread_rwlock_unlock(&master->rwlock);
        LOG_INFO("[分类器] ⚠️ master_scan: vocab 拓扑未找到");
        return;
    }

    funcword_scan_shadow(vsub->net,
                         dsub ? dsub->net : NULL,
                         csub ? csub->net : NULL);

    pthread_rwlock_unlock(&master->rwlock);
}
