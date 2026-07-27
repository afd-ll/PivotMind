#include "cross_edge_io.h"
#include "huarong_topology.h"
#include "node_hash.h"
#include "common.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int save_cross_edges(MasterTopology* master, const char* filepath) {
    if (!master || !filepath) return -1;

    int count = master->cross_link_count;
    if (count <= 0) return -1;

    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", filepath);

    FILE* fp = fopen(tmp_path, "wb");
    if (!fp) return -1;

    uint32_t magic = CROSS_EDGE_FILE_MAGIC;
    /* 先写魔术数，count 后续补写（因为 NULL link 会被跳过） */

    if (fwrite(&magic, sizeof(uint32_t), 1, fp) != 1) { fclose(fp); remove(tmp_path); return -1; }
    /* 预留 count 位置 */
    long count_pos = ftell(fp);
    uint32_t placeholder = 0;
    if (fwrite(&placeholder, sizeof(uint32_t), 1, fp) != 1) { fclose(fp); remove(tmp_path); return -1; }

    int written = 0;
    for (int i = 0; i < count; i++) {
        CrossTopologyLink* link = master->cross_links[i];
        if (!link) continue;

        uint32_t from_topo = (uint32_t)link->from_topo_id;
        uint32_t from_node = (uint32_t)link->from_node_id;
        uint32_t to_topo   = (uint32_t)link->to_topo_id;
        uint32_t to_node   = (uint32_t)link->to_node_id;
        float    weight    = link->weight;
        uint32_t use_cnt   = (uint32_t)link->use_count;

        if (fwrite(&from_topo, sizeof(uint32_t), 1, fp) != 1) goto write_fail;
        if (fwrite(&from_node, sizeof(uint32_t), 1, fp) != 1) goto write_fail;
        if (fwrite(&to_topo,   sizeof(uint32_t), 1, fp) != 1) goto write_fail;
        if (fwrite(&to_node,   sizeof(uint32_t), 1, fp) != 1) goto write_fail;
        if (fwrite(&weight,    sizeof(float),    1, fp) != 1) goto write_fail;
        if (fwrite(&use_cnt,   sizeof(uint32_t), 1, fp) != 1) goto write_fail;
        written++;
    }

    /* 回写实际的记录数 */
    fseek(fp, count_pos, SEEK_SET);
    uint32_t actual = (uint32_t)written;
    fwrite(&actual, sizeof(uint32_t), 1, fp);

    fclose(fp);

    if (rename(tmp_path, filepath) != 0) {
        remove(tmp_path);
        return -1;
    }

    return written;

write_fail:
    fclose(fp);
    remove(tmp_path);
    return -1;
}

int load_cross_edges(MasterTopology* master, const char* filepath) {
    if (!master || !filepath) return -1;

    FILE* fp = fopen(filepath, "rb");
    if (!fp) return -1;

    uint32_t magic, m;
    if (fread(&magic, sizeof(uint32_t), 1, fp) != 1) { fclose(fp); return -1; }
    if (magic != CROSS_EDGE_FILE_MAGIC) { fclose(fp); return -1; }
    if (fread(&m, sizeof(uint32_t), 1, fp) != 1) { fclose(fp); return -1; }
    if (m > 10000000) { fclose(fp); return -1; }  // anti-corruption max
    int loaded = 0;
    for (uint32_t i = 0; i < m; i++) {
        uint32_t from_topo, from_node, to_topo, to_node, use_cnt;
        float weight;

        if (fread(&from_topo, sizeof(uint32_t), 1, fp) != 1) break;
        if (fread(&from_node, sizeof(uint32_t), 1, fp) != 1) break;
        if (fread(&to_topo,   sizeof(uint32_t), 1, fp) != 1) break;
        if (fread(&to_node,   sizeof(uint32_t), 1, fp) != 1) break;
        if (fread(&weight,    sizeof(float),    1, fp) != 1) break;
        if (fread(&use_cnt,   sizeof(uint32_t), 1, fp) != 1) break;

        // validate topology type range
        if (from_topo >= (uint32_t)master->sub_topo_count ||
            to_topo   >= (uint32_t)master->sub_topo_count) {
            continue;
        }

        // validate node exists
        SubTopology* from_sub = master->sub_topologies[from_topo];
        SubTopology* to_sub   = master->sub_topologies[to_topo];
        if (!from_sub || !from_sub->net || !to_sub || !to_sub->net) continue;

        if ((int)from_node >= from_sub->net->node_count ||
            (int)to_node   >= to_sub->net->node_count) {
            continue;
        }

        if (!from_sub->net->nodes[from_node] || !to_sub->net->nodes[to_node]) continue;

        int result = master_add_cross_link(master,
                                           (int)from_topo, (int)from_node,
                                           (int)to_topo,   (int)to_node,
                                           weight, "persisted");

        if (result == 0) {
            for (int c = 0; c < master->cross_link_count; c++) {
                CrossTopologyLink* link = master->cross_links[c];
                if (link &&
                    link->from_topo_id == (int)from_topo &&
                    link->from_node_id == (int)from_node &&
                    link->to_topo_id   == (int)to_topo &&
                    link->to_node_id   == (int)to_node) {
                    link->use_count = (int)use_cnt;
                    break;
                }
            }
            loaded++;
        }
    }

    fclose(fp);
    return loaded;
}

