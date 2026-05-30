/**
 * merge_state.c — 合并两个经过不同数据集训练的拓扑状态文件
 *
 * 用法: ./build/bin/merge_state state_pi.dat state_zero.dat state_merged.dat
 *
 * 合并策略:
 *   - 节点: 取并集, 置信度/重要性/激活次数取两者最大
 *   - 边: 取并集, 置信度取均值
 *   - 特征向量: 取均值（从同一起点训练，理应相近）
 *   - 元数据: 取两个统计的和
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAGIC 0x50564954   // "PVIT"
#define NAME_MAX 63
#define MAX_NODES 200000
#define MAX_EDGES 5000000
#define MAX_STRINGS 500000
#define FEATURE_DIM 7
#define HASH_SIZE 65536

// v4 文件头
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t topo_count;
    uint32_t node_count;
    uint32_t edge_count;
    uint32_t cross_count;
    uint32_t total_strings;
    uint64_t data_size;
    uint32_t node_capacity;
    uint32_t active_count;
    uint64_t total_learn_count;
    uint64_t total_dialog_count;
    float avg_confidence;
    float complexity_index;
} FileHeader;

// SubTopology header (v4)
typedef struct {
    uint32_t topo_type;
    char name[64];
    uint32_t node_count;
    uint32_t edge_count;
    uint32_t capacity;
    uint32_t feature_dim;
    uint32_t cross_count;
    uint32_t padding;
} SubHeader;

// Node 结构 (v4)
typedef struct {
    uint32_t id;
    char name[64];
    float confidence;
    float importance;
    float activation;
    uint32_t activation_count;
    float features[FEATURE_DIM];  // 和代码中 FEATURE_DIM 保持一致
    float semantic_vector[FEATURE_DIM];
} NodeV4;

// Edge 结构 (v4)
typedef struct {
    uint32_t from;
    uint32_t to;
    float confidence;
    float weight;
    uint32_t count;
    uint32_t pad;
} EdgeV4;

// CrossEdge 结构 (v4)
typedef struct {
    uint32_t from_topo;
    uint32_t from_node;
    uint32_t to_topo;
    uint32_t to_node;
    float confidence;
    float weight;
    uint32_t count;
    uint32_t pad;
} CrossEdgeV4;

// String entry
typedef struct {
    uint32_t id;
    char str[256];
} StringEntry;

static int node_cmp_by_id(const void* a, const void* b) {
    uint32_t id_a = ((NodeV4*)a)->id;
    uint32_t id_b = ((NodeV4*)b)->id;
    return (id_a > id_b) - (id_a < id_b);
}

static int edge_cmp(const void* a, const void* b) {
    const EdgeV4* ea = (const EdgeV4*)a;
    const EdgeV4* eb = (const EdgeV4*)b;
    if (ea->from != eb->from) return (ea->from > eb->from) - (ea->from < eb->from);
    return (ea->to > eb->to) - (ea->to < eb->to);
}

static int cross_cmp(const void* a, const void* b) {
    const CrossEdgeV4* ea = (const CrossEdgeV4*)a;
    const CrossEdgeV4* eb = (const CrossEdgeV4*)b;
    if (ea->from_topo != eb->from_topo) return (ea->from_topo > eb->from_topo) - (ea->from_topo < eb->from_topo);
    if (ea->from_node != eb->from_node) return (ea->from_node > eb->from_node) - (ea->from_node < eb->from_node);
    if (ea->to_topo != eb->to_topo) return (ea->to_topo > eb->to_topo) - (ea->to_topo < eb->to_topo);
    return (ea->to_node > eb->to_node) - (ea->to_node < eb->to_node);
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        fprintf(stderr, "用法: %s <state_a> <state_b> <state_out>\n", argv[0]);
        return 1;
    }

    const char* path_a = argv[1];
    const char* path_b = argv[2];
    const char* path_out = argv[3];

    // 读取两个状态文件
    FILE *fa, *fb;
    fa = fopen(path_a, "rb");
    fb = fopen(path_b, "rb");
    if (!fa || !fb) {
        fprintf(stderr, "无法打开输入文件\n");
        if (fa) fclose(fa);
        return 1;
    }

    // ===== 读取文件 A =====
    FileHeader ha;
    fread(&ha, sizeof(FileHeader), 1, fa);
    if (ha.magic != MAGIC) {
        fprintf(stderr, "文件A魔数错误: 0x%X\n", ha.magic);
        fclose(fa); fclose(fb);
        return 1;
    }
    printf("文件A: v%d, %u节点, %u边, %u跨拓扑, 学习%lu次\n",
           ha.version, ha.node_count, ha.edge_count, ha.cross_count, ha.total_learn_count);

    // 定位到子拓扑数据
    fseek(fa, sizeof(FileHeader), SEEK_SET);
    SubHeader* subs_a = malloc(sizeof(SubHeader) * ha.topo_count);
    fread(subs_a, sizeof(SubHeader), ha.topo_count, fa);

    // 读取所有节点
    int total_nodes_a = 0;
    for (int i = 0; i < ha.topo_count; i++) total_nodes_a += subs_a[i].node_count;
    NodeV4* nodes_a = calloc(total_nodes_a, sizeof(NodeV4));
    int offset = 0;
    for (int i = 0; i < ha.topo_count; i++) {
        fread(nodes_a + offset, sizeof(NodeV4), subs_a[i].node_count, fa);
        offset += subs_a[i].node_count;
    }

    // 读取所有边
    int total_edges_a = 0;
    for (int i = 0; i < ha.topo_count; i++) total_edges_a += subs_a[i].edge_count;
    EdgeV4* edges_a = calloc(total_edges_a, sizeof(EdgeV4));
    offset = 0;
    for (int i = 0; i < ha.topo_count; i++) {
        fread(edges_a + offset, sizeof(EdgeV4), subs_a[i].edge_count, fa);
        offset += subs_a[i].edge_count;
    }

    // 读取跨拓扑边
    CrossEdgeV4* cross_a = calloc(ha.cross_count, sizeof(CrossEdgeV4));
    fread(cross_a, sizeof(CrossEdgeV4), ha.cross_count, fa);

    // 读取字符串池
    uint32_t str_count_a;
    fread(&str_count_a, sizeof(uint32_t), 1, fa);
    StringEntry* strings_a = calloc(str_count_a, sizeof(StringEntry));
    fread(strings_a, sizeof(StringEntry), str_count_a, fa);

    fclose(fa);

    // ===== 读取文件 B =====
    FileHeader hb;
    fread(&hb, sizeof(FileHeader), 1, fb);
    if (hb.magic != MAGIC) {
        fprintf(stderr, "文件B魔数错误: 0x%X\n", hb.magic);
        fclose(fb);
        return 1;
    }
    printf("文件B: v%d, %u节点, %u边, %u跨拓扑, 学习%lu次\n",
           hb.version, hb.node_count, hb.edge_count, hb.cross_count, hb.total_learn_count);

    fseek(fb, sizeof(FileHeader), SEEK_SET);
    SubHeader* subs_b = malloc(sizeof(SubHeader) * hb.topo_count);
    fread(subs_b, sizeof(SubHeader), hb.topo_count, fb);

    int total_nodes_b = 0;
    for (int i = 0; i < hb.topo_count; i++) total_nodes_b += subs_b[i].node_count;
    NodeV4* nodes_b = calloc(total_nodes_b, sizeof(NodeV4));
    offset = 0;
    for (int i = 0; i < hb.topo_count; i++) {
        fread(nodes_b + offset, sizeof(NodeV4), subs_b[i].node_count, fb);
        offset += subs_b[i].node_count;
    }

    int total_edges_b = 0;
    for (int i = 0; i < hb.topo_count; i++) total_edges_b += subs_b[i].edge_count;
    EdgeV4* edges_b = calloc(total_edges_b, sizeof(EdgeV4));
    offset = 0;
    for (int i = 0; i < hb.topo_count; i++) {
        fread(edges_b + offset, sizeof(EdgeV4), subs_b[i].edge_count, fb);
        offset += subs_b[i].edge_count;
    }

    CrossEdgeV4* cross_b = calloc(hb.cross_count, sizeof(CrossEdgeV4));
    fread(cross_b, sizeof(CrossEdgeV4), hb.cross_count, fb);

    uint32_t str_count_b;
    fread(&str_count_b, sizeof(uint32_t), 1, fb);
    StringEntry* strings_b = calloc(str_count_b, sizeof(StringEntry));
    fread(strings_b, sizeof(StringEntry), str_count_b, fb);

    fclose(fb);

    // ===== 合并 =====
    // 按ID排序做合并
    qsort(nodes_a, total_nodes_a, sizeof(NodeV4), node_cmp_by_id);
    qsort(nodes_b, total_nodes_b, sizeof(NodeV4), node_cmp_by_id);
    qsort(edges_a, total_edges_a, sizeof(EdgeV4), edge_cmp);
    qsort(edges_b, total_edges_b, sizeof(EdgeV4), edge_cmp);
    qsort(cross_a, ha.cross_count, sizeof(CrossEdgeV4), cross_cmp);
    qsort(cross_b, hb.cross_count, sizeof(CrossEdgeV4), cross_cmp);

    // 合并节点 (ID并集)
    NodeV4* nodes_merged = calloc(total_nodes_a + total_nodes_b, sizeof(NodeV4));
    int n_merged = 0, i = 0, j = 0;
    while (i < total_nodes_a || j < total_nodes_b) {
        if (j >= total_nodes_b || (i < total_nodes_a && nodes_a[i].id < nodes_b[j].id)) {
            nodes_merged[n_merged++] = nodes_a[i++];
        } else if (i >= total_nodes_a || (j < total_nodes_b && nodes_b[j].id < nodes_a[i].id)) {
            nodes_merged[n_merged++] = nodes_b[j++];
        } else {
            // 相同节点—取最大值
            nodes_merged[n_merged] = nodes_a[i];
            if (nodes_b[j].confidence > nodes_merged[n_merged].confidence)
                nodes_merged[n_merged].confidence = nodes_b[j].confidence;
            if (nodes_b[j].importance > nodes_merged[n_merged].importance)
                nodes_merged[n_merged].importance = nodes_b[j].importance;
            if (nodes_b[j].activation_count > nodes_merged[n_merged].activation_count)
                nodes_merged[n_merged].activation_count = nodes_b[j].activation_count;
            // 特征向量取均值
            for (int k = 0; k < FEATURE_DIM; k++) {
                float avg_f = (nodes_a[i].features[k] + nodes_b[j].features[k]) / 2.0f;
                float avg_s = (nodes_a[i].semantic_vector[k] + nodes_b[j].semantic_vector[k]) / 2.0f;
                nodes_merged[n_merged].features[k] = avg_f;
                nodes_merged[n_merged].semantic_vector[k] = avg_s;
            }
            n_merged++;
            i++; j++;
        }
    }
    printf("合并节点: %d (A:%d + B:%d)\n", n_merged, total_nodes_a, total_nodes_b);

    // 合并边 (并集，同一条边置信度取均值)
    EdgeV4* edges_merged = calloc(total_edges_a + total_edges_b, sizeof(EdgeV4));
    int e_merged = 0;
    i = 0; j = 0;
    while (i < total_edges_a || j < total_edges_b) {
        if (j >= total_edges_b) {
            edges_merged[e_merged++] = edges_a[i++];
        } else if (i >= total_edges_a) {
            edges_merged[e_merged++] = edges_b[j++];
        } else {
            int cmp = edge_cmp(&edges_a[i], &edges_b[j]);
            if (cmp < 0) {
                edges_merged[e_merged++] = edges_a[i++];
            } else if (cmp > 0) {
                edges_merged[e_merged++] = edges_b[j++];
            } else {
                edges_merged[e_merged] = edges_a[i];
                edges_merged[e_merged].confidence = (edges_a[i].confidence + edges_b[j].confidence) / 2.0f;
                edges_merged[e_merged].weight = (edges_a[i].weight + edges_b[j].weight) / 2.0f;
                edges_merged[e_merged].count = edges_a[i].count + edges_b[j].count;
                e_merged++;
                i++; j++;
            }
        }
    }
    printf("合并边: %d (A:%d + B:%d)\n", e_merged, total_edges_a, total_edges_b);

    // 合并跨拓扑边
    CrossEdgeV4* cross_merged = calloc(ha.cross_count + hb.cross_count, sizeof(CrossEdgeV4));
    int c_merged = 0;
    i = 0; j = 0;
    while (i < ha.cross_count || j < hb.cross_count) {
        if (j >= hb.cross_count) {
            cross_merged[c_merged++] = cross_a[i++];
        } else if (i >= ha.cross_count) {
            cross_merged[c_merged++] = cross_b[j++];
        } else {
            int cmp = cross_cmp(&cross_a[i], &cross_b[j]);
            if (cmp < 0) {
                cross_merged[c_merged++] = cross_a[i++];
            } else if (cmp > 0) {
                cross_merged[c_merged++] = cross_b[j++];
            } else {
                cross_merged[c_merged] = cross_a[i];
                cross_merged[c_merged].confidence = (cross_a[i].confidence + cross_b[j].confidence) / 2.0f;
                cross_merged[c_merged].weight = (cross_a[i].weight + cross_b[j].weight) / 2.0f;
                cross_merged[c_merged].count = cross_a[i].count + cross_b[j].count;
                c_merged++;
                i++; j++;
            }
        }
    }
    printf("合并跨拓扑边: %d (A:%d + B:%d)\n", c_merged, ha.cross_count, hb.cross_count);

    // 合并字符串池 (去重)
    StringEntry* strings_merged = calloc(str_count_a + str_count_b, sizeof(StringEntry));
    int s_merged = 0;
    for (int i = 0; i < str_count_a; i++) {
        int found = 0;
        for (int j = 0; j < s_merged; j++) {
            if (strcmp(strings_merged[j].str, strings_a[i].str) == 0) {
                found = 1; break;
            }
        }
        if (!found) strings_merged[s_merged++] = strings_a[i];
    }
    for (int i = 0; i < str_count_b; i++) {
        int found = 0;
        for (int j = 0; j < s_merged; j++) {
            if (strcmp(strings_merged[j].str, strings_b[i].str) == 0) {
                found = 1; break;
            }
        }
        if (!found) strings_merged[s_merged++] = strings_b[i];
    }
    printf("合并字符串: %d (A:%d + B:%d)\n", s_merged, str_count_a, str_count_b);

    // ===== 写输出 =====
    // 简化：按子拓扑分类节点
    // 复用文件A的子拓扑结构，重新分配节点
    uint32_t merged_node_count = n_merged;
    uint32_t merged_edge_count = e_merged;
    uint32_t merged_cross_count = c_merged;

    // 直接使用A的子拓扑结构 (合并后无法按原拓扑分离，简化处理)
    // 把所有节点归到第一个子拓扑
    FileHeader h_out = ha;
    h_out.node_count = merged_node_count;
    h_out.edge_count = merged_edge_count;
    h_out.cross_count = merged_cross_count;
    h_out.total_strings = s_merged;
    h_out.total_learn_count = ha.total_learn_count + hb.total_learn_count;
    h_out.total_dialog_count = ha.total_dialog_count + hb.total_dialog_count;
    h_out.avg_confidence = 0;
    h_out.complexity_index = (ha.complexity_index + hb.complexity_index) / 2.0f;
    h_out.data_size = 0;  // 不计算，写时填充

    // 更新子拓扑—把A的节点合并到子拓扑0
    for (int i = 0; i < ha.topo_count; i++) {
        if (i == 0) subs_a[i].node_count = merged_node_count;
        else subs_a[i].node_count = 0;
        subs_a[i].edge_count = 0;    // 边不按拓扑区分（简化）
        subs_a[i].cross_count = 0;
    }

    FILE* fout = fopen(path_out, "wb");
    if (!fout) { perror("fopen out"); return 1; }

    fwrite(&h_out, sizeof(FileHeader), 1, fout);
    fwrite(subs_a, sizeof(SubHeader), ha.topo_count, fout);

    // 写所有节点
    for (int i = 0; i < ha.topo_count; i++) {
        if (i == 0)
            fwrite(nodes_merged, sizeof(NodeV4), merged_node_count, fout);
        // 其他子拓扑不写节点
    }

    // 写边 — 全写入子拓扑0
    // 把merged_edges_tally按子拓扑分类...
    // 简化：全部写入第一个子拓扑的边段
    // 先写个占位，再回头写
    uint32_t topo_edge_counts = merged_edge_count;
    fwrite(&topo_edge_counts, sizeof(uint32_t), 1, fout);
    fwrite(edges_merged, sizeof(EdgeV4), merged_edge_count, fout);

    // 其他子拓扑边数为0
    for (int i = 1; i < ha.topo_count; i++) {
        topo_edge_counts = 0;
        fwrite(&topo_edge_counts, sizeof(uint32_t), 1, fout);
    }

    // 跨拓扑边
    fwrite(cross_merged, sizeof(CrossEdgeV4), merged_cross_count, fout);

    // 字符串池
    fwrite(&s_merged, sizeof(uint32_t), 1, fout);
    fwrite(strings_merged, sizeof(StringEntry), s_merged, fout);

    fclose(fout);

    printf("\n✅ 合并完成: %s\n", path_out);
    printf("   节点: %u | 边: %u | 跨拓扑: %u | 字符串: %u\n",
           merged_node_count, merged_edge_count, merged_cross_count, s_merged);

    // 清理
    free(subs_a); free(subs_b);
    free(nodes_a); free(nodes_b); free(nodes_merged);
    free(edges_a); free(edges_b); free(edges_merged);
    free(cross_a); free(cross_b); free(cross_merged);
    free(strings_a); free(strings_b); free(strings_merged);

    return 0;
}
