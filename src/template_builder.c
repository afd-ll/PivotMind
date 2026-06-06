/**
 * @file template_builder.c
 * @brief 路径模板构建器 — 前缀分组 + 软聚类 + 模板节点生成
 */

#include "template_builder.h"
#include "string_pool.h"
#include "common.h"
#include "cognitive_controller.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ================================================================
 *  默认配置
 * ================================================================ */

TemplateBuildConfig template_config_default(void) {
    TemplateBuildConfig cfg;
    cfg.max_templates        = 200;
    cfg.min_ratio            = TEMPLATE_MIN_RATIO;
    cfg.similarity_threshold = TEMPLATE_SIMILARITY_THRESHOLD;
    cfg.min_cluster_size     = TEMPLATE_MIN_CLUSTER_SIZE;
    return cfg;
}

/* ================================================================
 *  辅助: 余弦相似度
 * ================================================================ */

static float cosine_sim(const float* a, const float* b, int dim) {
    if (!a || !b || dim <= 0) return 0.0f;
    float dot = 0.0f, na = 0.0f, nb = 0.0f;
    for (int i = 0; i < dim; i++) {
        dot += a[i] * b[i];
        na  += a[i] * a[i];
        nb  += b[i] * b[i];
    }
    float denom = sqrtf(na) * sqrtf(nb);
    return (denom > 1e-10f) ? (dot / denom) : 0.0f;
}

/* ================================================================
 *  辅助: 并查集 (Union-Find)
 * ================================================================ */

typedef struct {
    int* parent;
    int* rank;
    int  size;
} UnionFind;

static UnionFind* uf_create(int size) {
    UnionFind* uf = (UnionFind*)malloc(sizeof(UnionFind));
    if (!uf) return NULL;
    uf->parent = (int*)malloc((size_t)size * sizeof(int));
    uf->rank   = (int*)calloc((size_t)size, sizeof(int));
    uf->size   = size;
    if (!uf->parent || !uf->rank) {
        free(uf->parent); free(uf->rank); free(uf); return NULL;
    }
    for (int i = 0; i < size; i++) uf->parent[i] = i;
    return uf;
}

static int uf_find(UnionFind* uf, int x) {
    while (uf->parent[x] != x) {
        uf->parent[x] = uf->parent[uf->parent[x]];  /* path halving */
        x = uf->parent[x];
    }
    return x;
}

static void uf_union(UnionFind* uf, int x, int y) {
    int rx = uf_find(uf, x);
    int ry = uf_find(uf, y);
    if (rx == ry) return;
    if (uf->rank[rx] < uf->rank[ry]) {
        uf->parent[rx] = ry;
    } else if (uf->rank[rx] > uf->rank[ry]) {
        uf->parent[ry] = rx;
    } else {
        uf->parent[ry] = rx;
        uf->rank[rx]++;
    }
}

static void uf_destroy(UnionFind* uf) {
    if (!uf) return;
    free(uf->parent);
    free(uf->rank);
    free(uf);
}

/* ================================================================
 *  前缀分组分组结构（内部使用）
 * ================================================================ */

#define GROUP_INITIAL_CAP 32

typedef struct {
    int node_a;
    int node_b;
    int topo_id;
} PrefixKey;

/* 前缀分组内部查找表（哈希） */
typedef struct {
    PrefixKey key;
    int       group_index;  /* 在 groups 数组中的索引 */
    int       is_used;
} PrefixEntry;

#define PREFIX_HASH_SIZE 4093  /* 质数 */

/* 前缀哈希函数 (开放寻址) */
static unsigned prefix_hash_key(int node_a, int node_b) {
    return (unsigned)(((size_t)node_a * 31u + (size_t)node_b * 17u) % PREFIX_HASH_SIZE);
}

static int prefix_hash_lookup(PrefixEntry* table, int node_a, int node_b) {
    unsigned h = prefix_hash_key(node_a, node_b);
    for (int probe = 0; probe < PREFIX_HASH_SIZE; probe++) {
        unsigned idx = (h + (unsigned)probe) % PREFIX_HASH_SIZE;
        if (!table[idx].is_used) return -1;  /* not found */
        if (table[idx].key.node_a == node_a && table[idx].key.node_b == node_b)
            return table[idx].group_index;
    }
    return -1;  /* table full, fall through */
}

