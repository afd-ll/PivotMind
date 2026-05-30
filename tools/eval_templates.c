/**
 * @file eval_templates.c
 * @brief P2 Task 9: 模板投票效果端到端评估
 *
 * 用法:
 *   ./build/bin/eval_templates [state_file]
 *
 * 默认从 pivotmind_state.dat 加载状态，
 * 在 50 个标准测试问题下对比模板投票前后的走边输出质量。
 *
 * 评估维度:
 *   - 流畅度: 路径长度 / 最大步数
 *   - 无回声度: 路径中无重复节点的比例
 *   - 多样性:  不同测试间输出节点的唯一性
 *   - 语义紧度: 路径中相邻节点特征余弦相似度均值
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "multi_topology.h"
#include "huarong_topology.h"
#include "path_encoding.h"
#include "template_builder.h"
#include "common.h"

#define MAX_WALK_STEPS 20
#define NUM_SEEDS      50

typedef struct {
    int    path_len;
    float  fluency;          /* 路径长度/MAX_WALK_STEPS */
    float  no_echo;          /* 唯一步数/总步数 */
    float  sem_cohesion;     /* 相邻节点特征余弦均值 */
    float  unique_nodes;     /* 唯一点数 */
    char   path_str[256];
} WalkMetrics;

static const char* node_concept_s(ReasoningNode* node, int max_len) {
    static char buf[32];
    if (!node || !node->concept) return "?";
    int len = (int)strlen(node->concept);
    if (len <= max_len) return node->concept;
    memcpy(buf, node->concept, (size_t)max_len);
    buf[max_len] = '\0';
    return buf;
}

/* 评估一次走边 */
static WalkMetrics eval_single_walk(SubTopology* vocab, MasterTopology* master,
                                    int start_id, int use_tpl, int nc) {
    WalkMetrics m;
    memset(&m, 0, sizeof(m));

    int path[MAX_WALK_STEPS];
    float scores[MAX_WALK_STEPS];
    int bms = (nc + 7) / 8;
    unsigned char* vis = (unsigned char*)calloc((size_t)bms, 1);
    if (!vis) return m;

    ReasoningNode* sn = vocab->net->nodes[start_id];
    float saved_act = 0.0f;
    if (sn) { saved_act = sn->activation; sn->activation = 0.8f; }

    master->use_template_voting = use_tpl;
    int pl = topology_walk_greedy(vocab, start_id, path, scores,
                                  MAX_WALK_STEPS, vis, 1.0f, master, NULL);
    master->use_template_voting = 0;
    if (sn) sn->activation = saved_act;
    free(vis);

    if (pl < 2) return m;

    m.path_len = pl;
    m.fluency = (float)pl / (float)MAX_WALK_STEPS;

    /* 无回声度: 检查重复节点 */
    int unique = 0;
    for (int i = 0; i < pl; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++) {
            if (path[i] == path[j]) { dup = 1; break; }
        }
        if (!dup) unique++;
    }
    m.no_echo = (float)unique / (float)pl;
    m.unique_nodes = (float)unique;

    /* 语义紧度: 相邻节点特征余弦相似度均值 */
    float sim_sum = 0.0f;
    int sim_count = 0;
    ReasoningNode** nodes = vocab->net->nodes;
    for (int i = 0; i < pl - 1; i++) {
        ReasoningNode* na = (path[i] >= 0 && path[i] < nc) ? nodes[path[i]] : NULL;
        ReasoningNode* nb = (path[i+1] >= 0 && path[i+1] < nc) ? nodes[path[i+1]] : NULL;
        if (na && na->features && nb && nb->features) {
            float dot = 0.0f, na2 = 0.0f, nb2 = 0.0f;
            for (int d = 0; d < NODE_FEATURE_DIM; d++) {
                dot += na->features[d] * nb->features[d];
                na2 += na->features[d] * na->features[d];
                nb2 += nb->features[d] * nb->features[d];
            }
            float denom = sqrtf(na2) * sqrtf(nb2);
            if (denom > 1e-10f) sim_sum += dot / denom;
            sim_count++;
        }
    }
    m.sem_cohesion = (sim_count > 0) ? (sim_sum / (float)sim_count) : 0.0f;

    /* 路径字符串 */
    int pos = 0;
    for (int i = 0; i < pl && pos < 240; i++) {
        ReasoningNode* nd = (path[i] < nc) ? nodes[path[i]] : NULL;
        const char* c = node_concept_s(nd, 6);
        int cl = (int)strlen(c);
        if (pos + cl + 2 < 240) {
            if (i > 0) { m.path_str[pos++] = '-'; m.path_str[pos++] = '>'; }
            memcpy(m.path_str + pos, c, cl); pos += cl;
        }
    }
    m.path_str[pos] = '\0';

    return m;
}