// ==================== 鍐呴儴杈呭姪锛氬湪涓や釜鎷撴墤涔嬮棿鎸夋蹇靛悕寤鸿法杩炴帴 ====================

/**
 * 鍦ㄤ袱涓瓙鎷撴墤涔嬮棿鎸夌簿纭蹇靛悕鍖归厤寤虹珛璺ㄦ嫇鎵戣繛锟? * @param master 涓绘嫇锟? * @param from_sub 婧愭嫇锟? * @param to_sub 鐩爣鎷撴墤
 * @param weight 杩炴帴鏉冮噸
 * @param relation 鍏崇郴绫诲瀷鏍囩
 * @param bidirectional 鏄惁鍙屽悜
 * @param max_connections 鏈€澶ц繛鎺ユ暟锟?=鏃犻檺鍒讹級
 * @return number of connections created
 */
static int link_topos_by_name(MasterTopology* master,
                              SubTopology* from_sub,
                              SubTopology* to_sub,
                              float weight,
                              const char* relation,
                              int bidirectional,
                              int max_connections) {
    if (!master || !from_sub || !from_sub->net || !to_sub || !to_sub->net) return 0;

    int created = 0;
    int limit = (max_connections > 0) ? max_connections : 1000000;

    for (int fi = 0; fi < from_sub->net->node_count && created < limit; fi++) {
        ReasoningNode* fn = from_sub->net->nodes[fi];
        if (!fn || !fn->concept) continue;

        /* use hash table for O(1) lookup instead of O(n) inner loop */
        if (to_sub->node_hash) {
            ReasoningNode* tn = node_hash_find(to_sub->node_hash, fn->concept);
            if (tn && tn->node_id >= 0) {
                if (fi == tn->node_id && from_sub->type == to_sub->type) continue;

                int ret = master_add_cross_link(master,
                    from_sub->topo_id, fn->node_id,
                    to_sub->topo_id, tn->node_id,
                    weight, relation);
                if (ret >= 0) created++;

                if (bidirectional) {
                    if (master_add_cross_link(master,
                        to_sub->topo_id, tn->node_id,
                        from_sub->topo_id, fn->node_id,
                        weight * 0.8f, relation) >= 0) {
                        created++;
                    }
                }
            }
        } else {
            /* fallback: linear scan (shouldn't happen if hash is properly initialized) */
            for (int ti = 0; ti < to_sub->net->node_count && created < limit; ti++) {
                ReasoningNode* tn = to_sub->net->nodes[ti];
                if (!tn || !tn->concept) continue;
                if (fi == ti && from_sub->type == to_sub->type) continue;
                if (strcmp_null(fn->concept, tn->concept) == 0) {
                    int ret = master_add_cross_link(master,
                        from_sub->topo_id, fn->node_id,
                        to_sub->topo_id, tn->node_id,
                        weight, relation);
                    if (ret >= 0) created++;
                    if (bidirectional) {
                        if (master_add_cross_link(master,
                            to_sub->topo_id, tn->node_id,
                            from_sub->topo_id, fn->node_id,
                            weight * 0.8f, relation) >= 0) {
                            created++;
                        }
                    }
                }
            }
        }
    }
    return created;
}

