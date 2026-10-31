/* tools/qa_bigram.c — 把 QA 语料灌进转移表（词级 bigram 条件概率）
 *
 * 用法: ./qa_bigram <state_file> <pipe_file> [output_transitions.bin]
 *
 * 为什么要有这个（2026-10-27）：
 *   `transition_model` 这个模块早就写好了，设计说明就是「走边生成 + bigram
 *   条件概率 P(next|prev)」，接口里连 `tm_sample_next` / `tm_chain_logprob`
 *   （= Σ log P，即句子的连贯度）都有。但它一直是张空表：
 *     · 写侧 `tm_observe_*` 全仓零调用（10-27 才在 gateway_learn 接上）
 *     · 读侧 `tm_sample_next` 只在旧的 dialog_system 里用，主路径不经过
 *   实测线上 `tm_pairs` = 2,083（只从几轮对话学到），transitions.bin 25 KB，
 *   而语料有 907,214 行 —— 差六个数量级。
 *
 *   所以「沿边逐步生成、不靠密集计算」这条路走不通，不是因为方法不对，
 *   是因为表里没东西。这个工具负责把语料变成表。
 *
 * 切分口径：
 *   · 用【现有词表】做最长匹配（4→3→2→1 字），词表就是线上那张，不另造；
 *   · 遇 ASCII 空白/标点、CJK 标点、非汉字，一律【断链】—— 不跨句学，
 *     否则「。下一句」会把两句粘成一个假转移；
 *   · 未登录的孤立汉字跳过（不产生 token，也就不断链之外的影响）；
 *   · 问句与答案分开喂（不学「问尾→答首」的对话转移；那不是句内连贯，
 *     混进来会污染 P(next|prev) 的语义。要做对话转移应另开一张表）。
 */
#include "pivotmind_paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "multi_topology.h"
#include "huarong_topology.h"
#include "transition_model.h"
#include "corpus_filter.h"

/* ── 虚词过滤（CX-39 · 2026-10-31）───────────────────────────
 *
 * 为什么要有：实测转移表 top-8 后续大量是虚词（和/的/来/去/或者/等等/中/里），
 * 因为虚词在任何语料里都高频相邻。而扩散候选池已经过 `is_function_word()`
 * 全剔（100+ 虚词），两边过滤口径相反 ⇒ 走边时「池内命中 0」。
 *
 * 治本做法：**灌表时就不记含虚词的 bigram** —— 让转移表只含「实词→实词」，
 * 和候选池同口径。遇到虚词**断链**（不跨过它记 bigram），而不是跳过它继续连。
 *
 * 词表来源：从 diffusion.c 的 is_function_word 里摘最高频的那批。
 * 不求全（那有 100+ 条），只覆盖真正会把 top-8 占满的。
 * 环境变量 PIVOTMIND_WBIGRAM_KEEP_FW=1 可关掉这个过滤做对照。
 */
static int skip_funcword(const char* w) {
    static int _keep = -1;
    if (_keep < 0) {
        const char* e = getenv("PIVOTMIND_WBIGRAM_KEEP_FW");
        _keep = (e && e[0] == '1') ? 1 : 0;
    }
    if (_keep) return 0;
    if (!w || !w[0]) return 1;

    /* 单字高频虚词（这是把 top-8 占满的主力） */
    static const char* ONE[] = {
        "\u7684","\u4e86","\u662f","\u5728","\u6709","\u6765","\u53bb","\u628a","\u88ab",
        "\u8ba9","\u7ed9","\u7740","\u8fc7","\u5f97","\u548c","\u4e0e","\u53ca","\u6216",
        "\u5c31","\u90fd","\u4e5f","\u8fd8","\u53c8","\u5f88","\u592a","\u66f4","\u6700",
        "\u4e0d","\u6ca1","\u4e0a","\u4e0b","\u4e2d","\u91cc","\u5916","\u524d","\u540e",
        "\u5bf9","\u4ece","\u5411","\u4e8e","\u4e3a","\u4ee5","\u56e0","\u6240","\u800c",
        "\u5219","\u4e14","\u5e76","\u5982","\u82e5","\u5373","\u4e4b","\u5176","\u6b64",
        "\u90a3","\u8fd9","\u54ea","\u4ec0","\u4e48","\u5462","\u5427","\u554a","\u5417",
        "\u4e00","\u4e8c","\u4e09","\u56db","\u4e94","\u516d","\u4e03","\u516b","\u4e5d","\u5341",
        NULL
    };
    /* 双字虚词 */
    static const char* TWO[] = {
        "\u6216\u8005","\u7b49\u7b49","\u4f46\u662f","\u56e0\u4e3a","\u6240\u4ee5",
        "\u5982\u679c","\u867d\u7136","\u8fd8\u662f","\u5c31\u662f","\u4e0d\u662f",
        "\u8fd9\u4e2a","\u90a3\u4e2a","\u4ec0\u4e48","\u600e\u4e48","\u4e3a\u4ec0",
        "\u4e00\u4e2a","\u4e00\u4e9b","\u4e00\u79cd","\u5df2\u7ecf","\u53ef\u4ee5",
        "\u5e94\u8be5","\u9700\u8981","\u8fd9\u6837","\u90a3\u6837","\u540c\u65f6",
        NULL
    };
    size_t L = strlen(w);
    if (L == 3) {
        for (int i = 0; ONE[i]; i++) if (strcmp(w, ONE[i]) == 0) return 1;
    } else if (L == 6) {
        for (int i = 0; TWO[i]; i++) if (strcmp(w, TWO[i]) == 0) return 1;
    }
    return 0;
}
   /* 与重训同一套语料闸门（繁体/文言/无实义） */
