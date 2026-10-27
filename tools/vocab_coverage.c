/* tools/vocab_coverage.c — CX-08 探针
 * 用法:
 *   ./vocab_coverage <state_file> <wordlist>   逐词查命中（词表每行一个，# 注释）
 *   ./vocab_coverage <state_file> --stats      全量扫 vocab：脏词比例 + 边数分布
 * 复用 state_dump.c 的加载骨架（必须注册全部子拓扑，否则加载器丢节点）。
 */
#include "pivotmind_paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multi_topology.h"
#include "huarong_topology.h"
#include "common.h"
#include "word_prob.h"

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

static HuarongTopologyNet* net_of_type(MasterTopology* m, int type) {
    for (int t = 0; t < m->sub_topo_count; t++) {
        if (m->sub_topologies[t] && m->sub_topologies[t]->type == type)
            return m->sub_topologies[t]->net;
    }
    return NULL;
}

static ReasoningNode* get_node(HuarongTopologyNet* net, int id) {
    if (!net || id < 0 || id >= net->node_count) return NULL;
    return net->nodes[id];
}

typedef struct { int cjk, latin, digit, sym, bad, repl, ctrl; } Scan;

static Scan scan_name(const char* nm) {
    Scan s;
    memset(&s, 0, sizeof(s));
    const unsigned char* q = (const unsigned char*)nm;
    while (*q) {
        if (q[0] == 0xEF && q[1] == 0xBF && q[2] == 0xBD) { s.repl = 1; q += 3; continue; }
        if (q[0] < 0x20 || q[0] == 0x7F) { s.ctrl = 1; q++; continue; }
        if (q[0] < 0x80) {
            if ((q[0] >= 'a' && q[0] <= 'z') || (q[0] >= 'A' && q[0] <= 'Z')) s.latin++;
            else if (q[0] >= '0' && q[0] <= '9') s.digit++;
            else s.sym++;
            q++;
            continue;
        }
        int len = 0;
        if ((q[0] & 0xE0) == 0xC0) len = 2;
        else if ((q[0] & 0xF0) == 0xE0) len = 3;
        else if ((q[0] & 0xF8) == 0xF0) len = 4;
        else { s.bad = 1; q++; continue; }
        int ok = 1;
        for (int k = 1; k < len; k++) if ((q[k] & 0xC0) != 0x80) { ok = 0; break; }
        if (!ok) { s.bad = 1; q++; continue; }
        if (len == 3) {
            unsigned int cp = ((unsigned int)(q[0] & 0x0F) << 12)
                            | ((unsigned int)(q[1] & 0x3F) << 6)
                            |  (unsigned int)(q[2] & 0x3F);
            if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF)) s.cjk++;
            else s.sym++;
        } else {
            s.sym++;
        }
        q += len;
    }
    return s;
}