/**
 * 鍦ㄤ袱涓瓙鎷撴墤涔嬮棿鎸夊瓙涓插尮閰嶏紙strstr锛夊缓绔嬭法鎷撴墤杩炴帴
 * 浠呬綔涓虹簿纭尮閰嶇殑琛ュ厖
 */
static int link_topos_by_substr(MasterTopology* master,
                                SubTopology* from_sub,
                                SubTopology* to_sub,
                                float weight,
                                const char* relation,
                                int max_connections) {
    if (!master || !from_sub || !from_sub->net || !to_sub || !to_sub->net) return 0;

    int created = 0;
    int limit = (max_connections > 0) ? max_connections : 5000;

    for (int fi = 0; fi < from_sub->net->node_count && created < limit; fi++) {
        ReasoningNode* fn = from_sub->net->nodes[fi];
        if (!fn || !fn->concept || strlen(fn->concept) < 2) continue;

        for (int ti = 0; ti < to_sub->net->node_count && created < limit; ti++) {
            ReasoningNode* tn = to_sub->net->nodes[ti];
            if (!tn || !tn->concept) continue;

            // avoid duplicate with exact match
            if (strcmp_null(fn->concept, tn->concept) == 0) continue;

                if (strstr(tn->concept, fn->concept) || strstr(fn->concept, tn->concept)) {
                // fast lookup
                if (cross_link_exists(master,
                        from_sub->topo_id, fn->node_id,
                        to_sub->topo_id, tn->node_id)) continue;

                int ret = master_add_cross_link(master,
                    from_sub->topo_id, fn->node_id,
                    to_sub->topo_id, tn->node_id,
                    weight * 0.6f, relation);
                if (ret >= 0) created++;
            }
        }
    }
    return created;
}

/**
 * 鍦ㄤ袱涓嫇鎵戜箣闂存寜鐗瑰緛鍚戦噺浣欏鸡鐩镐技搴﹀缓绔嬭法杩炴帴
 * requires both nodes to have non-null feature vectors
 */
static int link_topos_by_features(MasterTopology* master,
                                  SubTopology* from_sub,
                                  SubTopology* to_sub,
                                  float threshold,
                                  float weight,
                                  const char* relation,
                                  int max_per_node) {
    if (!master || !from_sub || !from_sub->net || !to_sub || !to_sub->net) return 0;
    if (threshold <= 0.0f) threshold = 0.35f;
    if (max_per_node <= 0) max_per_node = 3;

    int created = 0;

    for (int fi = 0; fi < from_sub->net->node_count; fi++) {
        ReasoningNode* fn = from_sub->net->nodes[fi];
        if (!fn || !fn->features || fn->feature_dim <= 0) continue;

        // Top-N selection
        typedef struct { int idx; float sim; } SimEntry;
        SimEntry top[16];
        int top_count = 0;
        int max_t = max_per_node > 16 ? 16 : max_per_node;

        for (int ti = 0; ti < to_sub->net->node_count; ti++) {
            ReasoningNode* tn = to_sub->net->nodes[ti];
            if (!tn || !tn->features || tn->feature_dim != fn->feature_dim) continue;
            if (fn->concept && tn->concept &&
                strcmp_null(fn->concept, tn->concept) == 0) continue; // already exact matched

            float sim = cosine_similarity(fn->features, tn->features, fn->feature_dim);
            if (isnan(sim) || isinf(sim)) sim = 0.0f;
            if (sim <= 0.0f || sim < threshold) continue;

            if (top_count < max_t) {
                top[top_count].idx = ti;
                top[top_count].sim = sim;
                top_count++;
            } else {
                int min_idx = 0;
                for (int k = 1; k < top_count; k++) {
                    if (top[k].sim < top[min_idx].sim) min_idx = k;
                }
                if (sim > top[min_idx].sim) {
                    top[min_idx].idx = ti;
                    top[min_idx].sim = sim;
                }
            }
        }

        for (int p = 0; p < top_count; p++) {
            ReasoningNode* tn = to_sub->net->nodes[top[p].idx];
            if (!tn) continue;

            // fast lookup
            if (cross_link_exists(master,
                    from_sub->topo_id, fn->node_id,
                    to_sub->topo_id, tn->node_id)) continue;

            float w = weight + 0.5f * top[p].sim;
            if (w > 1.0f) w = 1.0f;
            int ret = master_add_cross_link(master,
                from_sub->topo_id, fn->node_id,
                to_sub->topo_id, tn->node_id,
                w, relation);
            if (ret >= 0) created++;
        }
    }
    return created;
}