static int prefix_hash_insert(PrefixEntry* table, int node_a, int node_b, int group_index) {
    unsigned h = prefix_hash_key(node_a, node_b);
    for (int probe = 0; probe < PREFIX_HASH_SIZE; probe++) {
        unsigned idx = (h + (unsigned)probe) % PREFIX_HASH_SIZE;
        if (!table[idx].is_used) {
            table[idx].key.node_a = node_a;
            table[idx].key.node_b = node_b;
            table[idx].group_index = group_index;
            table[idx].is_used = 1;
            return 0;
        }
    }
    return -1;  /* table full */
}

/* ================================================================
 *  Step 1: 前缀分组
 * ================================================================ */

TripletPrefixGroup* template_group_triplets(
    IrreducibilityResult* results, int result_count,
    ReasoningNode* const* nodes, int node_count,
    TemplateBuildConfig* cfg, int* group_count_out) {
    (void)nodes; (void)node_count;
    if (!results || result_count <= 0 || !cfg || !group_count_out) return NULL;

    /* 按 ir_ratio 过滤 */
    int valid = 0;
    for (int i = 0; i < result_count; i++) {
        if (results[i].ir_ratio >= cfg->min_ratio) valid++;
    }
    if (valid == 0) { *group_count_out = 0; return NULL; }

    /* 预分配 groups + 前缀哈希表（O(1)查找替代O(n)线性扫描） */
    int groups_cap = (valid < 64) ? 64 : valid;
    TripletPrefixGroup* groups = (TripletPrefixGroup*)calloc(
        (size_t)groups_cap, sizeof(TripletPrefixGroup));
    if (!groups) return NULL;
    int gcount = 0;
    
    PrefixEntry* prefix_table = (PrefixEntry*)calloc(PREFIX_HASH_SIZE, sizeof(PrefixEntry));
    if (!prefix_table) { free(groups); return NULL; }

    for (int i = 0; i < result_count; i++) {
        IrreducibilityResult* r = &results[i];
        if (r->ir_ratio < cfg->min_ratio) continue;

        /* 前缀哈希查找 O(1) */
        int gi = prefix_hash_lookup(prefix_table, r->node_a, r->node_b);

        if (gi < 0) {
            /* 扩容 */
            if (gcount >= groups_cap) {
                groups_cap *= 2;
                TripletPrefixGroup* ng = (TripletPrefixGroup*)realloc(
                    groups, (size_t)groups_cap * sizeof(TripletPrefixGroup));
                if (!ng) { free(prefix_table); template_free_groups(groups, gcount); return NULL; }
                groups = ng;
            }
            gi = gcount;
            memset(&groups[gi], 0, sizeof(TripletPrefixGroup));
            groups[gi].node_a   = r->node_a;
            groups[gi].node_b   = r->node_b;
            groups[gi].topo_id  = 0;  /* 词汇拓扑 */
            groups[gi].capacity = GROUP_INITIAL_CAP;
            groups[gi].node_c_list = (int*)malloc(
                (size_t)groups[gi].capacity * sizeof(int));
            groups[gi].ir_ratios   = (float*)malloc(
                (size_t)groups[gi].capacity * sizeof(float));
            groups[gi].counts      = (int*)malloc(
                (size_t)groups[gi].capacity * sizeof(int));
            if (!groups[gi].node_c_list || !groups[gi].ir_ratios || !groups[gi].counts) {
                free(prefix_table); template_free_groups(groups, gi); return NULL;
            }
            gcount++;
            prefix_hash_insert(prefix_table, r->node_a, r->node_b, gi);
        }

        /* 添加到组 */
        TripletPrefixGroup* grp = &groups[gi];
        if (grp->size >= grp->capacity) {
            grp->capacity *= 2;
            int*   nc = (int*)realloc(grp->node_c_list, (size_t)grp->capacity * sizeof(int));
            float* ir = (float*)realloc(grp->ir_ratios, (size_t)grp->capacity * sizeof(float));
            int*   ct = (int*)realloc(grp->counts, (size_t)grp->capacity * sizeof(int));
            if (!nc || !ir || !ct) {
                template_free_groups(groups, gcount); return NULL;
            }
            grp->node_c_list = nc;
            grp->ir_ratios   = ir;
            grp->counts      = ct;
        }
        grp->node_c_list[grp->size] = r->node_c;
        grp->ir_ratios[grp->size]   = r->ir_ratio;
        grp->counts[grp->size]      = r->count;
        grp->size++;
    }

    /* 过滤: 移除 size < min_cluster_size 的组 */
    {
        int dst = 0;
        for (int i = 0; i < gcount; i++) {
            if (groups[i].size >= cfg->min_cluster_size) {
                if (dst != i) groups[dst] = groups[i];
                dst++;
            } else {
                free(groups[i].node_c_list);
                free(groups[i].ir_ratios);
                free(groups[i].counts);
            }
        }
        gcount = dst;
    }

    free(prefix_table);
    *group_count_out = gcount;
    if (gcount == 0) { free(groups); return NULL; }
    return groups;
}

