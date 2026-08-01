/**
 * feed_cli.c — 手机端灌书 CLI（复刻 /learn 温和管道，单线程无 OpenMP）
 * 用法: ./feed_cli <state.dat> <语料文件...>
 * 行为: 逐行 CJK 分词 → 词汇拓扑建节点/相邻词边 → 概念学习 → 保存状态
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multi_topology.h"
#include "dict_loader.h"

/* 复制自 demos/pivotmind_gateway.c:860 — CJK 字符间插空格 */
static void _cjk_insert_spaces(const char* src, char* dst, int dst_sz) {
    int wi = 0;
    for (const char* p = src; *p && wi < dst_sz - 4; ) {
        unsigned char c = (unsigned char)*p;
        int blen = 1;
        if (c >= 0xE0 && c <= 0xEF) blen = 3;
        else if (c >= 0xC0 && c <= 0xDF) blen = 2;
        if (blen > 1 && wi + blen + 2 >= dst_sz) break;
        if (blen == 3 && wi > 0 && (unsigned char)dst[wi-1] != ' ' && wi + 1 < dst_sz) {
            dst[wi++] = ' ';
        }
        for (int b = 0; b < blen && p[b] && wi < dst_sz - 1; b++)
            dst[wi++] = p[b];
        dst[wi] = '\0';
        p += blen;
    }
    if (wi < dst_sz) dst[wi] = '\0';
}

/* 复制自 demos/pivotmind_gateway.c:886 — 分词 + 建节点 + 相邻词建边
 * v0.6: Hebbian 累积（共现频率进边权）+ 中英分流
 *   - 中文单字 → 字拓扑（vocab），字-字边权随共现次数递增
 *   - 英文整词 → 概念拓扑（concept），词-词边同样累积
 *   - 中英链分离：跨语言不建边（架构原则：跨语言边禁建）
 */
#define HEBBIAN_DELTA 0.05f

/* v0.5.7: 词级虚字表——只放"绝对虚字"（的/了/是/在/和/与/或/也/都/就/
 * 很/不/要/会/能/吗/呢/吧/啊/这/那/有/我/你/他/她/它/们）。
 * 用户纠正：词级审视不能照搬字级虚字表——"过去/将来/上面/儿子"等
 * 真词含"过/来/去/上/下/子/儿"（字级虚字）但整体是实义词，必须保留。
 * 只有"绝对虚"组合（"是流"类）才跳过。 */
static int cc_is_void_char(const char* s) {
    static const char* voids[] = {"的","了","是","在","和","与","或","也","都",
        "就","很","不","要","会","能","吗","呢","吧","啊","这","那","有","我",
        "你","他","她","它","们",
        NULL};
    if (!s || !s[0]) return 0;
    for (int i = 0; voids[i]; i++)
        if (strcmp(s, voids[i]) == 0) return 1;
    return 0;
}

static int is_ascii_token(const char* s) {
    return (unsigned char)s[0] < 0x80;
}

/* 英文词过滤：纯字母（允许内部 - '），长度 2-24
 * 排除 ISBN/URL/数字串等版权页垃圾（CIP、http://、978-7-...） */
static int is_valid_word_token(const char* s) {
    size_t len = strlen(s);
    if (len < 2 || len > 24) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) continue;
        if ((c == '-' || c == '\'') && i > 0 && i < len - 1) continue;
        return 0;
    }
    return 1;
}

/* 线性查找已有边（feed_cli 单线程，不需要 conn_hash） */
static int find_edge_idx(ReasoningNode* n, ReasoningNode* target) {
    if (!n || !n->edges) return -1;
    for (int i = 0; i < n->edge_count; i++) {
        if (n->edges[i].target == target) return i;
    }
    return -1;
}

/* Hebbian 单向边：已有边权按饱和曲线递增（越接近 1.0 增量越小，
 * 高频共现和低频共现拉开差距），否则新建 0.4 */
static void hebbian_edge(HuarongTopologyNet* net, ReasoningNode* from, ReasoningNode* to) {
    if (!net || !from || !to || from == to) return;
    int idx = find_edge_idx(from, to);
    if (idx >= 0) {
        float w = from->edges[idx].weight;
        from->edges[idx].weight = w + HEBBIAN_DELTA * (1.0f - w);
        if (from->edges[idx].weight > 1.0f)
            from->edges[idx].weight = 1.0f;
    } else {
        huarong_net_add_connection(net, from->node_id, to->node_id, 0.4f);
    }
}