// ==================== 鏅鸿兘璇箟鏄犲皠锛堣法鎷撴墤鏄犲皠鍏崇郴琛級 ====================

/**
 * 鎷撴墤瀵归厤缃細涓や釜鎷撴墤绫诲瀷 + 鍏崇郴鏍囩 + 鏉冮噸 + 鍖归厤绛栫暐
 */
typedef struct {
    TopologyType from_type;   /* source topology type */
    TopologyType to_type;     /* target topology type */
    const char* relation;     /* relationship label */
    float weight;             /* base weight */
    int bidirectional;        /* whether bidirectional */
    int use_substr;           /* enable substring matching (after exact) */
    int use_features;         /* enable feature vector matching */
} TopoPairConfig;

/**
 * 鎵€鏈夋湁鎰忎箟鐨勬嫇鎵戝
 * 瑕嗙洊鎵€锟?9 涓瓙鎷撴墤涔嬮棿鐨勫悎鐞嗚繛锟? */
static const TopoPairConfig TOPO_PAIRS[] = {
    // vocab -> semantic: concept mapping (core)    {TOPO_VOCABULARY, TOPO_SEMANTIC,  "姒傚康鏄犲皠",  0.60f, 1, 1, 1},
    // vocab -> emotion: emotion relation    {TOPO_VOCABULARY, TOPO_EMOTION,   "鎯呯华鍏宠仈",  0.50f, 1, 1, 1},
    // vocab -> concept: concept relation    {TOPO_VOCABULARY, TOPO_CONCEPT,   "姒傚康鍏宠仈",  0.55f, 1, 1, 1},
    // vocab -> syntax: POS tag    {TOPO_VOCABULARY, TOPO_SYNTAX,    "璇嶆€ф爣锟?,  0.40f, 0, 1, 0},
    // 璇嶆眹 锟?涓婁笅鏂囷細璇鍏宠仈
    {TOPO_VOCABULARY, TOPO_CONTEXT,   "璇鍏宠仈",  0.45f, 0, 1, 0},
    // 璇嶆眹 锟?棰嗗煙锛氶鍩熷綊锟?    {TOPO_VOCABULARY, TOPO_DOMAIN,    "棰嗗煙褰掔被",  0.40f, 1, 1, 0},
    // 璇嶆眹 锟?璇敤锛氳鐢ㄥ惈锟?    {TOPO_VOCABULARY, TOPO_PRAGMA,    "璇敤鍏宠仈",  0.40f, 0, 1, 0},
    // 璇嶆眹 锟?鏂囧寲锛氭枃鍖栨槧锟?    {TOPO_VOCABULARY, TOPO_CULTURE,   "鏂囧寲鏄犲皠",  0.40f, 0, 1, 0},

    // 璇箟 锟?鎯呯华锛氭儏鎰熻锟?    {TOPO_SEMANTIC,   TOPO_EMOTION,   "鎯呮劅璇箟",  0.45f, 1, 0, 1},
    // 璇箟 锟?姒傚康锛氳涔夋蹇靛寲
    {TOPO_SEMANTIC,   TOPO_CONCEPT,   "璇箟姒傚康",  0.50f, 1, 1, 1},
    // 璇箟 锟?涓婁笅鏂囷細璇箟涓婁笅锟?    {TOPO_SEMANTIC,   TOPO_CONTEXT,   "璇箟璇",  0.40f, 1, 0, 0},
    // 璇箟 锟?棰嗗煙锛氳涔夐锟?    {TOPO_SEMANTIC,   TOPO_DOMAIN,    "璇箟棰嗗煙",  0.40f, 1, 0, 0},

    // emotion -> concept
    {TOPO_EMOTION,    TOPO_CONCEPT,   "emotion-concept", 0.40f, 1, 0, 0},
    // emotion -> culture
    {TOPO_EMOTION,    TOPO_CULTURE,   "emotion-culture", 0.35f, 1, 0, 0},

    // concept -> syntax
    {TOPO_CONCEPT,    TOPO_SYNTAX,    "concept-syntax", 0.35f, 0, 0, 0},
    // concept -> domain
    {TOPO_CONCEPT,    TOPO_DOMAIN,    "concept-domain", 0.40f, 1, 0, 0},
    // concept -> culture
    {TOPO_CONCEPT,    TOPO_CULTURE,   "concept-culture", 0.40f, 1, 0, 0},

    // context -> domain
    {TOPO_CONTEXT,    TOPO_DOMAIN,    "context-domain", 0.35f, 1, 0, 0},
    // context -> pragma
    {TOPO_CONTEXT,    TOPO_PRAGMA,   "context-pragma", 0.35f, 1, 0, 0},

    // pragma -> culture
    {TOPO_PRAGMA,     TOPO_CULTURE,   "pragma-culture", 0.30f, 1, 0, 0},
    // syntax -> pragma
    {TOPO_SYNTAX,     TOPO_PRAGMA,   "syntax-pragma", 0.30f, 0, 0, 0},
};