/* ================================================================
 *  Step 2: 软聚类
 * ================================================================ */

TemplateCluster* template_cluster_groups(
    TripletPrefixGroup* groups, int group_count,
    ReasoningNode* const* nodes, int node_count,
    TemplateBuildConfig* cfg, int* cluster_count_out) {
    if (!groups || group_count <= 0 || !nodes || node_count <= 0 || !cfg || !cluster_count_out)
        return NULL;

    /* 预分配 (每个组的每个成员都可能成为单独簇，实际上远少于这个数) */
    int max_clusters = 0;
    for (int g = 0; g < group_count; g++)
        max_clusters += groups[g].size;
    if (max_clusters == 0) { *cluster_count_out = 0; return NULL; }

    TemplateCluster* clusters = (TemplateCluster*)calloc(
        (size_t)max_clusters, sizeof(TemplateCluster));
    if (!clusters) return NULL;
    int cc = 0;

    for (int g = 0; g < group_count; g++) {
        TripletPrefixGroup* grp = &groups[g];
        int sz = grp->size;
        if (sz < cfg->min_cluster_size) continue;

        /* 构建特征向量数组 */
        float** feats = (float**)malloc((size_t)sz * sizeof(float*));
        if (!feats) continue;
        int has_features = 0;
        for (int i = 0; i < sz; i++) {
            int nc_id = grp->node_c_list[i];
            if (nc_id >= 0 && nc_id < node_count && nodes[nc_id] && nodes[nc_id]->features) {
                feats[i] = nodes[nc_id]->features;
                has_features = 1;
            } else {
                feats[i] = NULL;
            }
        }

        if (!has_features) {
            free(feats);
            continue;
        }

        /* 并查集聚类: 余弦相似度 > threshold 的归入同簇 */
        UnionFind* uf = uf_create(sz);
        if (!uf) { free(feats); continue; }

        for (int i = 0; i < sz; i++) {
            if (!feats[i]) continue;
            for (int j = i + 1; j < sz; j++) {
                if (!feats[j]) continue;
                float sim = cosine_sim(feats[i], feats[j], NODE_FEATURE_DIM);
                if (sim > cfg->similarity_threshold) {
                    uf_union(uf, i, j);
                }
            }
        }

        /* 收集各簇的根 */
        typedef struct {
            int    root;
            int    member_count;
            int    total_count;
            int    best_idx;     /* count 最大的成员索引 */
            int    best_count;
            int*   members;
        } ClusterRoot;
        ClusterRoot* roots = (ClusterRoot*)calloc((size_t)sz, sizeof(ClusterRoot));
        if (!roots) { uf_destroy(uf); free(feats); continue; }

        for (int i = 0; i < sz; i++) {
            int r = uf_find(uf, i);
            int found = 0;
            for (int k = 0; k < sz; k++) {
                if (roots[k].member_count == 0) {
                    roots[k].root = r;
                    roots[k].members = (int*)malloc((size_t)sz * sizeof(int));
                    roots[k].member_count = 1;
                    roots[k].members[0] = i;
                    roots[k].total_count = grp->counts[i];
                    roots[k].best_idx = i;
                    roots[k].best_count = grp->counts[i];
                    found = 1;
                    break;
                } else if (roots[k].root == r) {
                    int mc = roots[k].member_count;
                    roots[k].members[mc] = i;
                    roots[k].member_count++;
                    roots[k].total_count += grp->counts[i];
                    if (grp->counts[i] > roots[k].best_count) {
                        roots[k].best_idx = i;
                        roots[k].best_count = grp->counts[i];
                    }
                    found = 1;
                    break;
                }
            }
            (void)found;  /* should always be found */
        }

        /* 输出到 clusters */
        for (int k = 0; k < sz; k++) {
            if (roots[k].member_count < cfg->min_cluster_size) {
                free(roots[k].members);
                continue;
            }

            TemplateCluster* cl = &clusters[cc];
            cl->node_a = grp->node_a;
            cl->node_b = grp->node_b;
            cl->member_ids = (int*)malloc(
                (size_t)roots[k].member_count * sizeof(int));
            if (!cl->member_ids) {
                free(roots[k].members);
                continue;
            }
            for (int m = 0; m < roots[k].member_count; m++) {
                int mi = roots[k].members[m];
                cl->member_ids[m] = grp->node_c_list[mi];
            }
            cl->member_count     = roots[k].member_count;
            cl->representative_c = grp->node_c_list[roots[k].best_idx];
            cl->total_count      = roots[k].total_count;
            cl->template_node_id = -1;
            cc++;
            free(roots[k].members);
        }

        uf_destroy(uf);
        free(roots);
        free(feats);
    }

    if (cc == 0) { free(clusters); *cluster_count_out = 0; return NULL; }

    *cluster_count_out = cc;
    return clusters;
}

