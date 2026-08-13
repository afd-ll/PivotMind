/**
 * @file diffusion.c
 * @brief 多层扩散引擎实现
 *
 * 同时激活词汇/语义/模板/情绪四层，各层互相约束，
 * 输出序列由跨模态加权投票+侧抑制产生。
 */

#include "diffusion.h"
#include "emergent_pos.h"
#include "cognitive_controller.h"
#include "node_cache.h"   /* v0.5.10: 按需解冻 node_cache_thaw */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ================================================================
 *  虚词/停用词检查
 * ================================================================ */

static int is_function_word(const char* word) {
    if (!word || !word[0]) return 0;
    /* 快速拒绝：停用词最长 7 字节 ("through"/"because") */
    if (strlen(word) > 7) return 0;
    static const char* stopwords[] = {
        /* 英文虚词 */
        "a", "an", "the",
        "be", "is", "am", "are", "was", "were", "been", "being",
        "have", "has", "had", "having",
        "do", "does", "did", "doing",
        "will", "would", "shall", "should", "may", "might", "must", "can", "could",
        "of", "in", "to", "for", "with", "on", "at", "from", "by", "about", "into",
        "through", "during", "before", "after", "above", "below",
        "up", "down", "out", "off", "over", "under", "again", "then",
        "here", "there", "when", "where", "why", "how",
        "all", "both", "each", "every", "some", "more", "most", "other", "such",
        "no", "nor", "not", "only", "same", "so", "than", "too", "very", "just",
        "that", "this", "what", "which", "who",
        "it", "they", "them", "he", "she", "we", "you",
        "his", "her", "its", "their", "our", "my", "your",
        "and", "but", "or", "if", "while", "because", "as", "until",
        "also", "now", "well", "way", "even", "new", "make", "like",
        "Mr", "Mrs", "Ms", "Dr", "Mr.",
        /* 中文虚词 */
        "\xe4\xb8\x8d",   /* 不 */
        "\xe4\xb8\x80",   /* 一 */
        "\xe6\x9c\x80",   /* 最 */
        "\xe6\x9b\xb4",   /* 更 */
        "\xe5\x9c\xb0",   /* 地 */
        "\xe7\x9a\x84",   /* 的 */
        "\xe4\xba\x86",   /* 了 */
        "\xe5\x9c\xa8",   /* 在 */
        "\xe6\x98\xaf",   /* 是 */
        "\xe6\x88\x91",   /* 我 */
        "\xe4\xbd\xa0",   /* 你 */
        "\xe4\xbb\x96",   /* 他 */
        "\xe5\xa5\xb9",   /* 她 */
        "\xe5\xae\x83",   /* 它 */
        "\xe4\xbb\xac",   /* 们 */
        "\xe8\xbf\x99",   /* 这 */
        "\xe9\x82\xa3",   /* 那 */
        "\xe5\x93\xaa",   /* 哪 */
        "\xe5\x92\x8c",   /* 和 */
        "\xe4\xb8\x8e",   /* 与 */
        "\xe6\x88\x96",   /* 或 */
        "\xe4\xbd\x86",   /* 但 */
        "\xe8\x80\x8c",   /* 而 */
        "\xe4\xb8\x94",   /* 且 */
        "\xe5\xb0\xb1",   /* 就 */
        "\xe4\xb9\x9f",   /* 也 */
        "\xe9\x83\xbd",   /* 都 */
        "\xe5\xbe\x88",   /* 很 */
        "\xe8\xbf\x98",   /* 还 */
        "\xe8\xa6\x81",   /* 要 */
        "\xe4\xbc\x9a",   /* 会 */
        "\xe8\x83\xbd",   /* 能 */
        "\xe5\x8f\xaf",   /* 可 */
        "\xe4\xbb\xa5",   /* 以 */
        "\xe6\x8a\x8a",   /* 把 */
        "\xe8\xa2\xab",   /* 被 */
        "\xe5\xaf\xb9",   /* 对 */
        "\xe4\xbb\x8e",   /* 从 */
        "\xe8\x87\xaa",   /* 自 */
        "\xe5\x88\xb0",   /* 到 */
        "\xe5\x90\x91",   /* 向 */
        "\xe7\x94\xa8",   /* 用 */
        "\xe7\x94\xb1",   /* 由 */
        "\xe4\xb8\xba",   /* 为 */
        "\xe7\xbb\x99",   /* 给 */
        "\xe8\xae\xa9",   /* 让 */
        "\xe5\x8f\xab",   /* 叫 */
        "\xe4\xbd\xbf",   /* 使 */
        "\xe4\xb8\x8a",   /* 上 */
        "\xe4\xb8\xad",   /* 中 */
        "\xe4\xb8\x8b",   /* 下 */
        "\xe7\x9d\x80",   /* 着 */
        "\xe8\xbf\x87",   /* 过 */
        "\xe5\xbe\x97",   /* 得 */
        "\xe4\xb9\x8b",   /* 之 */
        "\xe6\x89\x80",   /* 所 */
        "\xe5\xa6\x82",   /* 如 */
        "\xe8\x8b\xa5",   /* 若 */
        "\xe8\x99\xbd",   /* 虽 */
        "\xe5\x9b\xa0",   /* 因 */
        "\xe6\x95\x85",   /* 故 */
        "\xe5\x90\x97",   /* 吗 */
        "\xe5\x90\xa7",   /* 吧 */
        "\xe5\x91\xa2",   /* 呢 */
        "\xe5\x95\x8a",   /* 啊 */
        "\xe5\x93\xa6",   /* 哦 */
        "\xe5\x97\xaf",   /* 嗯 */
        "\xe4\xb8\xaa",   /* 个 */
        "\xe4\xba\x9b",   /* 些 */
        "\xe7\xa7\x8d",   /* 种 */
        "\xe6\xac\xa1",   /* 次 */
        "\xe5\x9b\x9e",   /* 回 */
        "\xe7\x82\xb9",   /* 点 */
        "\xe9\x87\x8c",   /* 里 */
        "\xe5\xa4\x96",   /* 外 */
        "\xe8\xbe\xb9",   /* 边 */
        /* dialog_generate 停用词合并 */
        "\xe6\x9c\x89",   /* 有 */
        "\xe5\x8f\x8a",   /* 及 */
        "\xe5\x8e\xbb",   /* 去 */
        "\xe6\x9d\xa5",   /* 来 */
        "\xe4\xb8\xba",   /* 为 */
        "\xe6\xaf\x94",   /* 比 */
        "\xe5\x8f\x88",   /* 又 */
        "\xe5\x86\x8d",   /* 再 */
        "\xe6\x89\x8d",   /* 才 */
        "\xe5\x93\x88",   /* 哈 */
        "\xe5\x91\x80",   /* 呀 */
        "\xe5\x98\x9b",   /* 嘛 */
        "\xe5\x95\xa6",   /* 啦 */
        "\xe5\x93\x87",   /* 哇 */
        NULL
    };
    for (const char** p = stopwords; *p; p++)
        if (strcmp(word, *p) == 0) return 1;
    return 0;
}

/* 公共接口 — 供 dialog_generate 等模块复用 */
int diffusion_is_stop_word(const char* word) {
    if (!word || strlen(word) == 0) return 1;
    return is_function_word(word);
}

/* ================================================================
 *  语法组装 — 动词配价驱动，生成主谓宾结构句子
 * ================================================================ */

/** 中文动词配价需求 */
typedef struct {
    const char* verb;       /* 动词 */
    int needs_object:1;     /* 需要宾语 (及物) */
    int needs_double:1;     /* 需要双宾语 */
    int allows_complement:1;/* 可带补语 (得/完/好) */
    int is_copula:1;        /* 是/为(系词): 需要表语 */
    int is_you:1;           /* 有: NP+有+NP */
    int is_descriptive:1;   /* 状态描述: 很+ADJ */
    int is_motion:1;        /* 移动动词: 进/来/去/回 */
    int is_speech:1;        /* 言说动词: 说/问/告诉/叫 */
} VerbValency;

static const VerbValency CHINESE_VERB_VALENCY[] = {
    /* 常用及物动词 */
    {"吃",    1,0,1,0,0,0,0,0},  {"喝",    1,0,1,0,0,0,0,0},
    {"看",    1,0,1,0,0,0,0,0},  {"听",    1,0,1,0,0,0,0,0},
    {"说",    1,0,1,0,0,0,0,1},  {"问",    1,0,1,0,0,0,0,1},
    {"爱",    1,0,0,0,0,0,0,0},  {"想",    1,0,0,0,0,0,0,0},
    {"做",    1,0,1,0,0,0,0,0},  {"写",    1,0,1,0,0,0,0,0},
    {"走",    1,0,1,0,0,0,1,0},  {"跑",    1,0,1,0,0,0,1,0},
    {"知道",  1,0,0,0,0,0,0,0},  {"认识",  1,0,0,0,0,0,0,0},
    {"喜欢",  1,0,1,0,0,0,0,0},  {"需要",  1,0,0,0,0,0,0,0},
    {"得到",  1,0,0,0,0,0,0,0},  {"发现",  1,0,0,0,0,0,0,0},
    {"觉得",  1,0,0,0,0,0,0,0},  {"开始",  1,0,0,0,0,0,0,0},
    {"变成",  1,0,0,0,0,0,0,0},  {"发生",  1,0,0,0,0,0,0,0},
    {"学习",  1,0,1,0,0,0,0,0},  {"工作",  0,0,0,0,0,0,0,0},
    {"生活",  0,0,0,0,0,0,0,0},  {"幸福",  0,0,0,0,0,1,0,0},
    {"快乐",  0,0,0,0,0,1,0,0},  {"美丽",  0,0,0,0,0,1,0,0},
    {"重要",  0,0,0,0,0,1,0,0},  {"伟大",  0,0,0,0,0,1,0,0},
    {"特别",  0,0,0,0,0,1,0,0},

    /* 系词 / "有" 字 */
    {"是",    0,0,0,1,0,0,0,0},  {"有",    0,0,0,0,1,0,0,0},
    {"成为",  0,0,0,1,0,0,0,0},  {"像",    0,0,0,1,0,0,0,0},

    /* 双宾动词 */
    {"给",    0,1,0,0,0,0,0,1},  {"送",    0,1,0,0,0,0,0,0},
    {"告诉",  0,1,0,0,0,0,0,1},  {"教",    0,1,0,0,0,0,0,0},

    /* 补语动词 */
    {"会",    0,0,1,0,0,0,0,0},  {"能",    0,0,1,0,0,0,0,0},
    {"好",    0,0,1,0,0,0,0,0},  {"懂",    0,0,1,0,0,0,0,0},

    /* 常用形容词作谓语 (很X) */
    {"好",    0,0,0,0,0,1,0,0},  {"大",    0,0,0,0,0,1,0,0},
    {"多",    0,0,0,0,0,1,0,0},  {"难",    0,0,0,0,0,1,0,0},
    {"高",    0,0,0,0,0,1,0,0},  {"快",    0,0,0,0,0,1,0,0},
    {"真",    0,0,0,0,0,1,0,0},  {"对",    0,0,0,0,0,1,0,0},
    {0}
};