#define NUM_TOPO_PAIRS (sizeof(TOPO_PAIRS) / sizeof(TOPO_PAIRS[0]))

// ========== batch cross connection creation ==========

/**
 * 鍚戠洰鏍囧瓙鎷撴墤涓ˉ鍏呯己澶辩殑鑺傜偣锛堜粠婧愭嫇鎵戠殑鍚屽悕鑺傜偣澶嶅埗锟? * 鐢ㄤ簬纭繚鐩爣鎷撴墤涓瓨鍦ㄤ笌婧愭嫇鎵戝搴旂殑鑺傜偣
 * @return 鍒涘缓鐨勮妭鐐规暟
 */
static int ensure_nodes_in_target(MasterTopology* master,
                                  SubTopology* source,
                                  SubTopology* target) {
    if (!master || !source || !source->net || !target || !target->net) return 0;
    int created = 0;

    for (int si = 0; si < source->net->node_count; si++) {
        ReasoningNode* sn = source->net->nodes[si];
        if (!sn || !sn->concept) continue;

        // accelerated lookup via node_hash
        ReasoningNode* existing = NULL;
        if (target->node_hash) {
            existing = node_hash_find(target->node_hash, sn->concept);
        } else {
            for (int ti = 0; ti < target->net->node_count; ti++) {
                ReasoningNode* tn = target->net->nodes[ti];
                if (tn && tn->concept && strcmp_null(tn->concept, sn->concept) == 0) {
                    existing = tn;
                    break;
                }
            }
        }
        if (existing) continue;

        ReasoningNode* new_node = huarong_net_add_node(target->net, sn->concept, NULL, 0);
        if (new_node) {
            if (target->node_hash) {
                node_hash_add(target->node_hash, new_node);
            }
            created++;
        }
    }
    return created;
}

// ==================== 涓婚噸寤篈PI ====================

