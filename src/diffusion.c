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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ================================================================
 *  虚词/停用词检查
 * ================================================================ */

static int is_function_word(const char* word) {
    if (!word || !word[0]) return 0;
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
        NULL
    };
    for (const char** p = stopwords; *p; p++)
        if (strcmp(word, *p) == 0) return 1;
    return 0;
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
    int verb_idx = -1, verb_val = -1;
    const VerbValency* en_vv = NULL;

    /* 选择配价表 */
    const VerbValency* valency_table = is_english ? ENGLISH_VERB_VALENCY
                                                    : CHINESE_VERB_VALENCY;

    for (int w = 0; w < word_count; w++) {
        if (!word_buf[w] || word_pos[w] != POS_VERB) continue;

        /* 查配价表 */
        for (int v = 0; valency_table[v].verb; v++) {
            const char* vname = valency_table[v].verb;
            /* 英文不区分大小写 */
            int match = is_english
                ? (strcasecmp(word_buf[w], vname) == 0)
                : (strcmp(word_buf[w], vname) == 0);
            if (!match) continue;

            const VerbValency* vv = &valency_table[v];
            verb_idx = w;
            verb_val = v;
            if (is_english) en_vv = vv;
            break;
        }
        if (verb_idx >= 0) break;  /* 找到配价项 */
    }

    /* 如果没找到配价表中的动词，选任意动词 */
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
            /* 检查是否是配价表中的描述性形容词 */
            for (int v = 0; valency_table[v].verb; v++) {
                int match = is_english
                    ? (strcasecmp(word_buf[w], valency_table[v].verb) == 0)
                    : (strcmp(word_buf[w], valency_table[v].verb) == 0);
                if (match && valency_table[v].is_descriptive) {
                    verb_idx = w; verb_val = v;
                    if (is_english) en_vv = &valency_table[v];
                    break;
                }
            }
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

    /* 配价来源优先级: 硬编码表 > 拓扑边推断 > NULL(默认行为) */
    const VerbValency* vv = NULL;
    static VerbValency inferred_vv;  /* 静态避免栈地址失效 */
    if (verb_val >= 0 && !is_english)
        vv = &CHINESE_VERB_VALENCY[verb_val];
    else if (en_vv)
        vv = en_vv;
    else if (ctx && verb_idx >= 0 && word_buf[verb_idx]) {
        /* 硬编码未命中 → 从拓扑边推断配价 */
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
        if (vv->is_copula || vv->is_you) {
            /* 系词/有字句: 宾语 = NOUN/ADJ */
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
        for (int j = 0; j < dst->net->node_count; j++) {
            ReasoningNode* dn = dst->net->nodes[j];
            if (dn && dn->concept && strcmp(sn->concept, dn->concept) == 0) {
                scores[j] += weight;
                spread++;
            }
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

/* ================================================================
 *  多层扩散主函数
 * ================================================================ */

int diffusion_generate(DiffusionCtx* ctx,
                        const char* input,
                        const char** output_words,
                        int max_output) {
    if (!ctx || !ctx->master || !ctx->vocab || !input || !output_words) return 0;

    /* ── 第0步：滑动窗口分词 → 词汇层初始激活 ── */
    int active_ids[DIFF_MAX_CANDIDATES];
    int active_count = 0;
    int input_len = (int)strlen(input);

    /* 逐字扫描 + bigram + trigram 全部尝试匹配 */
    for (int win = 3; win >= 1 && active_count == 0; win--) {
        for (int pos = 0; pos + win <= input_len; pos++) {
            if (active_count >= 64) break;
            char sub[16];
            int sl = win < 15 ? win : 15;
            memcpy(sub, input + pos, sl);
            sub[sl] = 0;
            if (sl < 1) continue;

            int nid = huarong_net_find_concept(ctx->vocab->net, sub);
            if (nid >= 0) {
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

    /* 调试: 打印输入激活的词和其邻居 */
    {
        printf("[扩散] 输入=\"%.30s\" 激活 %d 节点:", input, active_count);
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

    /* ── 分配/复用评分数组 (静态避免反复calloc) ── */
    int vn = ctx->vocab->net->node_count;
    int sn = ctx->semantic ? ctx->semantic->net->node_count : 0;
    int tn = ctx->template ? ctx->template->net->node_count : 0;
    int en = ctx->emotion  ? ctx->emotion->net->node_count  : 0;
    int need = vn;
    if (sn > need) need = sn;
    if (tn > need) need = tn;
    if (en > need) need = en;
    if (!ctx->_vocab_scores || ctx->_score_cap < need) {
        free(ctx->_vocab_scores); free(ctx->_sem_scores);
        free(ctx->_tpl_scores);   free(ctx->_emo_scores);
        ctx->_vocab_scores = (float*)calloc(need, sizeof(float));
        ctx->_sem_scores   = sn ? (float*)calloc(sn, sizeof(float)) : NULL;
        ctx->_tpl_scores   = tn ? (float*)calloc(tn, sizeof(float)) : NULL;
        ctx->_emo_scores   = en ? (float*)calloc(en, sizeof(float)) : NULL;
        ctx->_score_cap    = need;
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

        /* 词汇层自身扩散 */
        diffusion_spread(ctx->vocab, cur_ids, cur_count,
                         vocab_scores, cur_decay, ctx->temperature);

        /* 跨到语义层（名称匹配） */
        if (ctx->semantic && sem_scores) {
            _cross_by_name(ctx->vocab, cur_ids, cur_count,
                            ctx->semantic, sem_scores, cur_decay * 0.4f);
            /* 语义层内部扩散 */
            int sem_active[64]; int sem_cnt = 0;
            for (int i = 0; i < sn && sem_cnt < 64; i++) {
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

        /* 更新当前活跃集 = 本轮得分最高的 K 个词（跳过虚词） */
        DiffusionCandidate tmp[DIFF_MAX_CANDIDATES];
        int tmp_cnt = 0;
        for (int i = 0; i < vn && tmp_cnt < DIFF_MAX_CANDIDATES; i++) {
            if (vocab_scores[i] > 0.001f && ctx->vocab->net->nodes[i]) {
                const char* concept = ctx->vocab->net->nodes[i]->concept;
                if (concept && is_function_word(concept)) continue;
                tmp[tmp_cnt].node_id     = i;
                tmp[tmp_cnt].total_score = vocab_scores[i];
                tmp[tmp_cnt].word        = concept;
                tmp_cnt++;
            }
        }
        qsort(tmp, tmp_cnt, sizeof(DiffusionCandidate), _cand_cmp);

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
        final[final_cnt].total_score    = vocab_scores[i];  /* 已含跨层回流 */
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

        /* 第一遍: 收集有效候选词 + POS 标注 */
        for (int i = 0; i < final_cnt && word_count < DIFF_MAX_SEQUENCE; i++) {
            if (final[i].used) continue;
            if (!final[i].word || strlen(final[i].word) < 2) continue;
            if (final[i].word[0] == '@' || final[i].word[0] == '?' ||
                (final[i].word[0] == 'H' && final[i].word[1] == 'e')) continue;
            if (is_function_word(final[i].word)) continue;

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

        /* 第二遍: 动词配价语法组装 */
        out = 0;

        /* 检测语言 */
        int lang_en = 0;
        for (int w = 0; w < word_count; w++) {
            if (word_buf[w] && word_buf[w][0]) {
                lang_en = ((unsigned char)word_buf[w][0] < 0x80);
                break;
            }
        }

        /* 动词配价驱动 */
        out = diffusion_assemble_grammar(word_buf, word_pos, word_count,
                                          output_words, max_output,
                                          lang_en, ctx);

        /* 语法组装失败 → 回退名词短语 */
        if (out < 1) {
            out = diffusion_assemble_scaffold_fallback(word_buf, word_pos,
                                                        word_count, output_words,
                                                        max_output, lang_en);
        }

        /* 最终回退: POS 优先级平铺 */
        if (out < 1) {
            const POSTag priority[] = {
                POS_NOUN, POS_VERB, POS_ADJ, POS_ADV,
                POS_NUM, POS_PRON, POS_PREP, POS_CONJ,
                POS_PARTICLE, POS_INTERJ, POS_UNKNOWN
            };
            const int pri_count = sizeof(priority) / sizeof(priority[0]);

            for (int pi = 0; pi < pri_count && out < max_output; pi++) {
                for (int w = 0; w < word_count && out < max_output; w++) {
                    if (!word_buf[w]) continue;
                    if (word_pos[w] != priority[pi]) continue;
                    output_words[out++] = word_buf[w];
                    word_buf[w] = NULL;
                }
            }
        }
    } else {
        /* ═══ 降级路径：模板连接词填充 (无 EmergentPOS 的回退) ═══ */
        int out_fallback = 0;
        const char* selected[DIFF_MAX_SEQUENCE];
        int sel = 0;

        for (int i = 0; i < final_cnt && out_fallback < max_output; i++) {
            if (final[i].used) continue;
            if (!final[i].word || strlen(final[i].word) < 2) continue;
            if (final[i].word[0] == '@' || final[i].word[0] == '?' ||
                (final[i].word[0] == 'H' && final[i].word[1] == 'e')) continue;
            if (is_function_word(final[i].word)) continue;

            int inhibited = 0;
            for (int s = 0; s < sel; s++) {
                if (final[i].word && selected[s] &&
                    strcmp(final[i].word, selected[s]) == 0) {
                    inhibited = 1; break;
                }
            }
            if (inhibited) continue;

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