/** 英文动词配价需求 */
/* 注: 复用 VerbValency 结构体，字段含义稍作调整:
 *   is_copula    →  be 动词 (am/is/are/was/were/been/being)
 *   is_you       →  have 动词 (have/has/had)
 *   is_descriptive → feel/become/seem (系动词 + Adj)
 */
static const VerbValency ENGLISH_VERB_VALENCY[] = {
    /* Be 动词 (copula) */
    {"be",   0,0,0,1,0,0,0,0},  {"am",    0,0,0,1,0,0,0,0},
    {"is",   0,0,0,1,0,0,0,0},  {"are",   0,0,0,1,0,0,0,0},
    {"was",  0,0,0,1,0,0,0,0},  {"were",  0,0,0,1,0,0,0,0},
    {"been", 0,0,0,1,0,0,0,0},  {"being", 0,0,0,1,0,0,0,0},

    /* Have 动词 */
    {"have", 1,0,0,0,1,0,0,0},  {"has",   1,0,0,0,1,0,0,0},
    {"had",  1,0,0,0,1,0,0,0},

    /* 系动词 (形容词表语) */
    {"feel",  0,0,0,0,0,1,0,0}, {"become", 0,0,0,0,0,1,0,0},
    {"seem",  0,0,0,0,0,1,0,0}, {"sound",  0,0,0,0,0,1,0,0},
    {"look",  0,0,0,0,0,1,0,0}, {"appear", 0,0,0,0,0,1,0,0},
    {"get",   0,0,0,0,0,1,0,0}, {"stay",   0,0,0,0,0,1,0,0},
    {"remain",0,0,0,0,0,1,0,0}, {"prove",  0,0,0,0,0,1,0,0},

    /* 及物动词 (needs_object) */
    {"know",  1,0,0,0,0,0,0,0}, {"think",  1,0,0,0,0,0,0,0},
    {"want",  1,0,0,0,0,0,0,0}, {"love",   1,0,0,0,0,0,0,0},
    {"like",  1,0,0,0,0,0,0,0}, {"need",   1,0,0,0,0,0,0,0},
    {"see",   1,0,0,0,0,0,0,0}, {"show",   1,0,0,0,0,0,0,0},
    {"tell",  1,0,0,0,0,0,0,0}, {"give",   1,0,0,0,0,0,0,0},
    {"take",  1,0,0,0,0,0,0,0}, {"make",   1,0,0,0,0,0,0,0},
    {"find",  1,0,0,0,0,0,0,0}, {"believe",1,0,0,0,0,0,0,0},
    {"hear",  1,0,0,0,0,0,0,0}, {"meet",   1,0,0,0,0,0,0,0},
    {"bring", 1,0,0,0,0,0,0,0}, {"put",    1,0,0,0,0,0,0,0},
    {"set",   1,0,0,0,0,0,0,0}, {"keep",   1,0,0,0,0,0,0,0},
    {"hold",  1,0,0,0,0,0,0,0}, {"call",   1,0,0,0,0,0,0,0},
    {"help",  1,0,0,0,0,0,0,0}, {"leave",  1,0,0,0,0,0,0,0},
    {"send",  1,0,0,0,0,0,0,0}, {"spend",  1,0,0,0,0,0,0,0},
    {"use",   1,0,0,0,0,0,0,0}, {"hope",   1,0,0,0,0,0,0,0},
    {"mean",  1,0,0,0,0,0,0,0}, {"expect", 1,0,0,0,0,0,0,0},
    {"continue",1,0,0,0,0,0,0,0}, {"consider",1,0,0,0,0,0,0,0},
    {"provide",1,0,0,0,0,0,0,0}, {"receive", 1,0,0,0,0,0,0,0},
    {"imagine",1,0,0,0,0,0,0,0}, {"realize", 1,0,0,0,0,0,0,0},
    {"remember",1,0,0,0,0,0,0,0}, {"suppose", 1,0,0,0,0,0,0,0},

    /* 不及物 / 可带宾语 + 补语 */
    {"talk",  1,0,1,0,0,0,0,0}, {"speak",  1,0,1,0,0,0,0,0},
    {"walk",  1,0,1,0,0,0,0,0}, {"run",    0,0,1,0,0,0,0,0},
    {"come",  0,0,0,0,0,0,0,0}, {"go",     0,0,1,0,0,0,0,0},
    {"live",  0,0,0,0,0,0,0,0}, {"work",   0,0,0,0,0,0,0,0},
    {"begin", 1,0,0,0,0,0,0,0}, {"start",  1,0,0,0,0,0,0,0},
    {"try",   1,0,0,0,0,0,0,0}, {"continue",1,0,0,0,0,0,0,0},
    {"seem",  0,0,0,0,0,1,0,0}, {"happen", 0,0,0,0,0,0,0,0},
    {"change",1,0,0,0,0,0,0,0}, {"move",   1,0,0,0,0,0,0,0},
    {"turn",  1,0,0,0,0,0,0,0}, {"lead",   1,0,0,0,0,0,0,0},
    {"follow",1,0,0,0,0,0,0,0}, {"serve",  1,0,0,0,0,0,0,0},
    {"grow",  1,0,0,0,0,0,0,0}, {"die",    0,0,0,0,0,0,0,0},
    {"agree", 1,0,0,0,0,0,0,0},

    /* 情态 / 助动词 */
    {"can",   0,0,1,0,0,0,0,0}, {"could",  0,0,1,0,0,0,0,0},
    {"will",  0,0,1,0,0,0,0,0}, {"would",  0,0,1,0,0,0,0,0},
    {"may",   0,0,1,0,0,0,0,0}, {"might",  0,0,1,0,0,0,0,0},
    {"must",  0,0,1,0,0,0,0,0}, {"should", 0,0,1,0,0,0,0,0},
    {"shall", 0,0,1,0,0,0,0,0}, {"do",     0,0,1,0,0,0,0,0},
    {"does",  0,0,1,0,0,0,0,0}, {"did",    0,0,1,0,0,0,0,0},
    {0}
};

/** 从候选词池中取一个指定 POS 且未使用的词，返回下标，-1 表示没有 */
static int pool_take(const char** word_buf, POSTag* word_pos, int word_count,
                     int* used, POSTag want, int prefer_wrd[DIFF_MAX_SEQUENCE],
                     int prefer_cnt) {
    /* 优先从 prefer 列表中选 */
    for (int p = 0; p < prefer_cnt; p++) {
        int w = prefer_wrd[p];
        if (w < 0 || w >= word_count) continue;
        if (used[w] || !word_buf[w]) continue;
        if (word_pos[w] == want) { used[w] = 1; return w; }
    }
    /* 兜底: 遍历所有 */
    for (int w = 0; w < word_count; w++) {
        if (used[w] || !word_buf[w]) continue;
        if (word_pos[w] == want) { used[w] = 1; return w; }
    }
    return -1;
}

/**
 * 动词配价驱动的语法组装
 * 流程:
 *   1. 从候选词找最佳动词/形容词谓语
 *   2. 分配主语（NP）
 *   3. 分配宾语/补语
 *   4. 添加句末助词(了/呢/啊)、标点
 *   5. 如果无合适谓语，回退到旧骨架模式
 */
/* ================================================================
 *  动词配价自动推断 — 从拓扑边统计替代硬编码配价表
 *
 *  当动词不在硬编码配价表中时，分析其词汇拓扑节点的出边
 *  来推断配价属性。
 * ================================================================ */
static VerbValency diffusion_infer_valency(const char* verb,
                                           DiffusionCtx* ctx) {
    VerbValency vv = {"", 0,0,0,0,0,0,0,0};
    if (!verb || !ctx || !ctx->vocab || !ctx->vocab->net) return vv;

    int node_id = huarong_net_find_concept(ctx->vocab->net, verb);
    if (node_id < 0 || node_id >= ctx->vocab->net->node_count) return vv;

    ReasoningNode* node = ctx->vocab->net->nodes[node_id];
    if (!node || node->edge_count == 0) return vv;

    /* 统计出边目标节点的涌现词类分布 */
    int noun_edges = 0, adj_edges = 0, adv_edges = 0, total = 0;
    for (int e = 0; e < node->edge_count && e < 32; e++) {
        ReasoningNode* tgt = node->edges[e].target;
        if (!tgt) continue;
        if (tgt->emergent_class_count == 0) continue;
        total++;
        int cls = tgt->emergent_class_ids[0];
        if      (cls == (int)POS_NOUN) noun_edges++;
        else if (cls == (int)POS_ADJ)  adj_edges++;
        else if (cls == (int)POS_ADV)  adv_edges++;
    }

    /* Heuristic 推断 */
    if (total >= 2) {
        float noun_ratio = (float)noun_edges / (float)total;
        float adj_ratio  = (float)adj_edges  / (float)total;

        if (noun_ratio > 0.35f) vv.needs_object = 1;  /* 及物 */
        if (adj_ratio  > 0.35f) vv.is_descriptive = 1; /* 描述性 */
        if (adj_ratio > 0.15f)  vv.allows_complement = 1; /* 可带补语 */
    }

    /* 特殊词硬识别 (跨语言通用) */
    if (strcmp(verb, "是") == 0 || strcasecmp(verb, "be") == 0 ||
        strcasecmp(verb, "is") == 0 || strcasecmp(verb, "am") == 0 ||
        strcasecmp(verb, "are") == 0) {
        vv.is_copula = 1; vv.needs_object = 0; vv.is_descriptive = 0;
    }
    if (strcmp(verb, "有") == 0 || strcasecmp(verb, "have") == 0 ||
        strcasecmp(verb, "has") == 0 || strcasecmp(verb, "had") == 0) {
        vv.is_you = 1; vv.needs_object = 0;
    }

    return vv;
}