static int learn_tokens(SubTopology* vocab, SubTopology* concept,
                        const char* text, int* p_prev_v, int* p_prev_c) {
    if (!vocab || !vocab->net || !text || !text[0]) return 0;
    int text_len = (int)strlen(text);
    int copy_sz = text_len * 4 / 3 + 32;
    if (copy_sz < 256) copy_sz = 256;
    char* copy = (char*)malloc((size_t)copy_sz);
    if (!copy) return 0;
    _cjk_insert_spaces(text, copy, copy_sz);
    char* tok = strtok(copy, " \t\n\r。，！？、；：\"\"''（）《》…—");
    int added = 0;
    while (tok) {
        if (strlen(tok) >= 2) {
            /* 中英分流：英文词（含字母）→ 概念拓扑；中文单字 → 字拓扑 */
            int is_en = is_ascii_token(tok);
            if (is_en && !is_valid_word_token(tok)) {
                /* 数字串/ISBN/URL 等无效 token：跳过（不建节点不建边） */
                tok = strtok(NULL, " \t\n\r。，！？、；：\"\"''（）《》…—");
                continue;
            }
            SubTopology* topo = is_en ? concept : vocab;
            int* p_prev = is_en ? p_prev_c : p_prev_v;
            if (!topo || !topo->net) { tok = strtok(NULL, " \t\n\r。，！？、；：\"\"''（）《》…—"); continue; }

            int nid = huarong_net_find_concept(topo->net, tok);
            if (nid < 0 && (size_t)topo->net->node_count < topo->net->max_nodes) {
                nid = huarong_net_dynamic_add_node(topo->net, tok, NULL, 0);
                if (nid >= 0) {
                    added++;
                    node_hash_add(topo->node_hash, topo->net->nodes[nid]);
                }
            }
            if (nid >= 0) {
                topo->net->nodes[nid]->activation += 0.1f;
                if (*p_prev >= 0 && *p_prev != nid) {
                    ReasoningNode* prev = topo->net->nodes[*p_prev];
                    ReasoningNode* cur = topo->net->nodes[nid];
                    /* 单向语序边 + Hebbian 累积：
                     * 语序方向保留在边权里（衣服→"衣→服"强），
                     * 词巩固用高频方向定词序，防"服衣"倒序词 */
                    hebbian_edge(topo->net, prev, cur);
                }
                *p_prev = nid;

                /* 词级语义场（v0.5.7）：2 字窗口配对 + 词-词共现边。
                 * 每 2 个连续中文字配成一个窗口词（时间/早晨…）——
                 * 窗口间共现建边（时间→早晨）；错位窗口（间早）不产生。
                 * 虚字过滤：窗口词含虚字跳过；失败窗口不更新 prev_word，
                 * 实义词段直接共现。 */
                if (!is_en && concept && concept->net) {
                    static int   cn_seq = 0;      /* 连续中文字计数（每2一组） */
                    static char  word_win[8] = {0};
                    static char  prev_word[8] = {0};
                    cn_seq++;
                    if (cn_seq % 2 == 1) {
                        /* 奇位置 = 窗口首字 */
                        snprintf(word_win, sizeof(word_win), "%s", tok);
                    } else {
                        /* 偶位置 = 窗口尾字——完整窗口 cand */
                        char cand[8];
                        snprintf(cand, sizeof(cand), "%s%s", word_win, tok);
                        if (!cc_is_void_char(word_win) && !cc_is_void_char(tok)) {
                            int cnid = huarong_net_find_concept(concept->net, cand);
                            if (cnid < 0 && (size_t)concept->net->node_count < concept->net->max_nodes) {
                                cnid = huarong_net_dynamic_add_node(concept->net, cand, NULL, 0);
                                if (cnid >= 0)
                                    node_hash_add(concept->node_hash, concept->net->nodes[cnid]);
                            }
                            if (cnid >= 0) {
                                concept->net->nodes[cnid]->activation += 0.05f;
                                if (prev_word[0]) {
                                    int pnid = huarong_net_find_concept(concept->net, prev_word);
                                    if (pnid >= 0 && pnid != cnid) {
                                        ReasoningNode* pn = concept->net->nodes[pnid];
                                        ReasoningNode* cn = concept->net->nodes[cnid];
                                        hebbian_edge(concept->net, pn, cn);
                                    }
                                }
                                snprintf(prev_word, sizeof(prev_word), "%s", cand);
                            }
                        }
                    }
                }
            }
        }
        tok = strtok(NULL, " \t\n\r。，！？、；：\"\"''（）《》…—");
    }
    free(copy);
    return added;
}