static int do_stats(HuarongTopologyNet* vocab) {
    int n_invalid = 0, n_repl = 0, n_ctrl = 0, n_sym = 0, n_latin = 0;
    int n_mixed = 0, n_c1 = 0, n_c2_6 = 0, n_c7 = 0, n_other = 0;
    int n_zero_edge = 0, n_total = 0, max_cjk = 0;
    unsigned long long sum_edge = 0;
    static char longest[8][64];
    int longest_rank[8];
    for (int i = 0; i < 8; i++) { longest[i][0] = '\0'; longest_rank[i] = -1; }

    for (int i = 0; i < vocab->node_count; i++) {
        ReasoningNode* nd = vocab->nodes[i];
        if (!nd || !nd->concept) continue;
        n_total++;
        if (nd->edge_count == 0) n_zero_edge++;
        if (nd->edge_count > 0) sum_edge += (unsigned long long)nd->edge_count;

        Scan sc = scan_name(nd->concept);
        if (sc.bad) n_invalid++;
        if (sc.repl) n_repl++;
        if (sc.ctrl) n_ctrl++;
        if (sc.cjk == 0 && sc.latin > 0) n_latin++;
        else if (sc.cjk > 0 && (sc.latin > 0 || sc.digit > 0)) n_mixed++;
        else if (sc.cjk == 0 && sc.sym > 0) n_sym++;
        else if (sc.cjk == 1) n_c1++;
        else if (sc.cjk >= 2 && sc.cjk <= 6) n_c2_6++;
        else if (sc.cjk >= 7) n_c7++;
        else n_other++;

        if (sc.cjk > max_cjk) max_cjk = sc.cjk;
        if (sc.cjk >= 6) {
            for (int k = 0; k < 8; k++) {
                if (longest_rank[k] < sc.cjk) {
                    for (int j = 7; j > k; j--) {
                        memcpy(longest[j], longest[j-1], 64);
                        longest_rank[j] = longest_rank[j-1];
                    }
                    strncpy(longest[k], nd->concept, 63);
                    longest[k][63] = '\0';
                    longest_rank[k] = sc.cjk;
                    break;
                }
            }
        }
    }
    if (n_total == 0) { printf("no vocab nodes\n"); return 1; }

    fprintf(stderr, "[VOCAB-STATS] total=%d zero_edge=%d(%.1f%%) avg_edge=%.1f max_cjk=%d\n",
            n_total, n_zero_edge, 100.0 * n_zero_edge / n_total,
            (double)sum_edge / n_total, max_cjk);
    fprintf(stderr, "[分类] 单字=%d(%.1f%%) 2-6字=%d(%.1f%%) 7字以上=%d(%.1f%%)\n",
            n_c1, 100.0 * n_c1 / n_total, n_c2_6, 100.0 * n_c2_6 / n_total,
            n_c7, 100.0 * n_c7 / n_total);
    fprintf(stderr, "[脏] 非法UTF8=%d U+FFFD=%d 控制字节=%d 纯符号=%d 纯拉丁=%d 中英混=%d 其他=%d\n",
            n_invalid, n_repl, n_ctrl, n_sym, n_latin, n_mixed, n_other);
    for (int k = 0; k < 8; k++)
        if (longest_rank[k] >= 0)
            fprintf(stderr, "[最长%d汉字] %s\n", longest_rank[k], longest[k]);
    return 0;
}