/* ================================================================
 *  Step 3: 创建模板节点
 * ================================================================ */

int template_build_nodes(
    MasterTopology* master,
    TemplateCluster* clusters, int cluster_count,
    SubTopology* vocab, int max_templates) {
    if (!master || !clusters || cluster_count <= 0 || !vocab || !vocab->net)
        return 0;

    SubTopology* tpl = master_get_sub_topology_by_type(master, TOPO_TEMPLATE);
    if (!tpl || !tpl->net) return 0;

    HuarongTopologyNet* vnet = vocab->net;
    int vnc = vnet->node_count;

    int limit = (cluster_count < max_templates) ? cluster_count : max_templates;
    int built = 0;

    for (int i = 0; i < limit; i++) {
        TemplateCluster* cl = &clusters[i];
        if (cl->member_count < 2) continue;

        int na_id = cl->node_a;
        int nb_id = cl->node_b;
        int nc_id = cl->representative_c;

        /* 节点范围校验 */
        if (na_id < 0 || na_id >= vnc) continue;
        if (nb_id < 0 || nb_id >= vnc) continue;
        if (nc_id < 0 || nc_id >= vnc) continue;

        ReasoningNode* na = vnet->nodes[na_id];
        ReasoningNode* nb = vnet->nodes[nb_id];
        ReasoningNode* nc = vnet->nodes[nc_id];
        if (!na || !nb || !nc) continue;

        /* 构建模板节点名称 */
        char name_buf[256];
        const char* ca = na->concept ? na->concept : "?";
        const char* cb = nb->concept ? nb->concept : "?";
        const char* cc = nc->concept ? nc->concept : "?";
        snprintf(name_buf, sizeof(name_buf), "T:%s_%s_%s", ca, cb, cc);

        /* 构建模板特征向量：三节点特征的加权平均 */
        float tpl_feat[NODE_FEATURE_DIM];
        memset(tpl_feat, 0, sizeof(tpl_feat));
        int feat_dim = NODE_FEATURE_DIM;

        if (na->features && nb->features && nc->features) {
            for (int d = 0; d < feat_dim; d++) {
                tpl_feat[d] = (na->features[d] * 0.4f +
                               nb->features[d] * 0.35f +
                               nc->features[d] * 0.25f);
            }
        } else if (na->features) {
            memcpy(tpl_feat, na->features, feat_dim * sizeof(float));
        } else {
            continue;  /* 没有特征无法建模板 */
        }

        /* 在模板拓扑中创建节点 */
        float* feat_copy = (float*)malloc((size_t)feat_dim * sizeof(float));
        if (!feat_copy) continue;
        memcpy(feat_copy, tpl_feat, (size_t)feat_dim * sizeof(float));

        ReasoningNode* tpl_node = huarong_net_add_node(tpl->net, name_buf, feat_copy, feat_dim);
        if (!tpl_node) {
            free(feat_copy);
            continue;
        }
        int tpl_node_id = tpl_node->node_id;
        cl->template_node_id = tpl_node_id;

        /* 存储模板锚点元数据 — POS 序列格式 */
        tpl_node->tpl_pos_len = 3;  /* 三元组模板 */
        /* 从锚点节点获取 POS 标签（通过跨拓扑→语法拓扑） */
        tpl_node->tpl_pos_seq[0] = master_get_node_pos_tag(master, vocab->topo_id, na_id);
        tpl_node->tpl_pos_seq[1] = master_get_node_pos_tag(master, vocab->topo_id, nb_id);
        tpl_node->tpl_pos_seq[2] = master_get_node_pos_tag(master, vocab->topo_id, nc_id);
        memset(tpl_node->tpl_connectors, 0, sizeof(tpl_node->tpl_connectors));

        /* 建立跨拓扑连接: anchor nodes → template node */
        master_add_cross_link(master, vocab->topo_id, na_id,
                              tpl->topo_id, tpl_node_id, 0.8f, "anchor_a");
        master_add_cross_link(master, vocab->topo_id, nb_id,
                              tpl->topo_id, tpl_node_id, 0.6f, "anchor_b");

        /* 模板节点之间：共享 node_a 或 node_b 的建立跨拓扑连接 */
        for (int j = 0; j < i; j++) {
            if (clusters[j].template_node_id < 0) continue;
            if (clusters[j].node_a == cl->node_a || clusters[j].node_b == cl->node_b) {
                master_add_cross_link(master, tpl->topo_id, clusters[j].template_node_id,
                                      tpl->topo_id, tpl_node_id, 0.4f, "shared_anchor");
            }
        }

        built++;
    }

    if (built > 0) {
        /* 启用模板投票 */
        master->use_template_voting = 1;
    }

    return built;
}

