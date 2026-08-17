/**
 * @file template_builder.c
 * @brief 路径模板构建器 — 前缀分组 + 软聚类 + 模板节点生成
 */

#include "template_builder.h"
#include "string_pool.h"
#include "common.h"
#include "cognitive_controller.h"
#include "emergent_pos.h"
#include "topology_growth.h"
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
 *  模板合并辅助: 连接词归一化
 *
 *  规则:
 *    1. NULL 或空字符串 → ""
 *    2. 去除首尾空白
 *    3. 纯空白 → ""
 *
 *  dst 缓冲区至少需要 TPL_CONNECTOR_BUF 字节。
 * ================================================================ */

static void norm_connector(const char* src, char* dst) {
    if (!src || !src[0]) { dst[0] = '\0'; return; }
    /* 跳过头部的空白 */
    while (*src == ' ' || *src == '\t') src++;
    /* 找到尾部非空白位置 */
    int len = 0;
    while (src[len] && src[len] != ' ' && src[len] != '\t') len++;
    if (len == 0) { dst[0] = '\0'; return; }
    int n = (len < TPL_CONNECTOR_BUF - 1) ? len : TPL_CONNECTOR_BUF - 1;
    memcpy(dst, src, (size_t)n);
    dst[n] = '\0';
}

/* ================================================================
 *  模板合并辅助: 合并键哈希
 *
 *  合并键 = (tpl_pos_len, tpl_pos_seq[0..len-1],
 *            normalized tpl_connectors[0..len-2])
 *
 *  pos_seq 逐元素 + 归一化后的 connector 逐字节参与 djb2 哈希。
 * ================================================================ */

static uint32_t merge_key_hash(int pos_len, const int* pos_seq,
                                const char connectors[][TPL_CONNECTOR_BUF]) {
    uint32_t h = 5381;
    h = (h * 33) ^ (uint32_t)pos_len;
    for (int i = 0; i < pos_len; i++)
        h = (h * 33) ^ (uint32_t)pos_seq[i];
    for (int i = 0; i < pos_len - 1; i++) {
        char norm[TPL_CONNECTOR_BUF];
        norm_connector(connectors[i], norm);
        for (const char* p = norm; *p; p++)
            h = (h * 33) ^ (uint8_t)*p;
    }
    return h;
}

/* ================================================================
 *  模板合并辅助: 合并键相等性判断
 *
 *  conn_a: 组内已存储的归一化 connector
 *  conn_b: 候选模板的原始 connector（需要归一化后比较）
 * ================================================================ */