int main(int argc, char** argv) {
    const char* state_path = (argc > 1) ? argv[1] : pm_file(PM_FILE_STATE);
    const char* list_path  = (argc > 2) ? argv[2] : "-";

    MasterTopology* master = load_master(state_path);
    if (!master) { fprintf(stderr, "FAIL: load %s\n", state_path); return 1; }

    HuarongTopologyNet* vocab   = net_of_type(master, TOPO_VOCABULARY);
    HuarongTopologyNet* concept = net_of_type(master, TOPO_CONCEPT);
    HuarongTopologyNet* sem     = net_of_type(master, TOPO_SEMANTIC);
    if (!vocab) { fprintf(stderr, "FAIL: no vocab topo\n"); return 1; }
    fprintf(stderr, "[probe] vocab=%d nodes, concept=%d, sem=%d\n",
            vocab->node_count, concept ? concept->node_count : -1,
            sem ? sem->node_count : -1);

    if (strcmp(list_path, "--stats") == 0) {
        int rc = do_stats(vocab);
        master_topology_destroy(master);
        return rc;
    }

    /* --prob:<词>  打印该词的语料统计（P / 左右熵 / 生成增益）——
     * 与生成侧同一口径（word_prob），用于解释「为什么这个词被抬或被压」。 */
    if (strncmp(list_path, "--prob:", 7) == 0) {
        const char* w = list_path + 7;
        if (strncmp(w, "en:", 3) == 0) w += 3;
        printf("词\t%s\n", w);
        printf("次数\t%llu\n", (unsigned long long)wp_count(w));
        printf("P\t%.8f\n", wp_p(w));
        printf("Pnorm\t%.4f\n", (double)wp_pnorm(w));
        printf("左熵\t%.3f\n", wp_entropy(w, 0));
        printf("右熵\t%.3f\n", wp_entropy(w, 1));
        printf("min熵\t%.3f\n", wp_min_entropy(w));
        printf("增益\t%.4f\n", (double)wp_gain(w));
        printf("合法词\t%d\n", wp_is_wordlike(w, 2.0, 5));
        { int tk = 0, pr = 0; unsigned long long tot = 0;
          wp_stats(&tk, &pr, &tot);
          fprintf(stderr, "[prob] tokens=%d pairs=%d total=%llu\n", tk, pr, tot); }
        master_topology_destroy(master);
        return 0;
    }

    /* --nbr:<词>  打印该词在 vocab 里的邻居构成（判「扩散被垃圾邻居带偏」） */
    if (strncmp(list_path, "--nbr:", 6) == 0) {
        const char* w = list_path + 6;
        ReasoningNode* nd = get_node(vocab, huarong_net_find_concept(vocab, w));
        if (!nd) { fprintf(stderr, "[nbr] 不在 vocab: %s\n", w); master_topology_destroy(master); return 1; }
        fprintf(stderr, "[nbr] %s  edge_count=%d cap=%d cooled=%d\n",
                w, nd->edge_count, nd->edge_capacity, nd->is_cooled);
        int cjk_n = 0, latin_n = 0, sym_n = 0, bad_n = 0, zero_w = 0;
        int len1 = 0, len2_6 = 0, len7 = 0;
        for (int i = 0; i < nd->edge_count; i++) {
            ReasoningNode* t = nd->edges[i].target;
            if (!t || !t->concept) continue;
            Scan sc = scan_name(t->concept);
            const char* cls = sc.bad ? "非法" : (sc.repl ? "乱码" :
                              (sc.cjk > 0 && sc.latin == 0 && sc.digit == 0) ? "汉字" :
                              (sc.cjk == 0 && sc.latin > 0) ? "拉丁" : "混合/符号");
            if (!strcmp(cls, "汉字")) cjk_n++;
            else if (!strcmp(cls, "拉丁")) latin_n++;
            else if (sc.bad || sc.repl) bad_n++;
            else sym_n++;
            if (nd->edges[i].weight <= 0.0f) zero_w++;
            if (sc.cjk == 1) len1++;
            else if (sc.cjk >= 2 && sc.cjk <= 6) len2_6++;
            else if (sc.cjk >= 7) len7++;
            if (i < 40)
                printf("%s\t%s\tw=%.3f\t边数=%d\n", cls, t->concept, nd->edges[i].weight, t->edge_count);
        }
        fprintf(stderr, "[nbr汇总] 汉字=%d 拉丁=%d 混合/符号=%d 非法/乱码=%d 权重<=0=%d 单字=%d 2-6字=%d 7字+=%d\n",
                cjk_n, latin_n, sym_n, bad_n, zero_w, len1, len2_6, len7);
        master_topology_destroy(master);
        return 0;
    }

    FILE* in = (strcmp(list_path, "-") == 0) ? stdin : fopen(list_path, "rb");
    if (!in) { fprintf(stderr, "FAIL: open %s\n", list_path); return 1; }

    char line[512];
    int total = 0, hit_vocab = 0, hit_any = 0, zero_edge = 0;
    while (fgets(line, sizeof(line), in)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        char* e = p + strlen(p);
        while (e > p && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = '\0';
        if (!*p) continue;

        ReasoningNode* nv  = get_node(vocab,   huarong_net_find_concept(vocab, p));
        ReasoningNode* nc2 = concept ? get_node(concept, huarong_net_find_concept(concept, p)) : NULL;
        ReasoningNode* ns  = sem     ? get_node(sem,     huarong_net_find_concept(sem, p))     : NULL;

        int hits = (nv ? 1 : 0) + (nc2 ? 1 : 0) + (ns ? 1 : 0);
        total++;
        if (nv) hit_vocab++;
        if (hits > 0) hit_any++;
        if (nv && nv->edge_count == 0 && (!nc2 || nc2->edge_count == 0)) zero_edge++;

        printf("%s\t%d\t%d\t%d\t%d\n", p, hits,
               nv  ? nv->edge_count  : -1,
               nc2 ? nc2->edge_count : -1,
               ns  ? ns->edge_count  : -1);
    }
    if (in != stdin) fclose(in);

    fprintf(stderr, "[SUMMARY] total=%d hit_vocab=%d(%.1f%%) hit_any=%d(%.1f%%) zero_edge=%d\n",
            total, hit_vocab, total ? 100.0 * hit_vocab / total : 0.0,
            hit_any, total ? 100.0 * hit_any / total : 0.0, zero_edge);
    master_topology_destroy(master);
    return 0;
}