int rebuild_cross_connections(MasterTopology* master) {
    if (!master) return 0;

    // clear old cross connections (full rebuild)
    master_clear_cross_links(master);

    int total_created = 0;

    // step 1: ensure target topologies have same-named nodes
    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    if (!vocab || !vocab->net) {
        printf("[cross-topo] vocab topology unavailable, skip\n");
        return 0;
    }

    printf("[cross-topo] rebuilding cross connections...\n");
    printf("[cross-topo] vocab topology: %d nodes\n", vocab->net->node_count);

    // fill vocab into semantic/emotion/concept/template topologies
    const TopologyType seed_types[] = {
        TOPO_SEMANTIC, TOPO_EMOTION, TOPO_CONCEPT,
        TOPO_SYNTAX, TOPO_CONTEXT, TOPO_DOMAIN,
        TOPO_PRAGMA, TOPO_CULTURE, TOPO_TEMPLATE
    };
    int num_seed_types = sizeof(seed_types) / sizeof(seed_types[0]);

    for (int s = 0; s < num_seed_types; s++) {
        SubTopology* tgt = master_get_sub_topology_by_type(master, seed_types[s]);
        if (!tgt || !tgt->net) continue;
        int added = ensure_nodes_in_target(master, vocab, tgt);
        if (added > 0) {
            printf("[cross-topo]   %s: 琛ュ厖 %d nodes added\n",
                   TOPOLOGY_TYPE_NAMES[seed_types[s]], added);
        }
    }

    // step 2: build cross connections between each pair
    for (int p = 0; p < (int)NUM_TOPO_PAIRS; p++) {
        const TopoPairConfig* cfg = &TOPO_PAIRS[p];

        SubTopology* from_sub = master_get_sub_topology_by_type(master, cfg->from_type);
        SubTopology* to_sub   = master_get_sub_topology_by_type(master, cfg->to_type);
        if (!from_sub || !from_sub->net || !to_sub || !to_sub->net) continue;

        // 绮剧‘鍚嶇О鍖归厤
        int exact = link_topos_by_name(master, from_sub, to_sub,
                                       cfg->weight, cfg->relation,
                                       cfg->bidirectional, 0);
        total_created += exact;

        // 瀛愪覆鍖归厤锛堝彲閫夛級
        int substr = 0;
        if (cfg->use_substr && exact < 50000) {
            substr = link_topos_by_substr(master, from_sub, to_sub,
                                          cfg->weight, cfg->relation, 20000);
            total_created += substr;
        }

        // 鐗瑰緛鍚戦噺鍖归厤锛堝彲閫夛級
        int feat = 0;
        if (cfg->use_features) {
            feat = link_topos_by_features(master, from_sub, to_sub,
                                          0.35f, cfg->weight, cfg->relation, 3);
            total_created += feat;
        }

        if (exact > 0 || substr > 0 || feat > 0) {
            printf("[跨拓扑] %s→%s: exact=%d substr=%d feat=%d\n",
                   TOPOLOGY_TYPE_NAMES[cfg->from_type],
                   TOPOLOGY_TYPE_NAMES[cfg->to_type],
                   exact, substr, feat);
        }
    }

    printf("[cross-topo] rebuild complete: %d cross connections\n", total_created);
    return total_created;
}

/**
 * 蹇嵎鍑芥暟锛氬湪鎸囧畾鎷撴墤涔嬮棿涓烘柊娣诲姞鐨勮妭鐐硅嚜鍔ㄥ缓绔嬭法杩炴帴
 * 锟?autonomic_learn_from_dialog 鍦ㄦ瘡娆″璇濆悗璋冪敤
 * 浠呭鐞嗗綋鍓嶆椿璺冪殑鑺傜偣锛岄伩鍏嶅叏閲忛噸锟? */