static int merge_key_equals(
    int len_a, const int* seq_a, const char conn_a[][TPL_CONNECTOR_BUF],
    int len_b, const int* seq_b, const char conn_b[][TPL_CONNECTOR_BUF])
{
    if (len_a != len_b) return 0;
    for (int i = 0; i < len_a; i++)
        if (seq_a[i] != seq_b[i]) return 0;
    for (int i = 0; i < len_a - 1; i++) {
        char norm[TPL_CONNECTOR_BUF];
        norm_connector(conn_b[i], norm);
        if (strcmp(conn_a[i], norm) != 0) return 0;
    }
    return 1;
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
            int new_cap = grp->capacity * 2;
            int*   nc = (int*)realloc(grp->node_c_list, (size_t)new_cap * sizeof(int));
            float* ir = (float*)realloc(grp->ir_ratios, (size_t)new_cap * sizeof(float));
            int*   ct = (int*)realloc(grp->counts, (size_t)new_cap * sizeof(int));
            if (!nc || !ir || !ct) {
                free(nc); free(ir); free(ct);
                template_free_groups(groups, gcount); return NULL;
            }
            grp->node_c_list = nc;
            grp->ir_ratios   = ir;
            grp->counts      = ct;
            grp->capacity    = new_cap;
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
        fprintf(stderr, "[模板]   组%d: size=%d a=%d b=%d\n", g, sz,
                grp->node_a, grp->node_b);
        if (sz < cfg->min_cluster_size) {
            fprintf(stderr, "[模板]   组%d: 跳过(size=%d < min=%d)\n", g, sz, cfg->min_cluster_size);
            continue;
        }

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
            /* 无特征时的降级：每个 size>=min_cluster_size 的组作为一个独立簇 */
            fprintf(stderr, "[模板]   组%d: 无特征，降级为直接簇\n", g);
            if (cc >= max_clusters) break;
            clusters[cc].member_ids = (int*)malloc((size_t)sz * sizeof(int));
            if (!clusters[cc].member_ids) { free(feats); continue; }
            for (int i = 0; i < sz; i++) {
                clusters[cc].member_ids[i] = grp->node_c_list[i];
            }
            clusters[cc].member_count = sz;
            clusters[cc].representative_c = grp->node_c_list[0];
            clusters[cc].total_count = grp->counts[0] + (sz > 1 ? grp->counts[1] : 0);
            cc++;
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
                float sim = cosine_similarity(feats[i], feats[j], NODE_FEATURE_DIM);
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
        int added = 0;
        for (int k = 0; k < sz; k++) {
            if (roots[k].member_count == 0) continue;
            if (roots[k].member_count < cfg->min_cluster_size) {
                fprintf(stderr, "[模板]   组%d root%d: 成员不足(%d<%d) 跳过\n",
                        g, k, roots[k].member_count, cfg->min_cluster_size);
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
            added++;
            free(roots[k].members);
        }

        uf_destroy(uf);
        free(roots);

        /* 无特征或无聚类时的降级：整个组作为一个簇 */
        if (added == 0 && sz >= cfg->min_cluster_size) {
            fprintf(stderr, "[模板]   组%d: 余弦聚类无产出，降级为直接簇\n", g);
            if (cc < max_clusters) {
                TemplateCluster* cl = &clusters[cc];
                cl->node_a = grp->node_a;
                cl->node_b = grp->node_b;
                cl->member_ids = (int*)malloc((size_t)sz * sizeof(int));
                if (cl->member_ids) {
                    for (int i = 0; i < sz; i++)
                        cl->member_ids[i] = grp->node_c_list[i];
                    cl->member_count = sz;
                    cl->representative_c = grp->node_c_list[0];
                    cl->total_count = grp->counts[0] + (sz > 1 ? grp->counts[1] : 0);
                    cl->template_node_id = -1;
                    cc++;
                }
            }
        }

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

        /* 在模板拓扑中创建节点（自动扩容） */
        float* feat_copy = (float*)malloc((size_t)feat_dim * sizeof(float));
        if (!feat_copy) continue;
        memcpy(feat_copy, tpl_feat, (size_t)feat_dim * sizeof(float));

        int tpl_node_id = insert_node_dynamic(master, tpl->topo_id, name_buf, feat_copy, feat_dim);
        if (tpl_node_id < 0) {
            free(feat_copy);
            continue;
        }
        cl->template_node_id = tpl_node_id;
        ReasoningNode* tpl_node = tpl->net->nodes[tpl_node_id];

        /* 存储模板锚点元数据 — POS 序列格式 */
        tpl_node->tpl_pos_len = 3;  /* 三元组模板 */
        /* 从锚点节点获取 POS 标签（通过跨拓扑→语法拓扑） */
        tpl_node->tpl_pos_seq[0] = master_get_node_pos_tag(master, vocab->topo_id, na_id);
        tpl_node->tpl_pos_seq[1] = master_get_node_pos_tag(master, vocab->topo_id, nb_id);
        tpl_node->tpl_pos_seq[2] = master_get_node_pos_tag(master, vocab->topo_id, nc_id);
        memset(tpl_node->tpl_connectors, 0, sizeof(tpl_node->tpl_connectors));

        /* 涌现槽位 — 从锚点节点的涌现词类填充 */
        tpl_node->tpl_emergent_slot[0] = na->emergent_class_count > 0
            ? na->emergent_class_ids[0] : -1;
        tpl_node->tpl_emergent_slot[1] = nb->emergent_class_count > 0
            ? nb->emergent_class_ids[0] : -1;
        tpl_node->tpl_emergent_slot[2] = nc->emergent_class_count > 0
            ? nc->emergent_class_ids[0] : -1;
        tpl_node->tpl_emergent_conf[0] = na->emergent_class_count > 0
            ? na->emergent_class_confs[0] : 0.0f;
        tpl_node->tpl_emergent_conf[1] = nb->emergent_class_count > 0
            ? nb->emergent_class_confs[0] : 0.0f;
        tpl_node->tpl_emergent_conf[2] = nc->emergent_class_count > 0
            ? nc->emergent_class_confs[0] : 0.0f;
        /* 确保未使用的槽位为 -1 */
        tpl_node->tpl_emergent_slot[3] = -1;
        tpl_node->tpl_emergent_conf[3] = 0.0f;

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
            float sim = cosine_similarity(feats[i], feats[j], NODE_FEATURE_DIM);
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

        int cid = insert_node_dynamic(master, concept->topo_id, name_buf,
                                       feat_copy, NODE_FEATURE_DIM);
        if (cid < 0) { free(feat_copy); continue; }

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
    if (!tpl || !tpl->net) { fprintf(stderr, "[模板] 模板拓扑不存在\n"); return 0; }

    /* 数据充足性检查 */
    if (!master->freq_table || master->freq_table->entry_count < min_entries) {
        fprintf(stderr, "[模板] freq不足: entries=%d need=%d\n",
                master->freq_table ? master->freq_table->entry_count : 0, min_entries);
        return 0;
    }

    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    if (!vocab || !vocab->net) { fprintf(stderr, "[模板] 词汇拓扑不存在\n"); return 0; }

    int nc = vocab->net->node_count;
    if (nc < 500) { fprintf(stderr, "[模板] 词汇不足: %d < 500\n", nc); return 0; }

    fprintf(stderr, "[模板] 开始构建: vocab=%d freq=%d max_tpl=%d\n", nc,
            master->freq_table->entry_count, max_templates);

    /* Step 1: 不可分解性分析 */
    int rc = 0;
    IrreducibilityResult* res = path_analyze_irreducibility(
        master->freq_table, (void* const*)vocab->net->nodes, nc, &rc);
    if (!res || rc == 0) {
        fprintf(stderr, "[模板] 不可分解性分析失败: res=%p rc=%d\n", (void*)res, rc);
        return 0;
    }
    fprintf(stderr, "[模板] 不可分解性分析: %d 结果\n", rc);

    /* Step 2: 前缀分组 */
    TemplateBuildConfig cfg = template_config_default();
    cfg.max_templates = max_templates;

    int gc = 0;
    TripletPrefixGroup* grps = template_group_triplets(
        res, rc, (ReasoningNode* const*)vocab->net->nodes, nc, &cfg, &gc);
    fprintf(stderr, "[模板] 三元组分组: %d 组\n", gc);

    /* Step 3: 软聚类 */
    int cc = 0;
    TemplateCluster* clus = NULL;
    if (grps && gc > 0) {
        clus = template_cluster_groups(
            grps, gc, (ReasoningNode* const*)vocab->net->nodes, nc, &cfg, &cc);
        fprintf(stderr, "[模板] 软聚类: %d 簇\n", cc);
    } else {
        fprintf(stderr, "[模板] 跳过聚类: grps=%p gc=%d\n", (void*)grps, gc);
    }

    /* Step 4: 创建模板节点 */
    int built = 0;
    if (clus && cc > 0) {
        built = template_build_nodes(master, clus, cc, vocab, max_templates);
        fprintf(stderr, "[模板] 模板创建: %d 个\n", built);
    } else {
        fprintf(stderr, "[模板] 跳过创建: clus=%p cc=%d\n", (void*)clus, cc);
    }

    /* Step 5: POS 结构合并 — 消除 Pipeline A/B 间的冗余模板 */
    if (built > 0) {
        template_merge_by_pos_structure(master);
    }

    /* Step 6: 创建高层概念节点 */
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

        int tn_id = insert_node_dynamic(master, tpl->topo_id, name_buf,
                                         feat_copy, NODE_FEATURE_DIM);
        if (tn_id < 0) { free(feat_copy); continue; }
        ReasoningNode* tn = tpl->net->nodes[tn_id];

        /* 存入 POS 序列 + 自动生成连接词 */
        tn->tpl_pos_len = pat->length;
        for (int k = 0; k < pat->length; k++) {
            tn->tpl_pos_seq[k] = pat->pos_seq[k];
            /* 涌现槽位: POS 语法模板的槽位 ID 即 POSTag 值本身 */
            tn->tpl_emergent_slot[k] = (int)pat->pos_seq[k];
            tn->tpl_emergent_conf[k] = 1.0f; /* POS 模式模板置信度高 */
        }
        /* 确保未使用的槽位为 -1 */
        for (int k = pat->length; k < 4; k++) {
            tn->tpl_emergent_slot[k] = -1;
            tn->tpl_emergent_conf[k] = 0.0f;
        }
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

/* ================================================================
 *  POS 槽位化诊断 — 用现有数据验证"相同 POS = 相同推理角色"假设
 *
 *  核心问题：
 *    相同 POS 结构（如 [N][的][N]）的不同词对（苹果的红色、
 *    中国的首都、人民的利益）在特征空间中是否足够接近？
 *    如果差异很大，简单 POS 合并会稀释模板的推理引导力。
 *
 *  诊断维度：
 *    (a) 组内特征离异度：pairwise cosine sim 的分布
 *    (b) 连接词一致性：tpl_connectors[0] 是否统一
 *    (c) 组间重合度：不同 POS 组之间的最近邻相似度
 *    (d) 组内样本数：样本太少时统计不可靠
 *
 *  输出：机器可解析的表格 + 人类可读的建议
 * ================================================================ */

/* 诊断用 POS 名映射（11 类，索引即 POSTag 值） */
static const char* diag_pos_name(int tag) {
    static const char* names[] = {
        "?", "N", "V", "Adj", "Adv", "Pron", "Prep", "Conj", "Num", "Part", "Int"
    };
    if (tag < 0 || tag >= POS_COUNT) return "?";
    return names[tag];
}

/* POS 组内统计 */
typedef struct {
    int   pos_a, pos_b;        /* 组 POS 前缀 */
    int*  tpl_ids;             /* 组内模板节点 ID */
    int   count;               /* 组内成员数 */
    float sim_mean;            /* 组内 pairwise cosine sim 均值 */
    float sim_std;             /* 组内 pairwise sim 标准差 */
    float sim_min, sim_max;    /* 组内 sim 极值 */
    int   connector_agree;     /* connector[0] 一致的数量（用于比率） */
    char  dominant_conn[TPL_CONNECTOR_BUF]; /* 最高频连接词 */
} DiagPosGroup;

/* 诊断报告 — 各维度建议 */
typedef struct {
    int   pos_pair_count;      /* 有 >=2 成员的 POS 组数 */
    int   total_templates;     /* 被分析模板总数 */
    int   safe_merge_groups;   /* 可直接合并的组数 */
    int   subcluster_groups;   /* 需要子聚类的组数 */
    int   insufficient_groups; /* POS 粒度不够的组数 */
    float overall_intra_mean;  /* 全局组内 sim 均值 */
    float overall_intra_std;   /* 全局组内 sim 标准差 */
    float worst_inter_intra;   /* 最差组间/组内 sim 比值 */
    int   recommendation;      /* 最终建议码 */
} DiagReport;

/* 阈值 */
#define DIAG_MIN_GROUP_SIZE     2     /* 组内至少2个成员才分析 */
#define DIAG_SAFE_SIM_MEAN      0.65f /* 组内均值 > 此值 → 同构 */
#define DIAG_SAFE_SIM_STD       0.15f /* 组内标准差 < 此值 → 单峰 */
#define DIAG_WARN_SIM_STD       0.25f /* 组内标准差 > 此值 → 多峰/散乱 */
#define DIAG_CONNECTOR_THRESH   0.8f  /* 连接词一致性 > 此值 → 可靠 */
#define DIAG_INTER_INTRA_RATIO  1.15f /* 组间/组内 sim 比值 > 此值 → POS 区分度不足 */

static void diag_compute_group_stats(DiagPosGroup* grp,
                                      ReasoningNode* const* tpl_nodes,
                                      int tpl_count) {
    if (grp->count < 2) { grp->sim_mean = grp->sim_std = 0.0f; return; }

    /* 收集组内特征向量指针 */
    float** feats = (float**)malloc((size_t)grp->count * sizeof(float*));
    int valid = 0;
    for (int i = 0; i < grp->count; i++) {
        int tid = grp->tpl_ids[i];
        if (tid >= 0 && tid < tpl_count) {
            ReasoningNode* tn = tpl_nodes[tid];
            if (tn && tn->features) feats[valid++] = tn->features;
        }
    }
    if (valid < 2) { free(feats); grp->sim_mean = grp->sim_std = 0.0f; return; }

    /* Pairwise 余弦相似度 */
    int npair = valid * (valid - 1) / 2;
    float* sims = (float*)malloc((size_t)npair * sizeof(float));
    int si = 0;
    grp->sim_min = 1.0f;
    grp->sim_max = -1.0f;
    double sum = 0.0;
    for (int i = 0; i < valid; i++) {
        for (int j = i + 1; j < valid; j++) {
            float s = cosine_similarity(feats[i], feats[j], NODE_FEATURE_DIM);
            sims[si++] = s;
            sum += s;
            if (s < grp->sim_min) grp->sim_min = s;
            if (s > grp->sim_max) grp->sim_max = s;
        }
    }
    grp->sim_mean = (float)(sum / (double)npair);

    /* 标准差 */
    double sqsum = 0.0;
    for (int k = 0; k < npair; k++) {
        double d = (double)sims[k] - (double)grp->sim_mean;
        sqsum += d * d;
    }
    grp->sim_std = (float)sqrt(sqsum / (double)npair);

    free(sims);
    free(feats);
}

static void diag_compute_connector_agree(DiagPosGroup* grp,
                                          ReasoningNode* const* tpl_nodes,
                                          int tpl_count) {
    typedef struct { char key[TPL_CONNECTOR_BUF]; int cnt; } ConnVote;
    ConnVote votes[16];
    int nv = 0;

    for (int i = 0; i < grp->count; i++) {
        int tid = grp->tpl_ids[i];
        const char* conn = "";
        if (tid >= 0 && tid < tpl_count) {
            ReasoningNode* tn = tpl_nodes[tid];
            if (tn && tn->tpl_connectors[0][0]) conn = tn->tpl_connectors[0];
        }
        /* 查找/创建投票 */
        int found = 0;
        for (int v = 0; v < nv; v++) {
            if (strcmp(votes[v].key, conn) == 0) { votes[v].cnt++; found = 1; break; }
        }
        if (!found && nv < 16) {
            snprintf(votes[nv].key, sizeof(votes[nv].key), "%s", conn);
            votes[nv].cnt = 1;
            nv++;
        }
    }

    /* 找最高频 */
    int best_cnt = 0;
    for (int v = 0; v < nv; v++) {
        if (votes[v].cnt > best_cnt) {
            best_cnt = votes[v].cnt;
            snprintf(grp->dominant_conn, sizeof(grp->dominant_conn), "%s", votes[v].key);
        }
    }
    grp->connector_agree = best_cnt;
}

static void diag_compute_inter_group(DiagPosGroup* groups, int gc,
                                      ReasoningNode* const* tpl_nodes,
                                      int tpl_count,
                                      DiagReport* report) {
    /* 对每对 POS 组，计算组间最近邻相似度 vs 组内均值 */
    float worst_ratio = 0.0f;

    for (int gi = 0; gi < gc; gi++) {
        if (groups[gi].count < DIAG_MIN_GROUP_SIZE) continue;

        for (int gj = gi + 1; gj < gc; gj++) {
            if (groups[gj].count < DIAG_MIN_GROUP_SIZE) continue;

            /* 组间最近邻: g_i 每个成员找 g_j 中最近的 */
            float best_inter = -1.0f;
            for (int i = 0; i < groups[gi].count; i++) {
                int ti = groups[gi].tpl_ids[i];
                if (ti < 0 || ti >= tpl_count) continue;
                ReasoningNode* ni = tpl_nodes[ti];
                if (!ni || !ni->features) continue;

                for (int j = 0; j < groups[gj].count; j++) {
                    int tj = groups[gj].tpl_ids[j];
                    if (tj < 0 || tj >= tpl_count) continue;
                    ReasoningNode* nj = tpl_nodes[tj];
                    if (!nj || !nj->features) continue;

                    float s = cosine_similarity(ni->features, nj->features, NODE_FEATURE_DIM);
                    if (s > best_inter) best_inter = s;
                }
            }

            /* g_j 每个成员找 g_i 中最近的 */
            float best_rev = -1.0f;
            for (int j = 0; j < groups[gj].count; j++) {
                int tj = groups[gj].tpl_ids[j];
                if (tj < 0 || tj >= tpl_count) continue;
                ReasoningNode* nj = tpl_nodes[tj];
                if (!nj || !nj->features) continue;

                for (int i = 0; i < groups[gi].count; i++) {
                    int ti = groups[gi].tpl_ids[i];
                    if (ti < 0 || ti >= tpl_count) continue;
                    ReasoningNode* ni = tpl_nodes[ti];
                    if (!ni || !ni->features) continue;

                    float s = cosine_similarity(nj->features, ni->features, NODE_FEATURE_DIM);
                    if (s > best_rev) best_rev = s;
                }
            }

            float inter_nn = (best_inter > best_rev) ? best_inter : best_rev;
            float intra_avg = (groups[gi].sim_mean + groups[gj].sim_mean) * 0.5f;

            if (intra_avg > 0.05f) {
                float ratio = inter_nn / intra_avg;
                if (ratio > worst_ratio) worst_ratio = ratio;
            }
        }
    }
    report->worst_inter_intra = worst_ratio;
}

int template_diagnose_pos_coherence(MasterTopology* master) {
    DiagReport report;
    memset(&report, 0, sizeof(report));

    if (!master) { fprintf(stderr, "[DIAG] master is NULL\n"); return 0; }

    SubTopology* tpl = master_get_sub_topology_by_type(master, TOPO_TEMPLATE);
    if (!tpl || !tpl->net) {
        fprintf(stderr, "[DIAG] 模板拓扑不存在\n");
        return 0;
    }

    HuarongTopologyNet* tnet = tpl->net;
    int tn = tnet->node_count;
    if (tn < 2) {
        fprintf(stderr, "[DIAG] 模板拓扑节点不足 (%d)\n", tn);
        return 0;
    }

    /* ---- 阶段 1: 收集有 POS 标签 + 特征的模板节点, 按 POS 前缀分组 ---- */

    /* 哈希桶: POS pair → 组索引（桶大小 11*11=121，加少量留白） */
    typedef struct { int pos_a, pos_b; int grp_idx; int used; } PosBucket;
    #define DIAG_POS_HASH_SIZE 151
    PosBucket* buckets = (PosBucket*)calloc(DIAG_POS_HASH_SIZE, sizeof(PosBucket));
    if (!buckets) return 0;

    /* 预分配组（最多 POS_COUNT*POS_COUNT 组） */
    int max_grps = POS_COUNT * POS_COUNT;
    DiagPosGroup* groups = (DiagPosGroup*)calloc((size_t)max_grps, sizeof(DiagPosGroup));
    if (!groups) { free(buckets); return 0; }

    /* 为每组预分配成员 ID 数组（上限 = 全部模板节点） */
    for (int g = 0; g < max_grps; g++) {
        groups[g].tpl_ids = (int*)malloc((size_t)tn * sizeof(int));
        groups[g].count = 0;
        groups[g].sim_mean = groups[g].sim_std = 0.0f;
        groups[g].dominant_conn[0] = '\0';
    }

    int gc = 0; /* 实际用到的组数 */
    int total_with_pos = 0;

    for (int i = 0; i < tn; i++) {
        ReasoningNode* node = tnet->nodes[i];
        if (!node) continue;
        if (node->tpl_pos_len < 2) continue;
        if (!node->features) continue;

        int pa = node->tpl_pos_seq[0];
        int pb = node->tpl_pos_seq[1];
        if (pa < 0 || pa >= POS_COUNT || pb < 0 || pb >= POS_COUNT) continue;

        total_with_pos++;

        /* 哈希查找 POS 组 */
        unsigned h = (unsigned)(pa * 31 + pb * 17) % DIAG_POS_HASH_SIZE;
        int gi = -1;
        for (int probe = 0; probe < DIAG_POS_HASH_SIZE; probe++) {
            unsigned idx = (h + (unsigned)probe) % DIAG_POS_HASH_SIZE;
            if (!buckets[idx].used) {
                /* 新组 */
                buckets[idx].pos_a = pa;
                buckets[idx].pos_b = pb;
                buckets[idx].grp_idx = gc;
                buckets[idx].used = 1;
                groups[gc].pos_a = pa;
                groups[gc].pos_b = pb;
                gi = gc;
                gc++;
                break;
            }
            if (buckets[idx].pos_a == pa && buckets[idx].pos_b == pb) {
                gi = buckets[idx].grp_idx;
                break;
            }
        }
        if (gi < 0 || gi >= max_grps) continue;

        groups[gi].tpl_ids[groups[gi].count] = i;
        groups[gi].count++;
    }

    free(buckets);

    report.total_templates = total_with_pos;
    report.pos_pair_count = 0;

    if (gc < 1) {
        fprintf(stderr, "[DIAG] 无可分析的 POS 组\n");
        for (int g = 0; g < max_grps; g++) free(groups[g].tpl_ids);
        free(groups);
        return 0;
    }

    /* ---- 阶段 2: 逐组计算统计 ---- */

    double global_sim_sum = 0.0;
    double global_sim_sqsum = 0.0;
    int global_npair = 0;

    for (int g = 0; g < gc; g++) {
        if (groups[g].count < DIAG_MIN_GROUP_SIZE) continue;
        report.pos_pair_count++;

        diag_compute_group_stats(&groups[g], tnet->nodes, tn);
        diag_compute_connector_agree(&groups[g], tnet->nodes, tn);

        /* 累积全局统计 */
        int np = groups[g].count * (groups[g].count - 1) / 2;
        global_sim_sum += (double)groups[g].sim_mean * (double)np;
        global_sim_sqsum += (double)(groups[g].sim_std * groups[g].sim_std) * (double)np;
        global_npair += np;

        /* 分类各组的合并建议 */
        float conn_ratio = (float)groups[g].connector_agree / (float)groups[g].count;

        int safe = (groups[g].sim_mean >= DIAG_SAFE_SIM_MEAN &&
                    groups[g].sim_std  <= DIAG_SAFE_SIM_STD &&
                    conn_ratio >= DIAG_CONNECTOR_THRESH);
        int need_sub = (groups[g].sim_std > DIAG_WARN_SIM_STD);
        /* need_sub 优先于 safe（高方差 + 高均值也可能需要子聚类） */

        if (need_sub) report.subcluster_groups++;
        else if (safe) report.safe_merge_groups++;
        else report.insufficient_groups++;
    }

    if (global_npair > 0) {
        report.overall_intra_mean = (float)(global_sim_sum / (double)global_npair);
        report.overall_intra_std  = (float)sqrt(global_sim_sqsum / (double)global_npair);
    }

    /* ---- 阶段 3: 组间重合度 ---- */

    diag_compute_inter_group(groups, gc, tnet->nodes, tn, &report);

    /* ---- 阶段 4: 诊断报告 ---- */

    fprintf(stderr, "\n");
    fprintf(stderr, "╔══════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║ 模板 POS 槽位化诊断报告                                    ║\n");
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║ 总模板节点: %-5d  有POS+特征: %-5d  POS组数: %-3d       ║\n",
            tn, report.total_templates, report.pos_pair_count);
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║ POS组   组内sim均值  sim标准差  连接词一致性  建议         ║\n");
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════╣\n");

    for (int g = 0; g < gc; g++) {
        if (groups[g].count < DIAG_MIN_GROUP_SIZE) continue;
        const char* pa_name = diag_pos_name(groups[g].pos_a);
        const char* pb_name = diag_pos_name(groups[g].pos_b);
        float cr = (float)groups[g].connector_agree / (float)groups[g].count;
        const char* tag;
        if (groups[g].sim_std > DIAG_WARN_SIM_STD)
            tag = "需子聚类";
        else if (groups[g].sim_mean >= DIAG_SAFE_SIM_MEAN && groups[g].sim_std <= DIAG_SAFE_SIM_STD && cr >= DIAG_CONNECTOR_THRESH)
            tag = "可纯POS合并";
        else
            tag = "粒度不够";

        fprintf(stderr, "║ [%s][%s]  %5.2f       %5.3f       %5.1f%% (%s)  %-14s║\n",
                pa_name, pb_name,
                groups[g].sim_mean, groups[g].sim_std,
                cr * 100.0f,
                groups[g].dominant_conn[0] ? groups[g].dominant_conn : "无",
                tag);
    }

    fprintf(stderr, "╠══════════════════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║ 全局组内 sim 均值: %.3f  标准差: %.3f                      ║\n",
            report.overall_intra_mean, report.overall_intra_std);
    fprintf(stderr, "║ 组间/组内 sim 比值 (最大): %.3f                           ║\n",
            report.worst_inter_intra);
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════╣\n");

    /* 建议决策 */
    if (report.pos_pair_count == 0) {
        report.recommendation = 0;
        fprintf(stderr, "║ 建议: 数据不足，推迟决策                                  ║\n");
    } else if (report.safe_merge_groups >= report.pos_pair_count * 0.7f
               && report.worst_inter_intra < DIAG_INTER_INTRA_RATIO) {
        report.recommendation = DIAG_POS_SAFE_MERGE;
        fprintf(stderr, "║ 建议: 纯POS合并 — 70%%+组同构，组间区分度良好              ║\n");
    } else if (report.subcluster_groups > 0
               && report.overall_intra_mean > 0.5f) {
        report.recommendation = DIAG_POS_WITH_SUBCLUSTER;
        fprintf(stderr, "║ 建议: POS+特征子聚类 — 组内多峰但POS仍有区分力             ║\n");
    } else {
        report.recommendation = DIAG_POS_INSUFFICIENT;
        fprintf(stderr, "║ 建议: POS粒度不够 — 需要额外信号（边共现/拓扑上下文）     ║\n");
    }

    fprintf(stderr, "╠══════════════════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║ 可纯POS合并: %-3d组   需子聚类: %-3d组   粒度不够: %-3d组  ║\n",
            report.safe_merge_groups, report.subcluster_groups,
            report.insufficient_groups);
    fprintf(stderr, "╚══════════════════════════════════════════════════════════════╝\n");
    fprintf(stderr, "\n");

    /* 清理 */
    for (int g = 0; g < max_grps; g++) free(groups[g].tpl_ids);
    free(groups);

    return report.recommendation;
}

/* ================================================================
 *  模板 POS 结构合并 — 统一 Pipeline A 和 Pipeline B 的模板产出
 *
 *  合并策略（批量合并 — 选项 b）：
 *    - 扫描 TOPO_TEMPLATE 中所有模板节点
 *    - 按合并键 = (tpl_pos_len, tpl_pos_seq, 归一化 tpl_connectors) 分组
 *    - 同组内保留 confidence 最高的为 survivor
 *    - Survivor 特征向量 = 组内所有成员加权平均（权重 = 各自 confidence）
 *    - Survivor 置信度 = 组内均值
 *    - 其他成员软删除: tpl_pos_len = 0, confidence = 0
 *
 *  下游匹配引擎零改动：master_find_template_for_pair_nolock
 *  只扫描一种模板格式，自动跳过 tpl_pos_len < 2 的节点。
 *
 *  @param master  主拓扑
 *  @return 合并的组数 (每组 ≥2 个成员才触发合并)
 * ================================================================ */

#define MERGE_HASH_SIZE 1021  /* 质数，足够容纳 ~300 个模板 */

int template_merge_by_pos_structure(MasterTopology* master) {
    if (!master) return 0;

    SubTopology* tpl = master_get_sub_topology_by_type(master, TOPO_TEMPLATE);
    if (!tpl || !tpl->net) return 0;

    HuarongTopologyNet* tnet = tpl->net;
    int tn = tnet->node_count;
    if (tn < 2) return 0;

    /* 哈希表分组结构 */
    typedef struct {
        int   pos_len;
        int   pos_seq[4];
        char  connectors[4][TPL_CONNECTOR_BUF];  /* 已归一化 */
        int*  member_ids;    /* tnet->nodes[] 索引 */
        int   member_count;
        int   member_cap;
        int   used;
    } MergeGroup;

    MergeGroup* groups = (MergeGroup*)calloc(MERGE_HASH_SIZE, sizeof(MergeGroup));
    if (!groups) return 0;

    /* ---- Pass 1: 按合并键分组 ---- */
    int total_groups = 0;

    for (int i = 0; i < tn; i++) {
        ReasoningNode* node = tnet->nodes[i];
        if (!node) continue;
        if (node->tpl_pos_len < 2) continue;
        if (!node->features) continue;

        uint32_t h = merge_key_hash(node->tpl_pos_len,
                                     node->tpl_pos_seq,
                                     node->tpl_connectors);
        uint32_t idx = h % MERGE_HASH_SIZE;

        int found = 0;
        for (int probe = 0; probe < MERGE_HASH_SIZE; probe++) {
            uint32_t cur = (idx + (uint32_t)probe) % MERGE_HASH_SIZE;

            if (!groups[cur].used) {
                /* 新组 */
                groups[cur].pos_len = node->tpl_pos_len;
                memcpy(groups[cur].pos_seq, node->tpl_pos_seq,
                       sizeof(int) * 4);
                for (int k = 0; k < node->tpl_pos_len - 1; k++) {
                    norm_connector(node->tpl_connectors[k],
                                   groups[cur].connectors[k]);
                }
                /* 确保未使用的 connector 槽位为空 */
                for (int k = node->tpl_pos_len - 1; k < 4; k++) {
                    groups[cur].connectors[k][0] = '\0';
                }
                groups[cur].member_cap = 8;
                groups[cur].member_ids = (int*)malloc(
                    (size_t)groups[cur].member_cap * sizeof(int));
                if (!groups[cur].member_ids) {
                    groups[cur].used = 0;
                    break;
                }
                groups[cur].member_ids[0] = i;
                groups[cur].member_count = 1;
                groups[cur].used = 1;
                total_groups++;
                found = 1;
                break;
            }

            if (merge_key_equals(
                    groups[cur].pos_len, groups[cur].pos_seq,
                    (const char(*)[TPL_CONNECTOR_BUF])groups[cur].connectors,
                    node->tpl_pos_len, node->tpl_pos_seq,
                    node->tpl_connectors))
            {
                /* 加入已有组 */
                if (groups[cur].member_count >= groups[cur].member_cap) {
                    groups[cur].member_cap *= 2;
                    int* new_ids = (int*)realloc(groups[cur].member_ids,
                        (size_t)groups[cur].member_cap * sizeof(int));
                    if (!new_ids) break;  /* 扩容失败，跳过此节点 */
                    groups[cur].member_ids = new_ids;
                }
                groups[cur].member_ids[groups[cur].member_count++] = i;
                found = 1;
                break;
            }
        }
        /* found==0: 哈希表满或扩容失败，安全跳过 */
        (void)found;
    }

    /* ---- Pass 2: 合并组内成员 ---- */
    int merged_groups = 0;
    int merged_nodes  = 0;

    for (int gi = 0; gi < MERGE_HASH_SIZE; gi++) {
        if (!groups[gi].used) continue;
        if (groups[gi].member_count < 2) continue;

        /* 选 survivor: confidence 最高者 */
        int survivor_idx = groups[gi].member_ids[0];
        float survivor_conf = tnet->nodes[survivor_idx]->confidence;
        for (int m = 1; m < groups[gi].member_count; m++) {
            int mid = groups[gi].member_ids[m];
            float mc = tnet->nodes[mid]->confidence;
            if (mc > survivor_conf) {
                survivor_idx = mid;
                survivor_conf = mc;
            }
        }

        ReasoningNode* survivor = tnet->nodes[survivor_idx];

        /* 重新计算特征向量: 组内所有成员加权平均 */
        float sum_weight = 0.0f;
        float new_feat[NODE_FEATURE_DIM];
        memset(new_feat, 0, sizeof(new_feat));

        for (int m = 0; m < groups[gi].member_count; m++) {
            int mid = groups[gi].member_ids[m];
            ReasoningNode* mn = tnet->nodes[mid];
            if (!mn || !mn->features) continue;
            float w = (mn->confidence > 0.01f) ? mn->confidence : 0.01f;
            for (int d = 0; d < NODE_FEATURE_DIM; d++) {
                new_feat[d] += mn->features[d] * w;
            }
            sum_weight += w;
        }

        if (sum_weight > 0.0f) {
            float inv_w = 1.0f / sum_weight;
            for (int d = 0; d < NODE_FEATURE_DIM; d++) {
                new_feat[d] *= inv_w;
            }
            memcpy(survivor->features, new_feat,
                   (size_t)NODE_FEATURE_DIM * sizeof(float));
        }

        /* 更新 survivor 置信度: 取组内最大值
         *
         * 不用均值的原因：均值会稀释高置信度模板。
         * 例如 A survivor conf=0.8 + B 同 POS 模板 conf=0.6 → 均值 0.7，
         * 但 B 是独立确认同一结构的证据，不应拉低置信度。
         * 取 max 保证 survivor 保持其锚点置信度不被稀释。 */
        survivor->confidence = survivor_conf;  /* 已是最高的 */

        /* 软删除其他成员: tpl_pos_len=0 使下游自动跳过 */
        for (int m = 0; m < groups[gi].member_count; m++) {
            int mid = groups[gi].member_ids[m];
            if (mid == survivor_idx) continue;
            ReasoningNode* mn = tnet->nodes[mid];
            mn->tpl_pos_len = 0;
            mn->confidence   = 0.0f;
            merged_nodes++;
        }

        merged_groups++;
    }

    /* ---- 清理 ---- */
    for (int gi = 0; gi < MERGE_HASH_SIZE; gi++) {
        free(groups[gi].member_ids);
    }
    free(groups);

    if (merged_groups > 0) {
        fprintf(stderr,
            "[TEMPLATE-MERGE] 合并 %d 组, 软删除 %d 个冗余模板节点\n",
            merged_groups, merged_nodes);
    }

    return merged_groups;
}