static int diffusion_assemble_grammar(
    const char** word_buf, POSTag* word_pos, int word_count,
    const char** output_words, int max_output,
    int is_english, DiffusionCtx* ctx) {
    int out = 0;
    if (word_count < 2 || !word_buf || !output_words) return 0;

    int used[DIFF_MAX_SEQUENCE] = {0};

    /* ── Step 1: 找谓语核心 ── */
    /* v0.5.16: 删除硬编码配价表（TWN 红线清洗——生成骨架由模板拓扑负责，
     * 动词论元需求全部从语料推断 diffusion_infer_valency，不再手写预置） */
    int verb_idx = -1;
    if (verb_idx < 0) {
        for (int w = 0; w < word_count; w++) {
            if (!word_buf[w] || word_pos[w] != POS_VERB) continue;
            verb_idx = w;
            break;
        }
    }

    /* ── 无动词 → 尝试形容词谓语 ── */
    if (verb_idx < 0) {
        for (int w = 0; w < word_count; w++) {
            if (!word_buf[w]) continue;
            if (word_pos[w] != POS_ADJ) continue;
            /* v0.5.16: 不再查配价表——ADJ 直接作谓语（模板/推断兜底） */
            verb_idx = w;
            if (verb_idx >= 0) break;
        }
    }

    /* ── 还是无谓语 → 用任意 ADJ ── */
    if (verb_idx < 0) {
        for (int w = 0; w < word_count; w++) {
            if (!word_buf[w] || word_pos[w] != POS_ADJ) continue;
            verb_idx = w;
            break;
        }
    }

    if (verb_idx < 0) return 0;

    /* v0.5.16: 配价全部从语料推断（删除硬编码表后唯一来源） */
    const VerbValency* vv = NULL;
    static VerbValency inferred_vv;  /* 静态避免栈地址失效 */
    if (ctx && verb_idx >= 0 && word_buf[verb_idx]) {
        inferred_vv = diffusion_infer_valency(word_buf[verb_idx], ctx);
        if (inferred_vv.needs_object || inferred_vv.is_copula ||
            inferred_vv.is_you || inferred_vv.is_descriptive)
            vv = &inferred_vv;
    }

    used[verb_idx] = 1;  /* 标记谓语已用 */

    /* 构建 preference 列表: 动词前最近的词、动词后最近的词 */
    int pre_verb[DIFF_MAX_SEQUENCE], pre_cnt = 0;
    int post_verb[DIFF_MAX_SEQUENCE], post_cnt = 0;
    for (int w = 0; w < word_count; w++) {
        if (w == verb_idx || !word_buf[w]) continue;
        if (w < verb_idx && pre_cnt < DIFF_MAX_SEQUENCE) pre_verb[pre_cnt++] = w;
        if (w > verb_idx && post_cnt < DIFF_MAX_SEQUENCE) post_verb[post_cnt++] = w;
    }

    /* ── Step 2: 主语 (ADV + NOUN/PRON) ── */
    if (is_english) {
        /* 英文: 主语 = PRON > NOUN，前缀可加 ADV，排除宾格/反身代词 */
        int subj = -1;
        /* 主格代词白名单 */
        static const char* subj_pron[] = {
            "i","you","he","she","we","they","it",
            "this","that","these","those","who","what","which",
            "someone","anyone","everyone","nobody","somebody","anybody",
            "everybody","everything","nothing","something","anything",
            NULL
        };
        /* 优先从 pre_verb 中找合适代词 */
        for (int p = 0; p < pre_cnt; p++) {
            int w = pre_verb[p];
            if (used[w] || !word_buf[w] || word_pos[w] != POS_PRON) continue;
            for (int s = 0; subj_pron[s]; s++)
                if (strcasecmp(word_buf[w], subj_pron[s]) == 0)
                    { subj = w; used[w] = 1; break; }
            if (subj >= 0) break;
        }
        /* 兜底: 从所有候选词中找合适代词 */
        if (subj < 0) {
            for (int w = 0; w < word_count; w++) {
                if (used[w] || !word_buf[w] || word_pos[w] != POS_PRON) continue;
                for (int s = 0; subj_pron[s]; s++)
                    if (strcasecmp(word_buf[w], subj_pron[s]) == 0)
                        { subj = w; used[w] = 1; break; }
                if (subj >= 0) break;
            }
        }
        /* 如果没有代词，用 NOUN */
        if (subj < 0) {
            subj = pool_take(word_buf, word_pos, word_count, used,
                             POS_NOUN, pre_verb, pre_cnt);
        }
        if (subj >= 0) {
            int adv = pool_take(word_buf, word_pos, word_count, used,
                                POS_ADV, pre_verb, pre_cnt);
            if (adv >= 0 && out < max_output - 1) {
                output_words[out++] = word_buf[adv];
                if (out < max_output - 1) output_words[out++] = " ";
            }
            output_words[out++] = word_buf[subj];
            if (out < max_output - 1) output_words[out++] = " ";
        }
    } else {
        /* 中文: 主语 = PRON > NOUN，前缀可加 ADV */
        int subj = pool_take(word_buf, word_pos, word_count, used,
                             POS_PRON, pre_verb, pre_cnt);
        if (subj < 0)
            subj = pool_take(word_buf, word_pos, word_count, used,
                             POS_NOUN, pre_verb, pre_cnt);
        if (subj >= 0) {
            /* 状语前置: 如果主语前有 ADJ 或 ADV，加在主语前面 */
            int adv = pool_take(word_buf, word_pos, word_count, used,
                                POS_ADV, pre_verb, pre_cnt);
            if (adv >= 0) output_words[out++] = word_buf[adv];
            output_words[out++] = word_buf[subj];
        }
    }

    /* ── Step 3: 谓语动词 / 形容词 ── */
    if (is_english && vv && vv->is_copula && out < max_output - 1) {
        /* 英文系词: "is" 不特殊处理，直接输出 */
        output_words[out++] = word_buf[verb_idx];
    } else if (is_english && vv && vv->is_you && out < max_output - 1) {
        /* have/has/had */
        output_words[out++] = word_buf[verb_idx];
    } else if (is_english && vv && vv->is_descriptive && out < max_output - 2) {
        /* 系动词+形容词: feel good, become happy */
        output_words[out++] = word_buf[verb_idx];
    } else if (is_english && vv && vv->allows_complement && out < max_output - 1) {
        /* 情态动词: can/must/should — 后面直接跟动词 */
        output_words[out++] = word_buf[verb_idx];
        /* 找另一个动词跟在后面 */
        int main_v = pool_take(word_buf, word_pos, word_count, used,
                                POS_VERB, post_verb, post_cnt);
        if (main_v >= 0 && out < max_output - 1) {
            if (out < max_output - 1) output_words[out++] = " ";
            output_words[out++] = word_buf[main_v];
        }
    } else if (!is_english && vv && vv->is_you && out < max_output - 1) {
        output_words[out++] = word_buf[verb_idx];
    } else if (!is_english && vv && vv->is_copula && out < max_output - 1) {
        output_words[out++] = word_buf[verb_idx];
    } else if (!is_english && vv && vv->is_descriptive && out < max_output - 2) {
        output_words[out++] = "\xe5\xbe\x88";  /* "很" */
        output_words[out++] = word_buf[verb_idx];
    } else {
        if (out < max_output - 1) output_words[out++] = word_buf[verb_idx];
    }

    /* ── Step 4: 宾语 / 补语 / 表语 ── */
    if (is_english) {
        /* ═══ 英文: 宾语 / 表语 / 补语 ═══ */
        if (vv && vv->is_copula) {
            /* Be 动词: 表语 = NOUN > ADJ */
            if (out < max_output - 1) output_words[out++] = " ";
            int obj = pool_take(word_buf, word_pos, word_count, used,
                                POS_NOUN, post_verb, post_cnt);
            if (obj < 0)
                obj = pool_take(word_buf, word_pos, word_count, used,
                                POS_ADJ, post_verb, post_cnt);
            if (obj >= 0 && out < max_output - 1) {
                /* 可加冠词 */
                const char* article = (word_buf[obj][0] == 'a' ||
                    word_buf[obj][0] == 'e' || word_buf[obj][0] == 'i' ||
                    word_buf[obj][0] == 'o' || word_buf[obj][0] == 'u') ? "an" : "a";
                /* 简化: 单字母代词不加冠词 */
                int need_article = (strlen(word_buf[obj]) > 1 &&
                    word_pos[obj] == POS_NOUN);
                if (need_article && out < max_output - 1)
                    output_words[out++] = article;
                output_words[out++] = word_buf[obj];
            }
        } else if (vv && vv->is_descriptive) {
            /* feel/become + Adj */
            if (out < max_output - 1) output_words[out++] = " ";
            int obj = pool_take(word_buf, word_pos, word_count, used,
                                POS_ADJ, post_verb, post_cnt);
            if (obj >= 0 && out < max_output - 1)
                output_words[out++] = word_buf[obj];
        } else if (vv && vv->needs_object) {
            /* 及物动词: 宾语, 前面可加 Adj */
            if (out < max_output - 1) output_words[out++] = " ";
            int adj_obj = pool_take(word_buf, word_pos, word_count, used,
                                     POS_ADJ, post_verb, post_cnt);
            if (adj_obj >= 0 && out < max_output - 1) {
                output_words[out++] = word_buf[adj_obj];
                if (out < max_output - 1) output_words[out++] = " ";
            }
            int obj = pool_take(word_buf, word_pos, word_count, used,
                                POS_NOUN, post_verb, post_cnt);
            if (obj < 0)
                obj = pool_take(word_buf, word_pos, word_count, used,
                                POS_PRON, post_verb, post_cnt);
            if (obj < 0)
                obj = pool_take(word_buf, word_pos, word_count, used,
                                POS_ADJ, post_verb, post_cnt);
            if (obj >= 0 && out < max_output - 1)
                output_words[out++] = word_buf[obj];
        } else if (vv && vv->is_you) {
            /* have: 宾语 */
            if (out < max_output - 1) output_words[out++] = " ";
            int obj = pool_take(word_buf, word_pos, word_count, used,
                                POS_NOUN, post_verb, post_cnt);
            if (obj < 0)
                obj = pool_take(word_buf, word_pos, word_count, used,
                                POS_ADJ, post_verb, post_cnt);
            if (obj >= 0 && out < max_output - 1)
                output_words[out++] = word_buf[obj];
        } else if (vv && vv->allows_complement) {
            /* 情态动词: 已经插入主动词在 Step 3 */
        } else {
            /* 无配价: 加宾语 */
            if (out < max_output - 1) output_words[out++] = " ";
            int obj = pool_take(word_buf, word_pos, word_count, used,
                                POS_NOUN, post_verb, post_cnt);
            if (obj >= 0 && out < max_output - 1)
                output_words[out++] = word_buf[obj];
        }
    } else {
        /* 中文: vv 可能为 NULL（硬编码未命中且拓扑边也推断不出来） */
        if (!vv || vv->is_copula || vv->is_you) {
            /* 系词/有字句 或 无配价: 宾语 = NOUN/ADJ */
            int obj = pool_take(word_buf, word_pos, word_count, used,
                                POS_NOUN, post_verb, post_cnt);
            if (obj < 0)
                obj = pool_take(word_buf, word_pos, word_count, used,
                                POS_ADJ, post_verb, post_cnt);
            if (obj >= 0 && out < max_output - 1) {
                if (is_english && out < max_output - 1) output_words[out++] = " ";
                output_words[out++] = word_buf[obj];
            }
        } else if (vv->needs_object) {
            /* 及物动词: 宾语 = NOUN，前面加 ADJ 作定语 */
            int adj_obj = pool_take(word_buf, word_pos, word_count, used,
                                     POS_ADJ, post_verb, post_cnt);
            int obj = pool_take(word_buf, word_pos, word_count, used,
                                POS_NOUN, post_verb, post_cnt);
            if (obj >= 0 && out < max_output - 1) {
                if (!is_english && adj_obj >= 0 && out < max_output - 1) {
                    output_words[out++] = word_buf[adj_obj];
                    if (out < max_output - 1)
                        output_words[out++] = "\xe7\x9a\x84";  /* "的" */
                }
                if (is_english && adj_obj >= 0 && out < max_output - 1) {
                    output_words[out++] = word_buf[adj_obj];
                    if (out < max_output - 1) output_words[out++] = " ";
                }
                output_words[out++] = word_buf[obj];
            } else if (adj_obj >= 0 && out < max_output - 1) {
                /* 没找到名词宾语，用形容词作宾语 */
                if (is_english && out < max_output - 1) output_words[out++] = " ";
                output_words[out++] = word_buf[adj_obj];
            }
        } else if (vv->is_descriptive) {
            /* "很X" 已完成，不需要额外宾语 */
        } else if (vv->allows_complement && out < max_output - 2) {
            /* 补语: V+得+ADJ/ADV */
            int compl = pool_take(word_buf, word_pos, word_count, used,
                                   POS_ADJ, post_verb, post_cnt);
            if (compl < 0)
                compl = pool_take(word_buf, word_pos, word_count, used,
                                   POS_ADV, post_verb, post_cnt);
            if (compl >= 0) {
                output_words[out++] = "\xe5\xbe\x97";  /* "得" */
                output_words[out++] = word_buf[compl];
            }
        } else {
            /* 无配价表但确实是动词: 尝试加宾语 */
            int obj = pool_take(word_buf, word_pos, word_count, used,
                                POS_NOUN, post_verb, post_cnt);
            if (obj >= 0 && out < max_output - 1)
                output_words[out++] = word_buf[obj];
        }
    }

    /* ── Step 5: 句末助词 + 标点 ── */
    if (is_english) {
        if (out < max_output - 1) output_words[out++] = ".";
    } else {
        if (vv && vv->needs_object && out < max_output - 2)
            output_words[out++] = "了";  /* 及物动词加"了"表完成 */
        if (out < max_output - 1)
            output_words[out++] = "\xe3\x80\x82";  /* "。" */
    }

    return out;
}