/* ================================================================
 *  资源释放
 * ================================================================ */

void template_free_groups(TripletPrefixGroup* groups, int count) {
    if (!groups) return;
    for (int i = 0; i < count; i++) {
        free(groups[i].node_c_list);
        free(groups[i].ir_ratios);
        free(groups[i].counts);
    }
    free(groups);
}

void template_free_clusters(TemplateCluster* clusters, int count) {
    if (!clusters) return;
    for (int i = 0; i < count; i++) {
        free(clusters[i].member_ids);
    }
    free(clusters);
}

/* ================================================================
 *  P2 Task 7: 模板的模板 — 高层概念节点
 *
 *  从高频模板节点中提取更高层的抽象概念。
 *  相似特征向量的模板节点聚类为一个概念，
 *  概念节点加入 TOPO_CONCEPT 并建立跨拓扑投票连接。
 * ================================================================ */

#define CONCEPT_SIMILARITY_THRESHOLD 0.65f  /* 概念聚类阈值（比模板低，允许更大粒度） */
#define CONCEPT_MIN_GROUP_SIZE       3      /* 至少3个模板才能形成概念 */

int template_build_concepts(MasterTopology* master, int max_concepts) {
    if (!master || max_concepts <= 0) return 0;

    SubTopology* tpl = master_get_sub_topology_by_type(master, TOPO_TEMPLATE);
    SubTopology* concept = master_get_sub_topology_by_type(master, TOPO_CONCEPT);
    if (!tpl || !tpl->net || !concept || !concept->net) return 0;

    HuarongTopologyNet* tnet = tpl->net;
    int tn = tnet->node_count;
    if (tn < CONCEPT_MIN_GROUP_SIZE) return 0;

    /* 收集有特征的模板节点 */
    float** feats = (float**)malloc((size_t)tn * sizeof(float*));
    int* tpl_ids = (int*)malloc((size_t)tn * sizeof(int));
    int valid = 0;
    for (int i = 0; i < tn; i++) {
        ReasoningNode* node = tnet->nodes[i];
        if (node && node->features) {
            feats[valid] = node->features;
            tpl_ids[valid] = i;
            valid++;
        }
    }
    if (valid < CONCEPT_MIN_GROUP_SIZE) { free(feats); free(tpl_ids); return 0; }

    /* 并查集聚类: 余弦相似度 > threshold 的模板归为一组 */
    UnionFind* uf = uf_create(valid);
    if (!uf) { free(feats); free(tpl_ids); return 0; }

    for (int i = 0; i < valid; i++) {
        for (int j = i + 1; j < valid; j++) {
            float sim = cosine_sim(feats[i], feats[j], NODE_FEATURE_DIM);
            if (sim > CONCEPT_SIMILARITY_THRESHOLD) {
                uf_union(uf, i, j);
            }
        }
    }

    /* 收集各根节点的成员 */
    typedef struct { int root; int* members; int count; } ConceptGroup;
    ConceptGroup* groups = (ConceptGroup*)calloc((size_t)valid, sizeof(ConceptGroup));
    int gc = 0;
    for (int i = 0; i < valid; i++) {
        int r = uf_find(uf, i);
        int found = 0;
        for (int g = 0; g < gc; g++) {
            if (groups[g].root == r) {
                if (!groups[g].members) {
                    groups[g].members = (int*)malloc((size_t)valid * sizeof(int));
                }
                groups[g].members[groups[g].count++] = i;
                found = 1; break;
            }
        }
        if (!found) {
            groups[gc].root = r;
            groups[gc].members = (int*)malloc((size_t)valid * sizeof(int));
            groups[gc].members[0] = i;
            groups[gc].count = 1;
            gc++;
        }
    }

    /* 为每个 >= CONCEPT_MIN_GROUP_SIZE 的组创建概念节点 */
    int built = 0;
    for (int g = 0; g < gc && built < max_concepts; g++) {
        if (groups[g].count < CONCEPT_MIN_GROUP_SIZE) continue;

        /* 构建概念特征向量: 组内成员特征的加权平均 */
        float concept_feat[NODE_FEATURE_DIM];
        memset(concept_feat, 0, sizeof(concept_feat));
        for (int m = 0; m < groups[g].count; m++) {
            int mi = groups[g].members[m];
            for (int d = 0; d < NODE_FEATURE_DIM; d++) {
                concept_feat[d] += feats[mi][d];
            }
        }
        float inv_n = 1.0f / (float)groups[g].count;
        for (int d = 0; d < NODE_FEATURE_DIM; d++) {
            concept_feat[d] *= inv_n;
        }

        /* 概念节点名称: 取前两个模板的代表节点名 */
        char name_buf[256];
        ReasoningNode* n0 = tnet->nodes[tpl_ids[groups[g].members[0]]];
        ReasoningNode* n1 = (groups[g].count >= 2)
            ? tnet->nodes[tpl_ids[groups[g].members[1]]] : NULL;
        const char* c0 = (n0 && n0->concept) ? n0->concept : "?";
        const char* c1 = (n1 && n1->concept) ? n1->concept : "";
        snprintf(name_buf, sizeof(name_buf), "C:%s+%s", c0, c1);

        /* 创建特征副本 */
        float* feat_copy = (float*)malloc((size_t)NODE_FEATURE_DIM * sizeof(float));
        if (!feat_copy) continue;
        memcpy(feat_copy, concept_feat, (size_t)NODE_FEATURE_DIM * sizeof(float));

        ReasoningNode* cnode = huarong_net_add_node(concept->net, name_buf,
                                                     feat_copy, NODE_FEATURE_DIM);
        if (!cnode) { free(feat_copy); continue; }
        int cid = cnode->node_id;

        /* 概念 → 每个成员模板: 双向跨拓扑连接 */
        for (int m = 0; m < groups[g].count; m++) {
            int tid = tpl_ids[groups[g].members[m]];
            master_add_cross_link(master, concept->topo_id, cid,
                                  tpl->topo_id, tid, 0.7f, "abstracts");
            master_add_cross_link(master, tpl->topo_id, tid,
                                  concept->topo_id, cid, 0.5f, "instance_of");
        }
        built++;
    }

    /* 清理 */
    for (int g = 0; g < gc; g++) free(groups[g].members);
    free(groups);
    uf_destroy(uf);
    free(feats);
    free(tpl_ids);

    return built;
}