#include "common.h"

/* ---------- 无锁局部哈希：token → node_id ----------
 * 不能用 huarong_net_find_concept：它每次调用都加读写锁，
 * 1500 万 token × 最多 4 次探测 = 6000 万次加锁，慢到不可用。 */
#define LH_CAP 131072u          /* 2^17，负载 ≈ 35704/131072 = 27% */
typedef struct { char name[16]; int id; } LHEntry;
static LHEntry g_lh[LH_CAP];
static long g_lh_used = 0;

static unsigned int _djb2(const char* s) {
    unsigned int h = 5381u;
    while (*s) h = ((h << 5) + h) + (unsigned char)(*s++);
    return h;
}

static int lh_find(const char* s) {
    unsigned int h = _djb2(s) & (LH_CAP - 1);
    while (g_lh[h].name[0]) {
        if (strcmp(g_lh[h].name, s) == 0) return g_lh[h].id;
        h = (h + 1) & (LH_CAP - 1);
    }
    return -1;
}

static void lh_insert(const char* s, int id) {
    if (!s || !s[0] || strlen(s) >= sizeof(g_lh[0].name)) return;
    unsigned int h = _djb2(s) & (LH_CAP - 1);
    while (g_lh[h].name[0]) {
        if (strcmp(g_lh[h].name, s) == 0) return;
        h = (h + 1) & (LH_CAP - 1);
    }
    snprintf(g_lh[h].name, sizeof(g_lh[h].name), "%s", s);
    g_lh[h].id = id;
    g_lh_used++;
}

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

/* ---------- 切分 + 灌计数 ---------- */
static TransitionModel* g_tm;
static long g_lines = 0, g_tokens = 0, g_pairs = 0, g_breaks = 0, g_filtered = 0;
static char g_unseen[64][16];
static int  g_unseen_n = 0;

/* 把一个 UTF-8 字符的长度与码点取出来；非 3 字节中文一律当「断链」 */
static int _utf8_next(const char* p, unsigned int* cp) {
    unsigned char c = (unsigned char)*p;
    int len;
    if (c < 0x80) { *cp = c; return 1; }
    len = (c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4);
    if (len == 3) {
        *cp = ((unsigned int)(p[0] & 0x0F) << 12)
            | ((unsigned int)(p[1] & 0x3F) << 6)
            |  (unsigned int)(p[2] & 0x3F);
    } else {
        *cp = 0xFFFFFFFFu;
    }
    return len;
}

static void _flush(int* ids, int* n) {
    if (*n > 1) {
        tm_observe_run(g_tm, (const int32_t*)ids, *n);
        g_pairs += (*n - 1);
    }
    g_tokens += *n;
    *n = 0;
}