/* ── 保留旧骨架函数作为降级路径 ── */
typedef struct {
    POSTag seq[4];
    int    len;
    float  weight;
} DiffSentencePattern;

static int diffusion_assemble_scaffold_fallback(
    const char** word_buf, POSTag* word_pos, int word_count,
    const char** output_words, int max_output, int is_english) {
    /* 简化版: 选 ADJ+NOUN 骨架 */
    int out = 0;
    int used[DIFF_MAX_SEQUENCE] = {0};

    /* 先取 ADJ */
    int adj_count = 0;
    for (int w = 0; w < word_count && adj_count < 3 && out < max_output - 1; w++) {
        if (used[w] || !word_buf[w]) continue;
        if (word_pos[w] != POS_ADJ) continue;
        if (adj_count > 0 && out < max_output - 1)
            output_words[out++] = is_english ? ", " : "\xe3\x80\x81";  /* "、" */
        output_words[out++] = word_buf[w];
        used[w] = 1;
        adj_count++;
    }
    /* 连接词 */
    if (adj_count > 0 && out < max_output - 1) {
        if (is_english) output_words[out++] = " ";
        else output_words[out++] = "\xe7\x9a\x84";  /* "的" */
    }
    /* 取 NOUN */
    int noun_count = 0;
    for (int w = 0; w < word_count && noun_count < 2 && out < max_output - 1; w++) {
        if (used[w] || !word_buf[w]) continue;
        if (word_pos[w] != POS_NOUN) continue;
        if (noun_count > 0 && out < max_output - 1)
            output_words[out++] = is_english ? ", " : "\xe3\x80\x81";
        output_words[out++] = word_buf[w];
        used[w] = 1;
        noun_count++;
    }
    /* 标点 */
    if (out < max_output - 1)
        output_words[out++] = is_english ? "." : "\xe3\x80\x82";
    return out;
}

/* ================================================================
 *  初始化
 * ================================================================ */

int diffusion_init(DiffusionCtx* ctx, MasterTopology* master) {
    if (!ctx || !master) return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->master = master;
    ctx->depth  = 2;
    ctx->top_k  = 10;
    ctx->output_len = 20;
    ctx->decay  = 0.7f;
    ctx->temperature = 0.03f;
    ctx->emergent_pos = NULL;  /* 调用者可选注入 (cingulate_diffusion_evaluate) */

    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        switch (sub->type) {
            case TOPO_VOCABULARY: ctx->vocab    = sub; break;
            case TOPO_SEMANTIC:   ctx->semantic  = sub; break;
            case TOPO_TEMPLATE:   ctx->template  = sub; break;
            case TOPO_EMOTION:    ctx->emotion   = sub; break;
            case TOPO_CONCEPT:    ctx->concept   = sub; break;
            default: break;
        }
    }

    if (!ctx->vocab) return -1;
    printf("[扩散引擎] 就绪 (词汇=%d, 语义=%s, 模板=%s, 情绪=%s, 深度=%d, topK=%d)\n",
           ctx->vocab->net->node_count,
           ctx->semantic ? "ON" : "OFF",
           ctx->template ? "ON" : "OFF",
           ctx->emotion  ? "ON" : "OFF",
           ctx->depth, ctx->top_k);
    return 0;
}

/* ================================================================
 *  扩散上下文清理
 * ================================================================ */
void diffusion_cleanup(DiffusionCtx* ctx) {
    if (!ctx) return;
    free(ctx->_vocab_scores); ctx->_vocab_scores = NULL;
    free(ctx->_sem_scores);   ctx->_sem_scores   = NULL;
    free(ctx->_tpl_scores);   ctx->_tpl_scores   = NULL;
    free(ctx->_emo_scores);   ctx->_emo_scores   = NULL;
    ctx->_vocab_cap = 0;
    ctx->_sem_cap   = 0;
    ctx->_tpl_cap   = 0;
    ctx->_emo_cap   = 0;
}

/* ================================================================
 *  单层扩散
 * ================================================================ */

int diffusion_spread(SubTopology* layer,
                      int* active_ids, int active_count,
                      float* scores,
                      float decay,
                      float temperature) {
    if (!layer || !layer->net || !scores || active_count <= 0) return 0;

    int spread = 0;
    for (int i = 0; i < active_count; i++) {
        int nid = active_ids[i];
        if (nid < 0 || nid >= layer->net->node_count) continue;
        ReasoningNode* node = layer->net->nodes[nid];
        if (!node) continue;
        /* 枢纽词跳过: >2000 边的超级连通节点 (如"你"8000边、"是"8000边) 扩散噪声太大 */
        if (node->edge_count > 2000) continue;

        for (int c = 0; c < node->edge_count; c++) {
            int tid = node->edges[c].target ? node->edges[c].target->node_id : -1;
            if (tid < 0 || tid >= layer->net->node_count) continue;
            float w = node->edges[c].weight * decay;
            if (node->edge_count > 0) w /= sqrtf((float)node->edge_count);
            // 温度扰动：如果 temperature > 0，添加随机噪声
            if (temperature > 0.0f) {
                float noise = ((float)(rand() % 201) - 100.0f) / 100.0f;  // -1.0 ~ 1.0
                w *= (1.0f + noise * temperature);
                if (w < 0.0f) w = 0.0f;
            }
            scores[tid] += w;
            spread++;
        }
    }
    return spread;
}

/* ================================================================
 *  跨层激活（名称匹配）
 *  源层激活的节点通过 concept 名匹配到目标层对应节点
 * ================================================================ */

static int _cross_by_name(SubTopology* src, int* src_ids, int src_count,
                           SubTopology* dst, float* scores, float weight) {
    if (!src || !dst || !scores) return 0;
    int spread = 0;
    for (int i = 0; i < src_count; i++) {
        ReasoningNode* sn = src->net->nodes[src_ids[i]];
        if (!sn || !sn->concept) continue;
        /* O(1) 哈希查找替代 O(N) 线性 strcmp 扫描 */
        int dst_id = huarong_net_find_concept(dst->net, sn->concept);
        if (dst_id >= 0) {
            scores[dst_id] += weight;
            spread++;
        }
    }
    return spread;
}

/* ================================================================
 *  侧抑制
 * ================================================================ */

void diffusion_side_inhibit(DiffusionCandidate* cands, int count,
                             const char** selected, int sel_count) {
    if (!cands || !selected) return;

    for (int i = 0; i < count; i++) {
        if (cands[i].used) continue;
        for (int s = 0; s < sel_count; s++) {
            if (!selected[s] || !cands[i].word) continue;
            /* 同类词互抑：完全相同或前2字相同 */
            if (strcmp(cands[i].word, selected[s]) == 0) {
                cands[i].total_score *= 0.1f;  /* 完全重复，大抑 */
                break;
            }
            if (strncmp(cands[i].word, selected[s], 2) == 0) {
                cands[i].total_score *= 0.5f;  /* 同前缀，小抑 */
            }
        }
    }
}

/* ================================================================
 *  模板评分
 * ================================================================ */

float diffusion_template_score(DiffusionCtx* ctx,
                                const char** words, int count) {
    if (!ctx || !ctx->template || !ctx->template->net || count < 2) return 0.5f;

    float score = 0;
    int pairs = 0;
    for (int i = 0; i < count - 1; i++) {
        if (!words[i] || !words[i+1]) continue;
        /* 在模板拓扑中找相邻词对 */
        for (int j = 0; j < ctx->template->net->node_count && j < 50; j++) {
            ReasoningNode* tn = ctx->template->net->nodes[j];
            if (!tn || !tn->concept) continue;
            if (strstr(tn->concept, words[i]) && strstr(tn->concept, words[i+1]))
                score += tn->activation;
        }
        pairs++;
    }
    return pairs > 0 ? score / pairs : 0.5f;
}

