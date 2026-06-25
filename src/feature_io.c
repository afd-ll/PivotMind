#include "feature_io.h"
#include "huarong_topology.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/**
 * 遍历所有子拓扑的所有节点, 收集节点数
 */
static int count_all_nodes(MasterTopology* master) {
    int total = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (sub && sub->net) {
            total += sub->net->node_count;
        }
    }
    return total;
}

int save_features(MasterTopology* master, const char* filepath) {
    if (!master || !filepath) return -1;

    int total_nodes = count_all_nodes(master);
    if (total_nodes <= 0) return -1;

    // 先写临时文件
    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", filepath);

    FILE* fp = fopen(tmp_path, "wb");
    if (!fp) return -1;

    uint32_t magic = FEATURE_FILE_MAGIC;
    uint32_t n = (uint32_t)total_nodes;
    uint32_t d = NODE_FEATURE_DIM;

    fwrite(&magic, sizeof(uint32_t), 1, fp);
    fwrite(&n, sizeof(uint32_t), 1, fp);
    fwrite(&d, sizeof(uint32_t), 1, fp);

    int written = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;

        for (int i = 0; i < sub->net->node_count; i++) {
            ReasoningNode* node = sub->net->nodes[i];
            if (!node) continue;

            // 如果节点没有特征向量, 写零
            if (!node->features || node->feature_dim <= 0) {
                float zeros[NODE_FEATURE_DIM];
                memset(zeros, 0, sizeof(zeros));
                fwrite(zeros, sizeof(float), NODE_FEATURE_DIM, fp);
            } else {
                fwrite(node->features, sizeof(float),
                       node->feature_dim < NODE_FEATURE_DIM ? node->feature_dim : NODE_FEATURE_DIM, fp);
                // 如果 feature_dim < NODE_FEATURE_DIM, 补零
                if (node->feature_dim < NODE_FEATURE_DIM) {
                    float pad = 0.0f;
                    for (int p = node->feature_dim; p < NODE_FEATURE_DIM; p++) {
                        fwrite(&pad, sizeof(float), 1, fp);
                    }
                }
            }
            written++;
        }
    }

    fclose(fp);

    // 原子写入: rename 覆盖
    if (rename(tmp_path, filepath) != 0) {
        remove(tmp_path);
        return -1;
    }

    return written;
}

int load_features(MasterTopology* master, const char* filepath) {
    if (!master || !filepath) return -1;

    FILE* fp = fopen(filepath, "rb");
    if (!fp) return -1;

    uint32_t magic, n, d;
    if (fread(&magic, sizeof(uint32_t), 1, fp) != 1) { fclose(fp); return -1; }
    if (magic != FEATURE_FILE_MAGIC) { fclose(fp); return -1; }
    if (fread(&n, sizeof(uint32_t), 1, fp) != 1) { fclose(fp); return -1; }
    if (fread(&d, sizeof(uint32_t), 1, fp) != 1) { fclose(fp); return -1; }
    if (d != NODE_FEATURE_DIM) { fclose(fp); return -1; }

    // 校验节点数 — 不再严格拒绝，改用自适应加载
    int total_nodes = count_all_nodes(master);
    int to_load = (int)n;           // 文件中可读取的节点数
    int to_skip_in_memory __attribute__((unused)) = 0;  // 内存中超出文件范围的节点数

    if ((int)n != total_nodes) {
        printf("  [特征持久化] 节点数变化 (文件=%d, 内存=%d), 执行自适应加载\n",
               n, total_nodes);
        if ((int)n < total_nodes) {
            to_skip_in_memory = total_nodes - (int)n;
            printf("  [特征持久化] 将加载 %d 个已存特征, 新节点自动随机初始化\n", (int)n);
        } else {
            to_load = total_nodes;  // 只读 total_nodes 个, 跳过文件多余部分
            printf("  [特征持久化] 将加载 %d 个特征, 忽略文件末尾 %d 个无效数据\n",
                   total_nodes, (int)n - total_nodes);
        }
    }

    int loaded = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;

        for (int i = 0; i < sub->net->node_count; i++) {
            ReasoningNode* node = sub->net->nodes[i];
            if (!node) {
                // 跳过文件中的对应位置
                if (loaded < to_load) {
                    fseek(fp, NODE_FEATURE_DIM * sizeof(float), SEEK_CUR);
                    loaded++;  // 计数跳过, 维持文件位置同步
                }
                continue;
            }

            // 确保节点有 feature 数组
            if (!node->features) {
                node->features = (float*)malloc(NODE_FEATURE_DIM * sizeof(float));
                node->feature_dim = NODE_FEATURE_DIM;
            } else if (node->feature_dim < NODE_FEATURE_DIM) {
                float* new_feat = (float*)realloc(node->features, NODE_FEATURE_DIM * sizeof(float));
                if (!new_feat) continue;  /* realloc 失败，旧指针仍有效，跳过 */
                node->features = new_feat;
                memset(node->features + node->feature_dim, 0,
                       (NODE_FEATURE_DIM - node->feature_dim) * sizeof(float));
                node->feature_dim = NODE_FEATURE_DIM;
            }

            if (loaded < to_load) {
                // 文件中有数据: 从文件读取
                if (fread(node->features, sizeof(float), NODE_FEATURE_DIM, fp) != NODE_FEATURE_DIM) {
                    fclose(fp);
                    return loaded;
                }
                loaded++;
            } else {
                // 文件中无数据 (新节点): 保持零, 让 init_random_features 填充
                // 清零确保 is_zero 检查能正确判断
                memset(node->features, 0, NODE_FEATURE_DIM * sizeof(float));
            }
        }
    }

    fclose(fp);

    // 如果是文件节点数 > 内存节点数, 文件末尾有未读数据, 忽略即可
    return loaded;
}

int init_random_features(MasterTopology* master) {
    if (!master) return 0;

    init_random();
    int initialized = 0;

    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;

        for (int i = 0; i < sub->net->node_count; i++) {
            ReasoningNode* node = sub->net->nodes[i];
            if (!node) continue;

            // 跳过已有非零特征的节点
            if (node->features && node->feature_dim >= NODE_FEATURE_DIM) {
                int has_data = 0;
                for (int j = 0; j < NODE_FEATURE_DIM; j++) {
                    if (node->features[j] < -1e-10f || node->features[j] > 1e-10f) {
                        has_data = 1;
                        break;
                    }
                }
                if (has_data) continue;
            }

            // 分配/重分配
            if (!node->features) {
                node->features = (float*)malloc(NODE_FEATURE_DIM * sizeof(float));
            } else if (node->feature_dim < NODE_FEATURE_DIM) {
                float* new_feat = (float*)realloc(node->features, NODE_FEATURE_DIM * sizeof(float));
                if (!new_feat) continue;  /* realloc 失败，旧指针仍有效但太小，跳过 */
                node->features = new_feat;
            }
            node->feature_dim = NODE_FEATURE_DIM;

            // Xavier/Kaiming 均匀初始化: 范围 [-limit, limit]
            // 使用 init_random() 设置的种子, 保证可复现
            float limit = sqrtf(6.0f / (float)NODE_FEATURE_DIM);
            for (int j = 0; j < NODE_FEATURE_DIM; j++) {
                float r = (float)rand() / (float)RAND_MAX;
                node->features[j] = (2.0f * r - 1.0f) * limit;
            }
            initialized++;
        }
    }

    return initialized;
}