/* 最长匹配切分一段文本并灌 bigram；标点/空白/未登录 一律断段 */
static void feed_text(const char* s) {
    int ids[1024];
    int n = 0;
    const char* p = s;
    while (*p) {
        unsigned int cp = 0;
        int len = _utf8_next(p, &cp);
        if (len != 3 || cp < 0x4E00u || cp > 0x9FFFu) {   /* ASCII / 标点 / 非汉字 */
            if (n > 0) { _flush(ids, &n); g_breaks++; }
            p += (len > 0 ? len : 1);
            continue;
        }
        /* 汉字：4→3→2→1 最长匹配 */
        int matched = 0;
        for (int L = 4; L >= 1; L--) {
            int need = L * 3;
            int avail = 0;
            const char* q = p;
            while (avail < need && *q) {
                unsigned int c2 = 0;
                int l2 = _utf8_next(q, &c2);
                if (l2 != 3 || c2 < 0x4E00u || c2 > 0x9FFFu) break;
                avail += 3;
                q += 3;
            }
            if (avail < need) continue;
            char buf[13];
            memcpy(buf, p, (size_t)need);
            buf[need] = 0;
            int id = lh_find(buf);
            if (id >= 0) {
                if (skip_funcword(buf)) {
                    /* 虚词：**断链**——先把当前链冲掉，然后不把它记进序列。
                     * 这样「和/的/来」这些就不会出现在任何 bigram 里。 */
                    _flush(ids, &n);
                    p += need;
                    matched = 1;
                    break;
                }
                if (n >= (int)(sizeof(ids)/sizeof(ids[0])) - 1) _flush(ids, &n);
                ids[n++] = id;
                p += need;
                matched = 1;
                break;
            }
        }
        if (!matched) {
            /* 未登录孤立字：跳过（不产生 token），并记样本便于诊断词表覆盖 */
            if (cp != 0xFFFFFFFFu && g_unseen_n < 64) {
                int dup = 0;
                for (int k = 0; k < g_unseen_n; k++)
                    if (memcmp(g_unseen[k], p, 3) == 0) { dup = 1; break; }
                if (!dup) {
                    char one[4] = {0};
                    memcpy(one, p, 3);
                    snprintf(g_unseen[g_unseen_n++], 16, "%s", one);
                }
            }
            p += 3;
        }
    }
    if (n > 0) _flush(ids, &n);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "用法: %s <state_file> <pipe_file> [out_transitions.bin]\n", argv[0]);
        return 2;
    }
    const char* state = argv[1];
    const char* pipe  = argv[2];

    MasterTopology* master = load_master(state);
    if (!master) { fprintf(stderr, "加载失败: %s\n", state); return 1; }

    SubTopology* vocab = NULL;
    for (int t = 0; t < master->sub_topo_count; t++)
        if (master->sub_topologies[t] && master->sub_topologies[t]->type == TOPO_VOCABULARY) {
            vocab = master->sub_topologies[t]; break;
        }
    if (!vocab || !vocab->net) { fprintf(stderr, "vocab 拓扑缺失\n"); return 1; }

    /* 建局部哈希 + 统计词表构成（多字词才有意义，单字也能当 token） */
    int n_single = 0, n_multi = 0;
    for (int i = 0; i < vocab->net->node_count; i++) {
        ReasoningNode* n = vocab->net->nodes[i];
        if (!n || !n->concept) continue;
        lh_insert(n->concept, n->node_id);
        size_t L = strlen(n->concept);
        if (L == 3) n_single++; else if (L >= 6) n_multi++;
    }
    printf("[bigram] 词表: %d 单字 / %d 多字词 · 局部哈希 %ld 项\n",
           n_single, n_multi, g_lh_used);

    g_tm = tm_create();
    if (!g_tm) { fprintf(stderr, "tm_create 失败\n"); master_topology_destroy(master); return 1; }

    FILE* f = fopen(pipe, "rb");
    if (!f) { fprintf(stderr, "打不开语料: %s\n", pipe); return 1; }

    static char line[65536];
    time_t t0 = time(NULL);
    while (fgets(line, sizeof(line), f)) {
        g_lines++;
        size_t L = strlen(line);
        while (L > 0 && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
        if (L == 0) continue;
        char* bar = strchr(line, '|');
        if (bar) {
            *bar = 0;
            /* 与 qa_retrain 同一套闸门：繁体/文言/无实义整行跳过。
             * 不过闸的话会把繁体字当成「未登录字」跳过，白扔断链机会，
             * 而且那些行本来就不该进模型（线上词表里没有繁体）。 */
            {
                int why = 0;
                if (!cf_accept_qa(line, bar + 1, &why)) { g_filtered++; continue; }
            }
            feed_text(line);        /* 问句 */
            feed_text(bar + 1);     /* 答案（分开喂，不学「问尾→答首」） */
        } else {
            int why = 0;
            if (!cf_accept_qa(line, line, &why)) { g_filtered++; continue; }
            feed_text(line);
        }
        if ((g_lines % 100000) == 0)
            printf("[bigram] 进度 %ld 行 · token %ld · 对 %ld · %lds\n",
                   g_lines, g_tokens, g_pairs, (long)(time(NULL) - t0));
    }
    fclose(f);

    printf("[bigram] 读完: %ld 行 · token %ld · 记入 %ld 对 · 断段 %ld · %lds\n",
           g_lines, g_tokens, g_pairs, g_breaks, (long)(time(NULL) - t0));
    printf("[bigram] 转移表: %d 个 (prev,next) 唯一对\n", tm_pair_count(g_tm));
    if (g_unseen_n > 0) {
        printf("[bigram] 未登录单字样本: ");
        for (int i = 0; i < g_unseen_n && i < 20; i++) printf("%s ", g_unseen[i]);
        printf("\n");
    }

    char outbuf[1024];
    const char* outp = (argc >= 4) ? argv[3] : NULL;
    if (!outp) {
        snprintf(outbuf, sizeof(outbuf), "%s", pm_file(PM_FILE_TRANSITIONS));
        outp = outbuf;
    }
    if (tm_save(g_tm, outp) == 0) printf("[bigram] 已存盘: %s\n", outp);
    else                          printf("[bigram] 存盘失败: %s\n", outp);

    tm_destroy(g_tm);
    master_topology_destroy(master);
    return 0;
}