/* ================================================================
 *  候选排序（降序）
 * ================================================================ */

static int _cand_cmp(const void* a, const void* b) {
    float sa = ((const DiffusionCandidate*)a)->total_score;
    float sb = ((const DiffusionCandidate*)b)->total_score;
    return (sa < sb) ? 1 : (sa > sb) ? -1 : 0;
}

/* 部分选择：只找前 K 个最大元素（无需完整排序） */
static void _sel_top_k(DiffusionCandidate* arr, int n, int k) {
    if (n <= k) return;
    for (int i = 0; i < k && i < n; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++)
            if (arr[j].total_score > arr[best].total_score) best = j;
        if (best != i) {
            DiffusionCandidate t = arr[i]; arr[i] = arr[best]; arr[best] = t;
        }
    }
}

/* ================================================================
 *  多层扩散主函数
 * ================================================================ */

int diffusion_generate(DiffusionCtx* ctx,
                        const char* input,
                        const char** output_words,
                        int max_output) {
    if (!ctx || !ctx->master || !ctx->vocab || !input || !output_words) return 0;

    /* ── 第0步：按字符滑动窗口匹配词汇拓扑节点 ──
     * 中文 UTF-8 每字 1-4 字节，必须按字符边界截取子串，
     * 否则 "人工智能"(12字节) 在字节窗口里永远拼不出来。 */

    /* 建立字符位置数组: char_pos[i] = 第 i 个字符的起始字节偏移 */
    int char_pos[256];
    int char_count = 0;
    const char* cp = input;
    while (*cp && char_count < 255) {
        char_pos[char_count++] = (int)(cp - input);
        unsigned char c = (unsigned char)*cp;
        if      (c < 0x80)       cp += 1;
        else if ((c & 0xE0) == 0xC0) cp += 2;
        else if ((c & 0xF0) == 0xE0) cp += 3;
        else                     cp += 4;
    }
    char_pos[char_count] = (int)(cp - input);  /* 末尾哨兵 */

    int active_ids[DIFF_MAX_CANDIDATES];
    int active_count = 0;

    /* 词锚定优先输出队列：命中的词节点 concept 直接进输出候选
     * （v0.6 Phase 2b：输入关联的实义词优先于散字联想） */
    const char* word_prio[16];
    int word_prio_count = 0;

    /* ── 第0步A：词锚定（v0.6）──
     * 滑窗先查概念拓扑（词层）→ 命中词节点 → 通过 cross-link
     * 激活其组成字（组合强化）。词从语料统计自己长出来（词巩固），
     * 生成端先词后字：词级语义聚焦，字级细粒度兜底。 */
    if (ctx->concept && ctx->concept->net && ctx->master->cross_link_count > 0) {
        for (int win_chars = 5; win_chars >= 2 && active_count < 64; win_chars--) {
            for (int ci = 0; ci + win_chars <= char_count; ci++) {
                if (active_count >= 64) break;
                int byte_start = char_pos[ci];
                int byte_end   = char_pos[ci + win_chars];
                int byte_len   = byte_end - byte_start;
                if (byte_len <= 0 || byte_len > 31) continue;
                char sub[32];
                memcpy(sub, input + byte_start, byte_len);
                sub[byte_len] = 0;

                int cnid = huarong_net_find_concept(ctx->concept->net, sub);
                if (cnid < 0) continue;

                /* 词节点 concept 进优先输出队列（输入直接关联的实义词） */
                if (cnid < ctx->concept->net->node_count) {
                    ReasoningNode* wnode = ctx->concept->net->nodes[cnid];

                    /* v0.5.10 fix: 按需解冻——冻结节点 edges=NULL，
                     * 语义场/扩散走边全失效 → 对话退化成"好的。"。
                     * 命中冷节点时从 node_cache 解冻（读 brain_state.dat
                     * 重建边），解冻后立即可用。auto_thaw_ok 闸门由
                     * brainstem 按 health 设置（YELLOW/RED 自动禁解冻）。 */
                    if (wnode && wnode->is_cooled && ctx->master->node_cache &&
                        ((NodeCache*)ctx->master->node_cache)->auto_thaw_ok) {
                        node_cache_thaw((NodeCache*)ctx->master->node_cache,
                                        ctx->concept->net, wnode, 0);
                    }

                    if (wnode && wnode->concept && wnode->concept[0] &&
                        word_prio_count < 16) {
                        int dup2 = 0;
                        for (int k2 = 0; k2 < word_prio_count; k2++)
                            if (strcmp(word_prio[k2], wnode->concept) == 0) { dup2 = 1; break; }
                        if (!dup2) word_prio[word_prio_count++] = wnode->concept;
                        fprintf(stderr, "[词锚定DBG] win=%d 命中词节点: %s\n",
                                win_chars, wnode->concept);
                    }

                    /* 词级语义场（v0.5.7）：词节点的词-词邻居（概念拓扑
                     * 共现边）→ 语义场词进优先队列——"时间"→
                     * "时间 过去 钟表"，子目标答案有实质内容 */
                    if (wnode && wnode->edges && word_prio_count < 16) {
                        for (int we = 0; we < wnode->edge_count; we++) {
                            ReasoningNode* wnb = wnode->edges[we].target;
                            if (!wnb || !wnb->concept || !wnb->concept[0]) continue;
                            if (wnb->node_id == cnid) continue;
                            float w = wnode->edges[we].weight;
                            if (w < 0.3f) continue;      /* 弱边不构成语义场 */
                            if (is_function_word(wnb->concept)) continue;
                            int dup3 = 0;
                            for (int k3 = 0; k3 < word_prio_count; k3++)
                                if (strcmp(word_prio[k3], wnb->concept) == 0) { dup3 = 1; break; }
                            if (!dup3) {
                                word_prio[word_prio_count++] = wnb->concept;
                                if (word_prio_count >= 16) break;
                            }
                        }
                    }

                    /* 语义场查询（v0.5.7）：词 → 所属语义概念（cross-link）
                     * → 概念的成员词（语义场词）——聚类语义场优先于
                     * 共现近似（时间 → 时间概念场：过去/钟表/现在） */
                    if (wnode && ctx->master->cross_links &&
                        ctx->master->cross_link_count > 0) {
                        for (int li = 0; li < ctx->master->cross_link_count; li++) {
                            CrossTopologyLink* l = ctx->master->cross_links[li];
                            if (!l || l->from_topo_id != TOPO_CONCEPT ||
                                l->from_node_id != cnid ||
                                l->to_topo_id != TOPO_SEMANTIC) continue;
                            int snid = l->to_node_id;
                            for (int li2 = 0; li2 < ctx->master->cross_link_count; li2++) {
                                CrossTopologyLink* l2 = ctx->master->cross_links[li2];
                                if (!l2 || l2->from_topo_id != TOPO_SEMANTIC ||
                                    l2->from_node_id != snid ||
                                    l2->to_topo_id != TOPO_CONCEPT ||
                                    l2->to_node_id == cnid) continue;
                                if (l2->to_node_id >= ctx->concept->net->node_count) continue;
                                ReasoningNode* mw = ctx->concept->net->nodes[l2->to_node_id];
                                if (!mw || !mw->concept || !mw->concept[0]) continue;
                                if (is_function_word(mw->concept)) continue;
                                int dup4 = 0;
                                for (int k4 = 0; k4 < word_prio_count; k4++)
                                    if (strcmp(word_prio[k4], mw->concept) == 0) { dup4 = 1; break; }
                                if (!dup4) {
                                    word_prio[word_prio_count++] = mw->concept;
                                    if (word_prio_count >= 16) break;
                                }
                            }
                            break;   /* 只查第一个语义概念 */
                        }
                    }
                }

                /* 词节点 → cross-link → 组成字，激活字节点 */
                for (int li = 0; li < ctx->master->cross_link_count; li++) {
                    CrossTopologyLink* l = ctx->master->cross_links[li];
                    if (!l || l->from_topo_id != TOPO_CONCEPT ||
                        l->from_node_id != cnid ||
                        l->to_topo_id != TOPO_VOCABULARY) continue;
                    int vid = l->to_node_id;
                    if (vid < 0 || vid >= ctx->vocab->net->node_count) continue;
                    ReasoningNode* vn = ctx->vocab->net->nodes[vid];
                    if (!vn) continue;
                    vn->activation += 0.3f;
                    int dup = 0;
                    for (int d = 0; d < active_count; d++)
                        if (active_ids[d] == vid) { dup = 1; break; }
                    if (!dup) active_ids[active_count++] = vid;
                }
            }
            if (active_count > 0) break;  /* 长词优先 */
        }
    }

    /* 从 5字窗口 到 1字窗口，长匹配优先（字拓扑单字兜底，补全非词覆盖的字） */
    for (int win_chars = 5; win_chars >= 1; win_chars--) {
        for (int ci = 0; ci + win_chars <= char_count; ci++) {
            if (active_count >= 64) break;
            int byte_start = char_pos[ci];
            int byte_end   = char_pos[ci + win_chars];
            int byte_len   = byte_end - byte_start;
            if (byte_len <= 0 || byte_len > 31) continue;
            char sub[32];
            memcpy(sub, input + byte_start, byte_len);
            sub[byte_len] = 0;

            int nid = huarong_net_find_concept(ctx->vocab->net, sub);
            if (nid >= 0) {
                /* v0.5.10 fix: 按需解冻（同词锚定）——字/词节点冻结时
                 * edges=NULL，扩散走边失效。命中冷节点从 cache 解冻。 */
                if (nid < ctx->vocab->net->node_count) {
                    ReasoningNode* vn = ctx->vocab->net->nodes[nid];
                    if (vn && vn->is_cooled && ctx->master->node_cache &&
                        ((NodeCache*)ctx->master->node_cache)->auto_thaw_ok) {
                        node_cache_thaw((NodeCache*)ctx->master->node_cache,
                                        ctx->vocab->net, vn, 0);
                    }
                }
                int dup = 0;
                for (int d = 0; d < active_count; d++)
                    if (active_ids[d] == nid) { dup = 1; break; }
                if (!dup) {
                    active_ids[active_count++] = nid;
                    ctx->vocab->net->nodes[nid]->activation += 0.2f;
                }
            }
        }
        if (active_count > 0) break;  /* 长匹配优先 */
    }
    if (active_count == 0) return 0;

    /* ── 输入主导语言检测 ── */
    /* 统计激活节点中 CJK (>0x80) vs ASCII 的比例，决定扩散偏好 */
    int cjk_nodes = 0, ascii_nodes = 0;
    for (int a = 0; a < active_count; a++) {
        ReasoningNode* an = ctx->vocab->net->nodes[active_ids[a]];
        if (!an || !an->concept) continue;
        if ((unsigned char)an->concept[0] >= 0x80) cjk_nodes++; else ascii_nodes++;
    }
    /* dominant: +1=CJK优先, -1=ASCII优先, 0=混合 */
    int lang_dom = (cjk_nodes > ascii_nodes * 2) ? 1 :
                   (ascii_nodes > cjk_nodes * 2) ? -1 : 0;

    /* ── 两跳激活扩散：直接匹配 → 一跳邻居 → 两跳邻居，几何衰减 ── */
    /* "苹果"激活 → "红色""水果"一跳 → "颜色""好吃"两跳 */
    #define SPREAD_1HOP 1.0f
    #define SPREAD_2HOP 0.4f
    #define SPREAD_MAX_EXTRA 256
    #define LANG_BOOST_SAME  1.3f   /* 同语言邻居激活增强 */
    #define LANG_BOOST_CROSS 0.4f   /* 跨语言邻居激活衰减 */
    /* 判断节点的语言类型 */
    #define NODE_IS_CJK(n) ((n) && (n)->concept && (unsigned char)(n)->concept[0] >= 0x80)

    int spread1_ids[SPREAD_MAX_EXTRA];
    int spread1_count = 0;

    /* 一跳：直接匹配节点的邻居 */
    for (int a = 0; a < active_count; a++) {
        ReasoningNode* src = ctx->vocab->net->nodes[active_ids[a]];
        if (!src) continue;
        /* v0.5.13 fix: 冷节点先解冻——否则 edge_count=0 无邻居可走（
         * 之前只有词锚定/字匹配解冻，一跳扩散中间层全断） */
        if (src->is_cooled && ctx->master->node_cache &&
            ((NodeCache*)ctx->master->node_cache)->auto_thaw_ok) {
            node_cache_thaw((NodeCache*)ctx->master->node_cache,
                            ctx->vocab->net, src, 0);
        }
        for (int e = 0; e < src->edge_count && spread1_count < SPREAD_MAX_EXTRA; e++) {
            ReasoningNode* nb = src->edges[e].target;
            if (!nb || nb->node_id == src->node_id) continue;
            int dup = 0;
            for (int d = 0; d < active_count; d++)
                if (active_ids[d] == nb->node_id) { dup = 1; break; }
            for (int d = 0; d < spread1_count; d++)
                if (spread1_ids[d] == nb->node_id) { dup = 1; break; }
            if (!dup) {
                float lang_mult = 1.0f;
                if (lang_dom != 0 && NODE_IS_CJK(nb)) {
                    lang_mult = (lang_dom > 0) ? LANG_BOOST_SAME : LANG_BOOST_CROSS;
                } else if (lang_dom != 0) {
                    lang_mult = (lang_dom < 0) ? LANG_BOOST_SAME : LANG_BOOST_CROSS;
                }
                nb->activation += SPREAD_1HOP * src->edges[e].weight * lang_mult;
                spread1_ids[spread1_count++] = nb->node_id;
            }
        }
    }

    /* 两跳：一跳邻居的邻居 — 衰减系数 SPREAD_2HOP */
    int spread2_count = 0;
    for (int s = 0; s < spread1_count && spread1_count + spread2_count < SPREAD_MAX_EXTRA; s++) {
        ReasoningNode* src = ctx->vocab->net->nodes[spread1_ids[s]];
        if (!src) continue;
        /* v0.5.13 fix: 两跳同样先解冻 */
        if (src->is_cooled && ctx->master->node_cache &&
            ((NodeCache*)ctx->master->node_cache)->auto_thaw_ok) {
            node_cache_thaw((NodeCache*)ctx->master->node_cache,
                            ctx->vocab->net, src, 0);
        }
        for (int e = 0; e < src->edge_count; e++) {
            ReasoningNode* nb = src->edges[e].target;
            if (!nb || nb->node_id == src->node_id) continue;
            int dup = 0;
            for (int d = 0; d < active_count; d++)
                if (active_ids[d] == nb->node_id) { dup = 1; break; }
            for (int d = 0; d < spread1_count; d++)
                if (spread1_ids[d] == nb->node_id) { dup = 1; break; }
            for (int d = 0; d < spread2_count; d++)
                if (spread1_ids[spread1_count + d] == nb->node_id) { dup = 1; break; }
            if (!dup) {
                float lm2 = 1.0f;
                if (lang_dom != 0 && NODE_IS_CJK(nb))
                    lm2 = (lang_dom > 0) ? LANG_BOOST_SAME : LANG_BOOST_CROSS;
                else if (lang_dom != 0)
                    lm2 = (lang_dom < 0) ? LANG_BOOST_SAME : LANG_BOOST_CROSS;
                nb->activation += SPREAD_2HOP * src->edges[e].weight * lm2;
                spread1_ids[spread1_count + spread2_count] = nb->node_id;
                spread2_count++;
            }
        }
    }
    /* (spread1/2 共享 spread1_ids 数组，不释放；仅用于去重) */

    /* ── Jaccard 邻接相似度激活重加权 ── */
    /* 对被激活的节点，计算其与输入锚点集的邻居重叠率，提升语义精准度 */
    int total_spread = spread1_count + spread2_count;
    for (int si = 0; si < total_spread; si++) {
        int nid = spread1_ids[si];
        if (nid < 0 || nid >= ctx->vocab->net->node_count) continue;
        ReasoningNode* node = ctx->vocab->net->nodes[nid];
        if (!node || node->edge_count == 0) continue;

        /* 统计该节点与输入激活集的共享邻居数 */
        int shared = 0;
        for (int e = 0; e < node->edge_count; e++) {
            int tgt = node->edges[e].target ? node->edges[e].target->node_id : -1;
            if (tgt < 0) continue;
            for (int a = 0; a < active_count; a++)
                if (active_ids[a] == tgt) { shared++; break; }
            if (shared > 0) continue; /* 只统计一次 */
            for (int s = 0; s < total_spread && s < SPREAD_MAX_EXTRA; s++)
                if (spread1_ids[s] == tgt) { shared++; break; }
        }
        /* Jaccard ≈ shared / node->edge_count，映射到 [0.5, 1.5] 乘数 */
        float jac = (node->edge_count > 0) ? (float)shared / (float)node->edge_count : 0.0f;
        float boost = 0.5f + jac;  /* jac=0→0.5x, jac=1.0→1.5x */
        node->activation *= boost;
    }

    /* 调试: 打印输入激活的词和其邻居 */
    {
        printf("[扩散] 输入=\"%.60s\" 激活 %d 节点:", input, active_count);
        for (int a = 0; a < active_count && a < 5; a++) {
            ReasoningNode* an = ctx->vocab->net->nodes[active_ids[a]];
            printf(" %s(%d边)", an ? an->concept : "?", an ? an->edge_count : 0);
        }
        printf("\n");
    }

    /* ── 输入词注入: 将输入匹配的候选词直接加入候选池，标记高权重 ── */
    /* 这些词是用户直接使用的词，应该出现在输出中 */
    float input_boost_score = 1.5f;  /* 高权重确保这些词进入 top-K */
    (void)input_boost_score;

    /* ── 分配/复用评分数组 (每层独立容量追踪，避免缓冲区越界) ── */
    int vn = ctx->vocab->net->node_count;
    int sn = ctx->semantic ? ctx->semantic->net->node_count : 0;
    int tn = ctx->template ? ctx->template->net->node_count : 0;
    int en = ctx->emotion  ? ctx->emotion->net->node_count  : 0;

    /* 词汇层 — 始终分配 */
    if (!ctx->_vocab_scores || ctx->_vocab_cap < vn) {
        free(ctx->_vocab_scores);
        ctx->_vocab_scores = (float*)calloc(vn, sizeof(float));
        if (!ctx->_vocab_scores) return -1;
        ctx->_vocab_cap = vn;
    }
    /* 语义层 */
    if (sn > 0 && (!ctx->_sem_scores || ctx->_sem_cap < sn)) {
        free(ctx->_sem_scores);
        ctx->_sem_scores = (float*)calloc(sn, sizeof(float));
        if (!ctx->_sem_scores) return -1;
        ctx->_sem_cap = sn;
    } else if (sn == 0) {
        ctx->_sem_scores = NULL;  /* 无语义层时清零 */
    }
    /* 模板层 */
    if (tn > 0 && (!ctx->_tpl_scores || ctx->_tpl_cap < tn)) {
        free(ctx->_tpl_scores);
        ctx->_tpl_scores = (float*)calloc(tn, sizeof(float));
        if (!ctx->_tpl_scores) return -1;
        ctx->_tpl_cap = tn;
    } else if (tn == 0) {
        ctx->_tpl_scores = NULL;
    }
    /* 情绪层 */
    if (en > 0 && (!ctx->_emo_scores || ctx->_emo_cap < en)) {
        free(ctx->_emo_scores);
        ctx->_emo_scores = (float*)calloc(en, sizeof(float));
        if (!ctx->_emo_scores) return -1;
        ctx->_emo_cap = en;
    } else if (en == 0) {
        ctx->_emo_scores = NULL;
    }

    float* vocab_scores = ctx->_vocab_scores;
    float* sem_scores   = ctx->_sem_scores;
    float* tpl_scores   = ctx->_tpl_scores;
    float* emo_scores   = ctx->_emo_scores;
    memset(vocab_scores, 0, vn * sizeof(float));
    if (sem_scores) memset(sem_scores, 0, sn * sizeof(float));
    if (tpl_scores) memset(tpl_scores, 0, tn * sizeof(float));
    if (emo_scores) memset(emo_scores, 0, en * sizeof(float));

    /* ── 第1步：词汇层初始扩散 ── */
    float cur_decay = 1.0f;
    int cur_ids[DIFF_MAX_CANDIDATES];
    int cur_count = active_count;
    memcpy(cur_ids, active_ids, cur_count * sizeof(int));

    for (int d = 0; d < ctx->depth && cur_count > 0; d++) {
        cur_decay *= ctx->decay;

        /* v0.5.13 fix: 每轮扩散前解冻当前活跃的冷节点——中间层冻结节点
         * 无边，扩散在此断链（此前只有词锚定/字匹配两处解冻） */
        for (int _i = 0; _i < cur_count; _i++) {
            int _nid = cur_ids[_i];
            if (_nid < 0 || _nid >= ctx->vocab->net->node_count) continue;
            ReasoningNode* _n = ctx->vocab->net->nodes[_nid];
            if (_n && _n->is_cooled && ctx->master->node_cache &&
                ((NodeCache*)ctx->master->node_cache)->auto_thaw_ok) {
                node_cache_thaw((NodeCache*)ctx->master->node_cache,
                                ctx->vocab->net, _n, 0);
            }
        }

        /* 词汇层自身扩散 */
        diffusion_spread(ctx->vocab, cur_ids, cur_count,
                         vocab_scores, cur_decay, ctx->temperature);

        /* 跨到语义层（名称匹配） */
        if (ctx->semantic && sem_scores) {
            _cross_by_name(ctx->vocab, cur_ids, cur_count,
                            ctx->semantic, sem_scores, cur_decay * 0.4f);
            /* 语义层内部扩散 */
            #define DIFF_ACTIVE_MAX 256
            int sem_active[DIFF_ACTIVE_MAX]; int sem_cnt = 0;
            for (int i = 0; i < sn && sem_cnt < DIFF_ACTIVE_MAX; i++) {
                if (sem_scores[i] > 0.01f) sem_active[sem_cnt++] = i;
            }
            if (sem_cnt > 0) {
                diffusion_spread(ctx->semantic, sem_active, sem_cnt,
                                 sem_scores, cur_decay * 0.3f, ctx->temperature);
                /* 语义层回归词汇层 */
                _cross_by_name(ctx->semantic, sem_active, sem_cnt,
                                ctx->vocab, vocab_scores, cur_decay * 0.3f);
            }
        }

        /* 跨到模板层（名称匹配） */
        if (d == 0 && ctx->template && tpl_scores) {
            _cross_by_name(ctx->vocab, cur_ids, cur_count,
                            ctx->template, tpl_scores, cur_decay * 0.5f);
        }

        /* 更新当前活跃集 = 本轮得分最高的 K 个词（跳过虚词、压制枢纽词） */
        DiffusionCandidate tmp[DIFF_MAX_CANDIDATES];
        int tmp_cnt = 0;
        for (int i = 0; i < vn && tmp_cnt < DIFF_MAX_CANDIDATES; i++) {
            if (vocab_scores[i] > 0.001f && ctx->vocab->net->nodes[i]) {
                ReasoningNode* nd = ctx->vocab->net->nodes[i];
                const char* concept = nd->concept;
                if (concept && is_function_word(concept)) continue;
                tmp[tmp_cnt].node_id     = i;
                /* 目标度惩罚前移到活跃集选择，防止枢纽词劫持后续扩散方向 */
                float dp = 1.0f;
                if (nd->edge_count > 10) {
                    dp = 1.0f / log2f((float)(nd->edge_count));
                    if (dp < 0.05f) dp = 0.05f;
                }
                tmp[tmp_cnt].total_score = vocab_scores[i] * dp;
                tmp[tmp_cnt].word        = concept;
                tmp_cnt++;
            }
        }
        _sel_top_k(tmp, tmp_cnt, ctx->top_k);

        cur_count = tmp_cnt < ctx->top_k ? tmp_cnt : ctx->top_k;
        for (int i = 0; i < cur_count; i++) {
            cur_ids[i] = tmp[i].node_id;
        }
    }

    /* ── 第2步：收敛 → 综合评分（跳过虚词；跨层反馈已通过 _cross_by_name 回流至 vocab_scores） ── */
    DiffusionCandidate final[DIFF_MAX_CANDIDATES];
    int final_cnt = 0;
    for (int i = 0; i < vn && final_cnt < DIFF_MAX_CANDIDATES; i++) {
        if (vocab_scores[i] < 0.001f) continue;
        ReasoningNode* n = ctx->vocab->net->nodes[i];
        if (!n || !n->concept) continue;
        if (is_function_word(n->concept)) continue;

        final[final_cnt].node_id       = i;
        final[final_cnt].vocab_score   = vocab_scores[i];
        final[final_cnt].semantic_score = 0;
        final[final_cnt].template_score = 0;
        final[final_cnt].emotion_score  = 0;
        /* P1: 目标度惩罚 — 枢纽词(>10边)得分衰减，特定词优势放大
         *  10边 → 0.28x, 100边 → 0.15x, 1000边 → 0.10x
         *  解决扩散被高频共现枢纽词(很/大/的)劫持的问题 */
        float degree_penalty = 1.0f;
        if (n->edge_count > 10) {
            degree_penalty = 1.0f / log2f((float)(n->edge_count));
            if (degree_penalty < 0.05f) degree_penalty = 0.05f;  /* 软底，不完全抹零 */
        }

        /* P2: 话题相关性（v0.6）— 候选与输入锚定节点集的边权关联。
         * 话题聚焦：强边邻居（三国→演义/关羽）相关性高 → 主输出；
         * 高频噪声（时间）与锚定集无强边 → 相关性低 → 降权。
         * 联想发散：弱边/两跳候选相关性低但保留（扩散激活兜底）。 */
        float relevance = 0.0f;
        for (int a = 0; a < active_count; a++) {
            ReasoningNode* anchor = ctx->vocab->net->nodes[active_ids[a]];
            if (!anchor || !anchor->edges) continue;
            for (int e = 0; e < anchor->edge_count; e++) {
                if (anchor->edges[e].target == n) {
                    float w = anchor->edges[e].weight;
                    if (w > relevance) relevance = w;
                    break;
                }
            }
        }
        final[final_cnt].total_score    = vocab_scores[i] * degree_penalty *
                                          (0.3f + 1.7f * relevance);
        final[final_cnt].relevance      = relevance;
        final[final_cnt].word = n->concept;
        final[final_cnt].used = 0;
        final_cnt++;
    }

    qsort(final, final_cnt, sizeof(DiffusionCandidate), _cand_cmp);

    /* ── 输入词注入: 将用户输入匹配的词轻微提升 ── */
    /* 少边词（更具体）加权更大，枢纽词(>2000边)不加权 */
    for (int a = 0; a < active_count; a++) {
        int nid = active_ids[a];
        if (nid < 0 || nid >= vn) continue;
        ReasoningNode* an = ctx->vocab->net->nodes[nid];
        if (!an) continue;
        float boost = 0.0f;
        if (an->edge_count <= 0)       boost = 0.6f;  /* 无连接词: 最具体 */
        else if (an->edge_count < 100) boost = 0.4f;  /* 少边词 */
        else if (an->edge_count < 2000)boost = 0.2f;  /* 中等连接词 */
        /* 枢纽词 (>2000边) 不加权 */
        if (boost > 0.0f) {
            for (int f = 0; f < final_cnt; f++) {
                if (final[f].node_id == nid) {
                    final[f].total_score += boost;
                    final[f].vocab_score += boost;
                    break;
                }
            }
        }
    }
    qsort(final, final_cnt, sizeof(DiffusionCandidate), _cand_cmp);

    /* ── 第2.5步：图交集重排序 ──
     * 核心思想：正确答案应该同时连接到多个输入词。
     * 例如"苹果是什么颜色"→"苹果"和"颜色"都连接到"红色"→高交集分。
     * 通用枢纽词("很"/"大")每个输入词都连但边权低→低交集分。 */
    {
        /* 对于每个final候选词，计算与多少个输入激活节点有高权边 */
        for (int f = 0; f < final_cnt; f++) {
            int gen_nid = final[f].node_id;
            if (gen_nid < 0 || gen_nid >= vn) continue;
            int input_connections = 0;
            float total_edge_weight = 0.0f;
            for (int a = 0; a < active_count; a++) {
                int src_nid = active_ids[a];
                if (src_nid < 0 || src_nid >= vn) continue;
                ReasoningNode* src = ctx->vocab->net->nodes[src_nid];
                if (!src) continue;
                for (int e = 0; e < src->edge_count; e++) {
                    if (src->edges[e].target &&
                        src->edges[e].target->node_id == gen_nid) {
                        float ew = src->edges[e].weight;
                        if (ew >= 0.3f) {
                            input_connections++;
                            total_edge_weight += ew;
                        }
                        break;
                    }
                }
            }
            /* 交集分：连接的输入词越多、边权越高 → 越像正确答案 */
            float intersect_score = 0.0f;
            if (active_count > 0 && input_connections > 0) {
                float coverage = (float)input_connections / (float)active_count;
                float avg_weight = total_edge_weight / (float)input_connections;
                intersect_score = coverage * (0.3f + 0.7f * avg_weight);
            }
            /* 覆盖率达到50%以上(两个输入词都连) → 大幅加分
             * 覆盖率低 → 不加分(不惩罚，让扩散原始分数仍然有效) */
            if (intersect_score > 0.3f) {
                final[f].total_score += intersect_score * 2.0f;
            }
        }
    }
    qsort(final, final_cnt, sizeof(DiffusionCandidate), _cand_cmp);

    /* ── 第3步：模板导向 → 从 template 层取最佳句式 ── */
    const char* tpl_pattern = NULL;
    if (ctx->template && tpl_scores && tn > 0) {
        float best_tpl_score = 0;
        for (int i = 0; i < tn; i++) {
            if (tpl_scores[i] > best_tpl_score && ctx->template->net->nodes[i]) {
                best_tpl_score = tpl_scores[i];
                tpl_pattern = ctx->template->net->nodes[i]->concept;
            }
        }
        if (tpl_pattern) printf("[扩散] 模板: %s (%.3f)\n", tpl_pattern, best_tpl_score);
    }

    /* ── 第4步：句式导向输出 ── */
    /* 解析模板概念中的连接词: "N的N是N的N" → ["", "的", "是", "的"] */
    const char* connectors[DIFF_MAX_SEQUENCE];
    char conn_pool[128];  /* 小对象池，避免 strdup 泄漏 */
    int conn_pool_pos = 0;
    int conn_count = 0;
    if (tpl_pattern) {
        const char* p = tpl_pattern;
        while (*p && conn_count < DIFF_MAX_SEQUENCE) {
            if (*p == 'N' || *p == 'V' || *p == 'A') {
                connectors[conn_count++] = "";  /* 实词位置 */
                p++;
            } else {
                /* 收集连续的非N字符作为连接词 */
                int ci = 0;
                while (*p && *p != 'N' && *p != 'V' && *p != 'A' && ci < 7 && conn_pool_pos + ci < 127)
                    conn_pool[conn_pool_pos + ci++] = *p++;
                if (ci > 0) {
                    conn_pool[conn_pool_pos + ci] = 0;
                    connectors[conn_count++] = conn_pool + conn_pool_pos;
                    conn_pool_pos += ci + 1;
                }
            }
        }
    }

    /* 如果模板没能提供足够连接词，用默认 */
    if (conn_count < 2) {
        const char* fallback[] = {"", "的", "是", "和", "了", "在"};
        for (int i = 0; i < 6; i++) connectors[i] = fallback[i];
        conn_count = 6;
    }

    /* ── 第4步：POS 词性重排输出 ── */
    int out;

    if (ctx->emergent_pos) {
        /* ═══ 涌现状词类系统重排：按词性优先级输出 ═══ */
        POSTag      word_pos[DIFF_MAX_SEQUENCE];    /* 栈数组 128B */
        const char* word_buf[DIFF_MAX_SEQUENCE];    /* 栈数组 256B */
        int         word_count = 0;

        /* 第0.5遍: 词锚定优先 — 命中的词节点先入候选（输入直接关联的
         * 实义词，如"三国"→直接输出三国/演义），再让散字扩散补充 */
        for (int p = 0; p < word_prio_count && word_count < DIFF_MAX_SEQUENCE; p++) {
            if (!word_prio[p] || strlen(word_prio[p]) < 2) continue;
            if (is_function_word(word_prio[p])) continue;
            /* v0.5.7: 子问题模板词过滤——PFE 子目标"X的操作步骤/前置条件"
             * 里的模板词（操作/步骤/方法/条件）会命中概念拓扑并被输出，
             * 污染答案（实测"操作小标起来"）。这些是问题框架词不是内容 */
            {
                static const char* TPL_WORDS[] = {"操作", "步骤", "方法", "前置", "条件",
                    "资源", "概念", "相关", "以及", "然后", "所以", "因为", "什么",
                    "怎么", "如何", "为什么", "小标", "起来", "需要", "影响", "方面"};
                int tpl = 0;
                for (int ti = 0; ti < (int)(sizeof(TPL_WORDS)/sizeof(TPL_WORDS[0])); ti++) {
                    if (strcmp(word_prio[p], TPL_WORDS[ti]) == 0) { tpl = 1; break; }
                }
                if (tpl) continue;
            }
            if (lang_dom > 0 && (unsigned char)word_prio[p][0] < 0x80) continue;
            if (lang_dom < 0 && (unsigned char)word_prio[p][0] >= 0x80) continue;
            /* 中文单字不输出（v0.6：口语至少 2 字词，"出大的只了"类噪声） */
            if ((unsigned char)word_prio[p][0] >= 0x80 && strlen(word_prio[p]) == 3) continue;
            int dup = 0;
            for (int w = 0; w < word_count; w++) {
                if (strcmp(word_prio[p], word_buf[w]) == 0) { dup = 1; break; }
            }
            if (dup) continue;
            word_buf[word_count] = word_prio[p];
            word_pos[word_count] = emergent_pos_tag(
                ctx->emergent_pos, ctx->master, word_prio[p]);
            if (word_pos[word_count] == POS_UNKNOWN)
                word_pos[word_count] = english_pos_lookup(word_prio[p]);
            word_count++;
        }

        /* 第一遍: 收集有效候选词 + POS 标注 */
        for (int i = 0; i < final_cnt && word_count < DIFF_MAX_SEQUENCE; i++) {
            if (final[i].used) continue;
            if (!final[i].word || strlen(final[i].word) < 2) continue;
            if (final[i].word[0] == '@' || final[i].word[0] == '?' ||
                (final[i].word[0] == 'H' && final[i].word[1] == 'e')) continue;
            if (strncmp(final[i].word, "sem_", 4) == 0) continue;   /* semantic_growth 匿名节点 */
            if (lang_dom > 0 && (unsigned char)final[i].word[0] < 0x80) continue;  /* 中文主导：过滤英文词 */
            if (lang_dom < 0 && (unsigned char)final[i].word[0] >= 0x80) continue; /* 英文主导：过滤中文词 */
            /* 话题分级（v0.6）：relevance<0.3 的候选是"高频噪声激活"
             * （如"时间"被无关输入激活），不进入主输出——话题聚焦 */
            if (final[i].relevance < 0.3f) continue;
            if (is_function_word(final[i].word)) continue;
            /* 中文单字不输出（v0.6） */
            if ((unsigned char)final[i].word[0] >= 0x80 && strlen(final[i].word) == 3) continue;

            /* 去重 */
            int dup = 0;
            for (int w = 0; w < word_count; w++) {
                if (strcmp(final[i].word, word_buf[w]) == 0) { dup = 1; break; }
            }
            if (dup) continue;

            word_buf[word_count] = final[i].word;
            word_pos[word_count] = emergent_pos_tag(
                ctx->emergent_pos, ctx->master, final[i].word);
            /* 英文兜底: EmergentPOS (zh) 无法分类英文词 */
            if (word_pos[word_count] == POS_UNKNOWN)
                word_pos[word_count] = english_pos_lookup(final[i].word);
            word_count++;
            final[i].used = 1;
        }

        /* 有界联想回退（v0.6）：主输出为空（relevance 过滤后无候选）
         * 时，从锚定集一跳内选次相关节点（边权 0.3-0.5）作为联想，
         * 替代原"全图自由激活"（"时间很大"胡话的病根）。
         * 联想是诚实的次相关，不是随机跳跃。 */
        if (word_count == 0) {
            for (int a = 0; a < active_count && word_count < DIFF_MAX_SEQUENCE; a++) {
                ReasoningNode* anchor = ctx->vocab->net->nodes[active_ids[a]];
                if (!anchor || !anchor->edges) continue;
                for (int e = 0; e < anchor->edge_count; e++) {
                    float w = anchor->edges[e].weight;
                    if (w < 0.3f || w >= 0.6f) continue;  /* 次相关窗口 */
                    ReasoningNode* nb = anchor->edges[e].target;
                    if (!nb || !nb->concept || strlen(nb->concept) < 2) continue;
                    if (is_function_word(nb->concept)) continue;
                    if (lang_dom > 0 && (unsigned char)nb->concept[0] < 0x80) continue;
                    if (lang_dom < 0 && (unsigned char)nb->concept[0] >= 0x80) continue;
                    /* 中文单字不输出（v0.6） */
                    if ((unsigned char)nb->concept[0] >= 0x80 && strlen(nb->concept) == 3) continue;
                    int dup = 0;
                    for (int w2 = 0; w2 < word_count; w2++)
                        if (strcmp(nb->concept, word_buf[w2]) == 0) { dup = 1; break; }
                    if (dup) continue;
                    word_buf[word_count] = nb->concept;
                    word_pos[word_count] = emergent_pos_tag(
                        ctx->emergent_pos, ctx->master, nb->concept);
                    if (word_pos[word_count] == POS_UNKNOWN)
                        word_pos[word_count] = english_pos_lookup(nb->concept);
                    word_count++;
                }
            }
        }

        /* 话题序输出（v0.6）：word_buf 已按话题相关性排序
         * （word_prio 词锚定在前 + final 按 relevance），直接输出
         * 前几个实词（口语短句）。
         * 语法组装（assemble_grammar）暂缓：当前 POS 标注质量下
         * 组装反而乱序（"个人家后来说话..."），话题序更符合中文
         * 口语习惯——等词性标注成熟后再启用语法组装。 */
        out = 0;
        {
            const int max_reply_words = 4;   /* 口语短句：≤4 实词 */
            for (int w = 0; w < word_count && out < max_reply_words; w++) {
                if (!word_buf[w]) continue;
                output_words[out++] = word_buf[w];
            }
        }
    } else {
        /* ═══ 降级路径：模板连接词填充 (无 EmergentPOS 的回退) ═══ */
        int out_fallback = 0;
        const char* selected[DIFF_MAX_SEQUENCE];
        int sel = 0;

        /* 词锚定优先（v0.6）：命中词节点先输出（降级路径也生效，
         * 否则词锚定只在 emergent_pos 分支工作，普通对话走降级时
         * 词库词被 final 顺序淹没——"衣服"→"历史时间"的病根） */
        for (int p = 0; p < word_prio_count && out_fallback < max_output; p++) {
            if (!word_prio[p] || strlen(word_prio[p]) < 2) continue;
            if (is_function_word(word_prio[p])) continue;
            if (lang_dom > 0 && (unsigned char)word_prio[p][0] < 0x80) continue;
            if (lang_dom < 0 && (unsigned char)word_prio[p][0] >= 0x80) continue;
            if ((unsigned char)word_prio[p][0] >= 0x80 && strlen(word_prio[p]) == 3) continue;  /* 中文单字 */
            output_words[out_fallback++] = word_prio[p];
            selected[sel++] = word_prio[p];
        }

        for (int i = 0; i < final_cnt && out_fallback < max_output; i++) {
            if (final[i].used) continue;
            if (!final[i].word || strlen(final[i].word) < 2) continue;
            /* 口语短句截断（v0.6） */
            if (out_fallback >= 4) break;
            if (final[i].word[0] == '@' || final[i].word[0] == '?' ||
                (final[i].word[0] == 'H' && final[i].word[1] == 'e')) continue;
            if (strncmp(final[i].word, "sem_", 4) == 0) continue;   /* semantic_growth 匿名节点 */
            /* fallback 路径不强制语言过滤（保证有输出，避免空回复/句号） */
            if (is_function_word(final[i].word)) continue;
            /* 中文单字不输出（v0.6，降级路径也生效——"时间是"的"是"） */
            if ((unsigned char)final[i].word[0] >= 0x80 && strlen(final[i].word) == 3) continue;

            int inhibited = 0;
            for (int s = 0; s < sel; s++) {
                if (final[i].word && selected[s] &&
                    strcmp(final[i].word, selected[s]) == 0) {
                    inhibited = 1; break;
                }
            }
            if (inhibited) continue;
            /* 语言一致性：fallback 也过滤跨语言词（避免中文输入输出英文） */
            if (lang_dom > 0 && (unsigned char)final[i].word[0] < 0x80) continue;
            if (lang_dom < 0 && (unsigned char)final[i].word[0] >= 0x80) continue;

            /* 模板连接词: 每2个实词插一次 */
            if (out_fallback > 0 && (out_fallback % 3 == 0)) {
                int ci = ((out_fallback - 1) % conn_count);
                if (connectors[ci] && connectors[ci][0])
                    output_words[out_fallback++] = connectors[ci];
            }
            output_words[out_fallback++] = final[i].word;
            selected[sel++] = final[i].word;
            final[i].used = 1;
        }
        out = out_fallback;
    }

    /* ── 静态数组不释放 (ctx生命周期管理) ── */

    return out;
}