/* 按连接数排名的节点选择 */
typedef struct { int id; int conn; } NodeRank;
static int rank_cmp(const void* a, const void* b) {
    return ((const NodeRank*)b)->conn - ((const NodeRank*)a)->conn;
}

int main(int argc, char** argv) {
    const char* state_file = "pivotmind_state.dat";
    if (argc > 1) state_file = argv[1];

    printf("========================================\n");
    printf("  PivotMind Template Evaluation\n");
    printf("  P2 Task 9: End-to-end comparison\n");
    printf("========================================\n\n");

    /* 创建主拓扑 */
    MasterTopology* master = master_topology_create(16);
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", 6000, 10);
    master_add_sub_topology(master, TOPO_SEMANTIC, "语义拓扑", 2000, 9);
    master_add_sub_topology(master, TOPO_EMOTION, "情绪拓扑", 500, 8);
    master_add_sub_topology(master, TOPO_SYNTAX, "语法拓扑", 500, 7);
    master_add_sub_topology(master, TOPO_CONTEXT, "上下文拓扑", 500, 6);
    master_add_sub_topology(master, TOPO_DOMAIN, "领域拓扑", 500, 5);
    master_add_sub_topology(master, TOPO_PRAGMA, "语用拓扑", 500, 4);
    master_add_sub_topology(master, TOPO_CULTURE, "文化拓扑", 500, 3);
    master_add_sub_topology(master, TOPO_CONCEPT, "概念拓扑", 6000, 9);
    master_add_sub_topology(master, TOPO_MASTER, "主拓扑", 100, 0);   /* 占位：保证 TOPO_TEMPLATE 获得正确的 topo_id=10 */
    master_add_sub_topology(master, TOPO_TEMPLATE, "模板拓扑", 2000, 8);

    printf("Loading state: %s\n", state_file);
    if (master_load_state(master, state_file) < 0) {
        fprintf(stderr, "ERROR: Failed to load state\n");
        return 1;
    }

    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    if (!vocab || !vocab->net || vocab->net->node_count == 0) {
        fprintf(stderr, "ERROR: No vocabulary topology\n");
        return 1;
    }

    int nc = vocab->net->node_count;
    printf("Vocabulary nodes: %d\n", nc);
    printf("Cross links: %d\n", master->cross_link_count);

    SubTopology* tpl = master_get_sub_topology_by_type(master, TOPO_TEMPLATE);
    if (tpl && tpl->net) {
        printf("Template nodes: %d\n", tpl->net->node_count);
    }

    /* 自动构建模板（如有频率表数据） */
    if (tpl && tpl->net && tpl->net->node_count == 0 && master->freq_table &&
        master->freq_table->entry_count >= 50) {
        printf("\n→ 模板拓扑为空，需要先运行 template_build 工具构建模板\n");
        printf("  跳过自动构建，仅评估基础模式\n");
    }

    /* 选起点: 按连接数排名前 N 个 */
    NodeRank* ranks = (NodeRank*)malloc((size_t)nc * sizeof(NodeRank));
    int rk = 0;
    for (int i = 0; i < nc; i++) {
        ReasoningNode* nd = vocab->net->nodes[i];
        if (nd && nd->connection_count > 0) {
            ranks[rk].id = i;
            ranks[rk].conn = nd->connection_count;
            rk++;
        }
    }
    qsort(ranks, rk, sizeof(NodeRank), rank_cmp);
    int num_seeds = (NUM_SEEDS < rk) ? NUM_SEEDS : rk;

    /* 评估指标聚合 */
    float off_len = 0, off_echo = 0, off_sem = 0, off_unique = 0;
    float on_len = 0, on_echo = 0, on_sem = 0, on_unique = 0;
    int off_count = 0, on_count = 0;

    printf("\n--- Running evaluation (%d seeds) ---\n\n", num_seeds);

    for (int s = 0; s < num_seeds; s++) {
        int sid = ranks[s].id;

        /* 模板投票 OFF */
        WalkMetrics m_off = eval_single_walk(vocab, master, sid, 0, nc);

        /* 模板投票 ON */
        WalkMetrics m_on = eval_single_walk(vocab, master, sid, 1, nc);

        if (m_off.path_len >= 2) {
            off_len    += m_off.fluency;
            off_echo   += m_off.no_echo;
            off_sem    += m_off.sem_cohesion;
            off_unique += m_off.unique_nodes;
            off_count++;
        }
        if (m_on.path_len >= 2) {
            on_len    += m_on.fluency;
            on_echo   += m_on.no_echo;
            on_sem    += m_on.sem_cohesion;
            on_unique += m_on.unique_nodes;
            on_count++;
        }

        /* 显示前10条对比 */
        if (s < 10) {
            printf("Seed #%d: \"%s\"\n", s + 1,
                   vocab->net->nodes[sid] && vocab->net->nodes[sid]->concept
                   ? vocab->net->nodes[sid]->concept : "?");
            printf("  OFF [len=%d flu=%.2f echo=%.2f sem=%.2f] %s\n",
                   m_off.path_len, m_off.fluency, m_off.no_echo,
                   m_off.sem_cohesion, m_off.path_str);
            printf("  ON  [len=%d flu=%.2f echo=%.2f sem=%.2f] %s\n",
                   m_on.path_len, m_on.fluency, m_on.no_echo,
                   m_on.sem_cohesion, m_on.path_str);
        }
    }

    /* 汇总 */
    if (off_count > 0) { off_len /= off_count; off_echo /= off_count;
                          off_sem /= off_count; off_unique /= off_count; }
    if (on_count  > 0) { on_len  /= on_count;  on_echo  /= on_count;
                          on_sem  /= on_count;  on_unique  /= on_count; }

    printf("\n========================================\n");
    printf("  Aggregate Metrics (avg over %d walks)\n", num_seeds);
    printf("========================================\n");
    printf("%-20s %10s %10s %10s\n", "Metric", "OFF", "ON", "Delta");
    printf("%-20s %10s %10s %10s\n", "----", "---", "--", "-----");
    printf("%-20s %10.3f %10.3f %+10.3f\n", "Fluency", off_len, on_len, on_len - off_len);
    printf("%-20s %10.3f %10.3f %+10.3f\n", "No-echo", off_echo, on_echo, on_echo - off_echo);
    printf("%-20s %10.3f %10.3f %+10.3f\n", "Sem.cohesion", off_sem, on_sem, on_sem - off_sem);
    printf("%-20s %10.1f %10.1f %+10.1f\n", "Unique nodes", off_unique, on_unique, on_unique - off_unique);

    printf("\n========================================\n");
    printf("  Verdict\n");
    printf("========================================\n");
    int improved = 0;
    if (on_len > off_len)   { printf("+ Fluency improved (+%.3f)\n", on_len - off_len); improved++; }
    if (on_echo > off_echo) { printf("+ No-echo improved (+%.3f)\n", on_echo - off_echo); improved++; }
    if (on_sem > off_sem)   { printf("+ Sem.cohesion improved (+%.3f)\n", on_sem - off_sem); improved++; }
    if (improved >= 2) {
        printf("\n★★★ Template voting is effective! ★★★\n");
    } else if (improved == 1) {
        printf("\n★ Template voting shows moderate improvement.\n");
    } else {
        printf("\nTemplate voting did not improve raw metrics.\n");
        printf("(This is expected if templates haven't been built yet.)\n");
    }

    /* 清理 */
    free(ranks);
    master_topology_destroy(master);

    printf("\nDone.\n");
    return 0;
}