int main(int argc, char** argv) {
    if (argc < 3) { printf("用法: %s <state.dat> <file...>\n", argv[0]); return 1; }
    const char* state_path = argv[1];
    setbuf(stdout, NULL);

    printf("创建认知网络...\n");
    MasterTopology* master = master_topology_create(11);
    master_add_sub_topology(master, TOPO_VOCABULARY, "", 100000, 10);
    master_add_sub_topology(master, TOPO_SEMANTIC,   "", 50000, 9);
    master_add_sub_topology(master, TOPO_EMOTION,    "", 4000, 8);
    master_add_sub_topology(master, TOPO_SYNTAX,     "", 1000, 7);
    master_add_sub_topology(master, TOPO_CONTEXT,    "", 1000, 6);
    master_add_sub_topology(master, TOPO_DOMAIN,     "", 1000, 5);
    master_add_sub_topology(master, TOPO_PRAGMA,     "", 1000, 4);
    master_add_sub_topology(master, TOPO_CULTURE,    "", 1000, 3);
    master_add_sub_topology(master, TOPO_CONCEPT,    "", 50000, 9);
    master_add_sub_topology(master, TOPO_MASTER,     "", 100, 0);
    master_add_sub_topology(master, TOPO_TEMPLATE,   "", 20000, 8);

    FILE* df = fopen("data/jieba_dict.txt", "r");
    if (df) {
        fclose(df);
        DictTable* d = dict_table_create(524288);
        if (dict_load_jieba(d, "data/jieba_dict.txt") > 0) {
            master->ext_dict = (struct ExternalDict*)d;
            printf("词典加载 OK\n");
        }
    } else {
        printf("无词典，逐字模式\n");
    }

    int loaded = master_load_state(master, state_path);
    printf("状态加载: %d 节点\n", loaded);
    if (loaded <= 10) { printf("x 状态异常\n"); master_topology_destroy(master); return 1; }

    SubTopology* vocab = NULL;
    SubTopology* concept = NULL;
    for (int t = 0; t < master->sub_topo_count; t++) {
        if (master->sub_topologies[t] && (int)master->sub_topologies[t]->type == TOPO_VOCABULARY) {
            vocab = master->sub_topologies[t];
        }
        if (master->sub_topologies[t] && (int)master->sub_topologies[t]->type == TOPO_CONCEPT) {
            concept = master->sub_topologies[t];
        }
    }
    if (!vocab || !vocab->net) { printf("x 无词汇拓扑\n"); master_topology_destroy(master); return 1; }
    if (!concept || !concept->net) { printf("x 无概念拓扑\n"); master_topology_destroy(master); return 1; }
    printf("词汇拓扑: %d 节点, 概念拓扑: %d 节点\n",
           vocab->net->node_count, concept->net->node_count);

    int total_added = 0;
    for (int a = 2; a < argc; a++) {
        FILE* f = fopen(argv[a], "r");
        if (!f) { printf("x 打不开 %s\n", argv[a]); continue; }
        char line[8192];
        int prev_v = -1, prev_c = -1;
        long lines = 0;
        int added = 0;
        int skip_header = 30;   /* 跳过版权页/CIP/目录（前 30 行噪声） */
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (skip_header > 0) { skip_header--; continue; }
            if (strlen(line) < 4) continue;
            int before = vocab->net->node_count + concept->net->node_count;
            learn_tokens(vocab, concept, line, &prev_v, &prev_c);
            added += (vocab->net->node_count + concept->net->node_count) - before;
            lines++;
            if (lines % 1000 == 0) {
                printf("  %s: %ld 行, +%d 节点, 总 %d\n", argv[a], lines, added, vocab->net->node_count);
            }
        }
        fclose(f);
        printf("OK %s: %ld 行, +%d 节点, 总 %d\n", argv[a], lines, added, vocab->net->node_count);
        total_added += added;
    }

    printf("保存状态 -> %s ...\n", state_path);
    int saved = master_save_state(master, state_path);
    printf("保存: %s (%d)\n", saved > 0 ? "OK" : "FAIL", saved);
    master_topology_destroy(master);
    printf("完成: +%d 节点\n", total_added);
    return 0;
}