/* ================================================================
 *  P2 Task 8: 冷路径稀释
 *
 *  不活跃的模板跨拓扑链接逐步衰减 transfer_rate，
 *  长期不激活的标记 but 保留不删除（允许后续重新激活）。
 * ================================================================ */

#define COLD_DECAY_MIN_USE   3      /* use_count 低于此值触发衰减 */
#define COLD_TRANSFER_FLOOR  0.05f  /* transfer_rate 下限，降至此处标记 inactive */

int template_decay_inactive_links(MasterTopology* master,
                                  int max_idle_rounds, float decay_rate) {
    if (!master || max_idle_rounds <= 0 || decay_rate <= 0.0f || decay_rate >= 1.0f)
        return 0;

    master->template_decay_round++;

    /* 非衰减轮: 仅重置 use_count 供下轮统计 */
    if (master->template_decay_round % max_idle_rounds != 0) {
        for (int i = 0; i < master->cross_link_count; i++) {
            if (master->cross_links[i]) master->cross_links[i]->use_count = 0;
        }
        return 0;
    }

    int decayed = 0;
    for (int i = 0; i < master->cross_link_count; i++) {
        CrossTopologyLink* link = master->cross_links[i];
        if (!link) continue;

        /* 仅处理涉及 TOPO_TEMPLATE 的链接 */
        if (link->from_topo_id != TOPO_TEMPLATE && link->to_topo_id != TOPO_TEMPLATE)
            continue;

        /* 低使用率: 衰减 transfer_rate */
        if (link->use_count < COLD_DECAY_MIN_USE) {
            float old_rate = link->transfer_rate;
            link->transfer_rate *= decay_rate;

            /* 降至地板: 标记为冷路径 */
            if (link->transfer_rate < COLD_TRANSFER_FLOOR) {
                link->transfer_rate = COLD_TRANSFER_FLOOR;
            }

            if (link->transfer_rate < old_rate) decayed++;
        }

        /* 重置计数器 (本期统计) */
        link->use_count = 0;
    }

    return decayed;
}