int auto_link_activated_nodes(MasterTopology* master,
                              const char** concepts, int concept_count) {
    if (!master || !concepts || concept_count <= 0) return 0;

    // 鎵惧埌鎵€鏈夊寘鍚繖浜涙蹇电殑鎷撴墤
    SubTopology* active_topos[16];
    int active_count = 0;

    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net || !sub->node_hash) continue;

        // check if at least one concept exists
        int has_any = 0;
        for (int c = 0; c < concept_count; c++) {
            ReasoningNode* node = node_hash_find(sub->node_hash, concepts[c]);
            if (node) { has_any = 1; break; }
        }
        if (has_any) {
            active_topos[active_count++] = sub;
        }
    }

    if (active_count < 2) return 0;

    // create cross connections between active topology pairs
    int created = 0;

    for (int a = 0; a < active_count; a++) {
        for (int b = a + 1; b < active_count; b++) {
            SubTopology* ta = active_topos[a];
            SubTopology* tb = active_topos[b];

            for (int ci = 0; ci < concept_count; ci++) {
                ReasoningNode* na = node_hash_find(ta->node_hash, concepts[ci]);
                ReasoningNode* nb = node_hash_find(tb->node_hash, concepts[ci]);
                if (!na || !nb) continue;

                // fast lookup
                if (cross_link_exists(master,
                        ta->topo_id, na->node_id,
                        tb->topo_id, nb->node_id)) continue;

                // determine relation label
                const char* relation = "co-activation";
                if ((ta->type == TOPO_VOCABULARY && tb->type == TOPO_SEMANTIC) ||
                    (ta->type == TOPO_SEMANTIC && tb->type == TOPO_VOCABULARY)) {
                    relation = "concept-mapping";
                } else if ((ta->type == TOPO_VOCABULARY && tb->type == TOPO_EMOTION) ||
                           (ta->type == TOPO_EMOTION && tb->type == TOPO_VOCABULARY)) {
                    relation = "emotion-assoc";
                } else if ((ta->type == TOPO_VOCABULARY && tb->type == TOPO_CONCEPT) ||
                           (ta->type == TOPO_CONCEPT && tb->type == TOPO_VOCABULARY)) {
                    relation = "concept-assoc";
                }

                int ret = master_add_cross_link(master,
                    ta->topo_id, na->node_id,
                    tb->topo_id, nb->node_id,
                    0.4f, relation);
                if (ret >= 0) created++;
            }
        }
    }

    // === 澧炲己锛氫笉鍚屾蹇电殑鑺傜偣涔嬮棿鎸夌壒寰佺浉浼煎害寤鸿法锟?===
    // 鍦ㄥ悓涓€QA婵€娲荤殑涓嶅悓鎷撴墤涓紝濡傛灉鑺傜偣鐗瑰緛鍚戦噺浣欏鸡鐩镐技搴﹁秴杩囬槇鍊煎垯寤虹珛杩炴帴
    {
        int feat_created = 0;
        for (int a = 0; a < active_count && feat_created < 20; a++) {
            for (int b = a + 1; b < active_count && feat_created < 20; b++) {
                SubTopology* ta = active_topos[a];
                SubTopology* tb = active_topos[b];

                for (int ci = 0; ci < concept_count && feat_created < 20; ci++) {
                    ReasoningNode* na = node_hash_find(ta->node_hash, concepts[ci]);
                    if (!na || !na->features || na->feature_dim <= 0) continue;

                    for (int cj = ci + 1; cj < concept_count && feat_created < 20; cj++) {
                        if (strcmp(concepts[ci], concepts[cj]) == 0) continue;

                        ReasoningNode* nb = node_hash_find(tb->node_hash, concepts[cj]);
                        if (!nb || !nb->features || nb->feature_dim != na->feature_dim) continue;

                        // fast lookup
                        if (cross_link_exists(master,
                                ta->topo_id, na->node_id,
                                tb->topo_id, nb->node_id)) continue;

                        float sim = cosine_similarity(na->features, nb->features, na->feature_dim);
                        if (isnan(sim) || isinf(sim)) sim = 0.0f;
                        if (sim < 0.45f) continue;

                        int ret = master_add_cross_link(master,
                            ta->topo_id, na->node_id,
                            tb->topo_id, nb->node_id,
                            0.3f + 0.3f * sim, "璇箟鐗瑰緛");
                        if (ret >= 0) { feat_created++; created++; }

                        ret = master_add_cross_link(master,
                            tb->topo_id, nb->node_id,
                            ta->topo_id, na->node_id,
                            0.3f + 0.3f * sim, "璇箟鐗瑰緛");
                        if (ret >= 0) { created++; }
                    }
                }
            }
        }
    }

    return created;
}
