#include "../include/cross_edge_io.h"
#include "../include/huarong_topology.h"
#include "../include/common.h"
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
    uint32_t m = (uint32_t)count;

    fwrite(&magic, sizeof(uint32_t), 1, fp);
    fwrite(&m, sizeof(uint32_t), 1, fp);

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

        fwrite(&from_topo, sizeof(uint32_t), 1, fp);
        fwrite(&from_node, sizeof(uint32_t), 1, fp);
        fwrite(&to_topo,   sizeof(uint32_t), 1, fp);
        fwrite(&to_node,   sizeof(uint32_t), 1, fp);
        fwrite(&weight,    sizeof(float),    1, fp);
        fwrite(&use_cnt,   sizeof(uint32_t), 1, fp);
        written++;
    }

    fclose(fp);

    if (rename(tmp_path, filepath) != 0) {
        remove(tmp_path);
        return -1;
    }

    return written;
}

int load_cross_edges(MasterTopology* master, const char* filepath) {
    if (!master || !filepath) return -1;

    FILE* fp = fopen(filepath, "rb");
    if (!fp) return -1;

    uint32_t magic, m;
    if (fread(&magic, sizeof(uint32_t), 1, fp) != 1) { fclose(fp); return -1; }
    if (magic != CROSS_EDGE_FILE_MAGIC) { fclose(fp); return -1; }
    if (fread(&m, sizeof(uint32_t), 1, fp) != 1) { fclose(fp); return -1; }
    if (m > 10000000) { fclose(fp); return -1; }  // 防损坏

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

        // 验证拓扑类型范围
        if (from_topo >= (uint32_t)master->sub_topo_count ||
            to_topo   >= (uint32_t)master->sub_topo_count) {
            continue;  // 拓扑不匹配, 跳过
        }

        // 验证节点存在性: 检查目标拓扑的节点数组
        SubTopology* from_sub = master->sub_topologies[from_topo];
        SubTopology* to_sub   = master->sub_topologies[to_topo];
        if (!from_sub || !from_sub->net || !to_sub || !to_sub->net) continue;

        // 验证节点ID是否在有效范围内
        if ((int)from_node >= from_sub->net->node_count ||
            (int)to_node   >= to_sub->net->node_count) {
            continue;  // 节点ID变化, 跳过此边
        }

        // 验证节点非空
        if (!from_sub->net->nodes[from_node] || !to_sub->net->nodes[to_node]) continue;

        // 重建跨拓扑连接
        int result = master_add_cross_link(master,
                                           (int)from_topo, (int)from_node,
                                           (int)to_topo,   (int)to_node,
                                           weight, "persisted");

        // 如果成功, 恢复 use_count
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

int rebuild_cross_connections(MasterTopology* master) {
    if (!master) return 0;

    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    if (!vocab || !vocab->net || !vocab->node_hash) return 0;

    int created = 0;

    // 词汇→语义 跨连接
    SubTopology* semantic = master_get_sub_topology_by_type(master, TOPO_SEMANTIC);
    if (semantic && semantic->net && semantic->node_hash) {
        for (int si = 0; si < semantic->net->node_count; si++) {
            ReasoningNode* sn = semantic->net->nodes[si];
            if (!sn || !sn->concept) continue;
            for (int vi = 0; vi < vocab->net->node_count; vi++) {
                ReasoningNode* vn = vocab->net->nodes[vi];
                if (!vn || !vn->concept) continue;
                if (strstr(sn->concept, vn->concept)) {
                    int ret = master_add_cross_link(master,
                        vocab->topo_id,  vn->node_id,
                        semantic->topo_id, sn->node_id,
                        0.6f, "概念映射");
                    if (ret == 0) created++;
                    if (created >= 5000) break;
                }
            }
            if (created >= 5000) break;
        }
    }

    // 词汇→情绪 跨连接
    SubTopology* emotion = master_get_sub_topology_by_type(master, TOPO_EMOTION);
    if (emotion && emotion->net && emotion->node_hash) {
        for (int ei = 0; ei < emotion->net->node_count; ei++) {
            ReasoningNode* en = emotion->net->nodes[ei];
            if (!en || !en->concept) continue;
            for (int vi = 0; vi < vocab->net->node_count; vi++) {
                ReasoningNode* vn = vocab->net->nodes[vi];
                if (!vn || !vn->concept) continue;
                if (strstr(en->concept, vn->concept)) {
                    int ret = master_add_cross_link(master,
                        vocab->topo_id, vn->node_id,
                        emotion->topo_id, en->node_id,
                        0.5f, "情绪关联");
                    if (ret == 0) created++;
                    if (created >= 5000) break;
                }
            }
            if (created >= 5000) break;
        }
    }

    // 词汇→概念 跨连接
    SubTopology* concept = master_get_sub_topology_by_type(master, TOPO_CONCEPT);
    if (concept && concept->net && concept->node_hash) {
        for (int ci = 0; ci < concept->net->node_count; ci++) {
            ReasoningNode* cn = concept->net->nodes[ci];
            if (!cn || !cn->concept) continue;
            for (int vi = 0; vi < vocab->net->node_count; vi++) {
                ReasoningNode* vn = vocab->net->nodes[vi];
                if (!vn || !vn->concept) continue;
                if (strstr(cn->concept, vn->concept)) {
                    int ret = master_add_cross_link(master,
                        vocab->topo_id, vn->node_id,
                        concept->topo_id, cn->node_id,
                        0.6f, "概念关联");
                    if (ret == 0) created++;
                    if (created >= 5000) break;
                }
            }
            if (created >= 5000) break;
        }
    }

    return created;
}