/* ================================================================
 *  运行时自构建: freq_table → 模板节点 一站式管线
 * ================================================================ */

int template_auto_build(MasterTopology* master, int min_entries, int max_templates) {
    if (!master) return 0;

    /* 增量构建: 模板拓扑用 confidence 自然生灭，不再一次性幂等 */
    SubTopology* tpl = master_get_sub_topology_by_type(master, TOPO_TEMPLATE);
    if (!tpl || !tpl->net) return 0;
    /* 移除幂等守卫: 允许模板节点随频率表增长而增量更新 */
    /* 旧 guard: if (tpl->net->node_count > 0) return 0; */

    /* 数据充足性检查 */
    if (!master->freq_table || master->freq_table->entry_count < min_entries) return 0;

    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    if (!vocab || !vocab->net) return 0;

    int nc = vocab->net->node_count;
    if (nc < 500) return 0;

    /* Step 1: 不可分解性分析 */
    int rc = 0;
    IrreducibilityResult* res = path_analyze_irreducibility(
        master->freq_table, (void* const*)vocab->net->nodes, nc, &rc);
    if (!res || rc == 0) return 0;

    /* Step 2: 前缀分组 */
    TemplateBuildConfig cfg = template_config_default();
    cfg.max_templates = max_templates;

    int gc = 0;
    TripletPrefixGroup* grps = template_group_triplets(
        res, rc, (ReasoningNode* const*)vocab->net->nodes, nc, &cfg, &gc);

    /* Step 3: 软聚类 */
    int cc = 0;
    TemplateCluster* clus = NULL;
    if (grps && gc > 0) {
        clus = template_cluster_groups(
            grps, gc, (ReasoningNode* const*)vocab->net->nodes, nc, &cfg, &cc);
    }

    /* Step 4: 创建模板节点 */
    int built = 0;
    if (clus && cc > 0) {
        built = template_build_nodes(master, clus, cc, vocab, max_templates);
    }

    /* Step 5: 创建高层概念节点 */
    if (built > 0) {
        template_build_concepts(master, max_templates / 4);
    }

    /* 清理 */
    template_free_clusters(clus, cc);
    template_free_groups(grps, gc);
    free(res);

    return built;
}

/* ================================================================
 *  语法句式模板构建 — 从 POSPattern 生成主谓宾/定中等句式模板
 *
 *  与频率表路径完全不同：不分析 co-occurrence，而是直接利用
 *  cognitive_controller 自动发现的 POS 句式模式。
 *
 *  每个模板节点编码：
 *    - pos_seq[0..len-1]: POSTag 序列 (如 [N,V,N] = 主谓宾)
 *    - connectors[0..len-2]: 槽位间连接词 (如 ["", ""])
 * ================================================================ */

int template_build_from_pos_patterns(MasterTopology* master,
                                      CognitiveController* cc,
                                      int min_count) {
    if (!master || !cc) return 0;
    if (cc->pos_pattern_count == 0) return 0;

    SubTopology* tpl = master_get_sub_topology_by_type(master, TOPO_TEMPLATE);
    if (!tpl || !tpl->net) return 0;

    int built = 0;
    int max_build = 64;  /* 最多64个语法模板，避免膨胀 */

    /* 按观测次数排序（简单的冒泡排序，PATTERN_COUNT 小） */
    POSPattern sorted[MAX_POS_PATTERNS];
    int sc = cc->pos_pattern_count < MAX_POS_PATTERNS
             ? cc->pos_pattern_count : MAX_POS_PATTERNS;
    memcpy(sorted, cc->pos_patterns, (size_t)sc * sizeof(POSPattern));
    for (int i = 0; i < sc - 1; i++) {
        for (int j = i + 1; j < sc; j++) {
            if (sorted[j].count > sorted[i].count) {
                POSPattern tmp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = tmp;
            }
        }
    }

    /* 去重：同一 POS 序列只取最高频的 */
    for (int i = 0; i < sc && built < max_build; i++) {
        POSPattern* pat = &sorted[i];
        if (pat->count < min_count) continue;
        if (pat->length < 2 || pat->length > 4) continue;

        /* 去重检查：已有同序列模板？ */
        int dup = 0;
        for (int j = 0; j < i && !dup; j++) {
            if (sorted[j].length != pat->length) continue;
            int same = 1;
            for (int k = 0; k < pat->length; k++) {
                if (sorted[j].pos_seq[k] != pat->pos_seq[k])
                    { same = 0; break; }
            }
            if (same) dup = 1;
        }
        if (dup) continue;

        /* 构建模板节点名称 */
        const char* pos_names[] = {
            "?", "N", "V", "Adj", "Adv", "Pron", "Prep", "Conj", "Num", "Part", "Int"
        };
        char name_buf[128];
        int np = 0;
        for (int k = 0; k < pat->length; k++) {
            int tag = pat->pos_seq[k];
            const char* pn = (tag >= 0 && tag <= POS_INTERJ) ? pos_names[tag] : "?";
            np += snprintf(name_buf + np, sizeof(name_buf) - np,
                           "%s%s", (k > 0 ? "+" : ""), pn);
        }

        /* 创建特征向量（槽位 POS 向量化） */
        float feat[NODE_FEATURE_DIM];
        memset(feat, 0, sizeof(feat));
        for (int k = 0; k < pat->length && k < NODE_FEATURE_DIM; k++)
            feat[k] = (float)pat->pos_seq[k] / (float)POS_COUNT;

        float* feat_copy = (float*)malloc((size_t)NODE_FEATURE_DIM * sizeof(float));
        if (!feat_copy) continue;
        memcpy(feat_copy, feat, (size_t)NODE_FEATURE_DIM * sizeof(float));

        ReasoningNode* tn = huarong_net_add_node(tpl->net, name_buf,
                                                   feat_copy, NODE_FEATURE_DIM);
        if (!tn) { free(feat_copy); continue; }

        /* 存入 POS 序列 + 自动生成连接词 */
        tn->tpl_pos_len = pat->length;
        for (int k = 0; k < pat->length; k++)
            tn->tpl_pos_seq[k] = pat->pos_seq[k];
        for (int k = 0; k < pat->length - 1; k++) {
            const char* conn = pos_connector_map(
                pat->pos_seq[k], pat->pos_seq[k+1]);
            if (conn && conn[0])
                snprintf(tn->tpl_connectors[k], 8, "%s", conn);
            else
                tn->tpl_connectors[k][0] = '\0';
        }

        /* 初始化置信度：基于观测频率 */
        tn->confidence = pat->count > 50 ? 0.8f :
                         pat->count > 20 ? 0.6f :
                         pat->count > 5  ? 0.4f : 0.25f;

        /* 更新 pattern 的 syntax_node_id */
        pat->syntax_node_id = tn->node_id;

        built++;
    }

    if (built > 0) {
        master->use_template_voting = 1;
    }
    return built;
}

