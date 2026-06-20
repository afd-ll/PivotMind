/**
 * @file topology_brain.c
 * @brief 9+1 脑区索引实现 — 词性从连接模式中涌现
 */

#include "topology_brain.h"
#include "huarong_topology.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ==================== 内部结构 ====================

/** 单节点脑区隶属度 */
typedef struct {
    int    node_id;
    float  ema[TOPOBRAIN_NUM_REGIONS];  // 归一化隶属度数组
    int    last_update_tick;             // 最后更新时的全局 tick
} TopoBrainEntry;

struct TopologyBrain {
    TopoBrainConfig cfg;

    TopoBrainEntry* entries;   // 按 node_id 排序的数组 → O(log n) 二分查找
    int count;
    int capacity;

    int total_updates;         // 累计 EMA 更新次数
    int total_migrations;      // 累计脑区迁移次数
};

// 脑区名称
static const char* TOPOBRAIN_NAMES[TOPOBRAIN_NUM_REGIONS] = {
    "词汇区", "名词区", "动词区", "形容词区", "代词区",
    "副词区", "介词区", "连词区", "助词区", "数词区"
};

// ==================== 二分查找工具 ====================

/** 查找条目（二分）返回索引，-1=未找到 */
static int _tb_find(TopologyBrain* tb, int node_id) {
    if (!tb || tb->count == 0) return -1;
    int lo = 0, hi = tb->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (tb->entries[mid].node_id == node_id) return mid;
        if (tb->entries[mid].node_id < node_id) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

/** 插入位置（保持有序），返回插入点索引 */
static int _tb_insert_pos(TopologyBrain* tb, int node_id) {
    int lo = 0, hi = tb->count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (tb->entries[mid].node_id < node_id) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// ==================== EMA 更新（核心算法） ====================

/**
 * 节点向某个脑区偏移：
 *   1. 全局衰减：所有 ema *= (1 - α)
 *   2. 目标区域强化：ema[region] += α
 *   3. 归一化
 */
static void _tb_shift(TopoBrainEntry* e, TopoBrainRegion region, float alpha) {
    float decay = 1.0f - alpha;
    float sum = 0.0f;

    // 全局衰减
    for (int i = 0; i < TOPOBRAIN_NUM_REGIONS; i++) {
        e->ema[i] *= decay;
    }

    // 目标强化
    e->ema[(int)region] += alpha;

    // 归一化
    for (int i = 0; i < TOPOBRAIN_NUM_REGIONS; i++) {
        sum += e->ema[i];
    }
    if (sum > 0.001f) {
        float inv_sum = 1.0f / sum;
        for (int i = 0; i < TOPOBRAIN_NUM_REGIONS; i++) {
            e->ema[i] *= inv_sum;
        }
    }
}

// ==================== 公共 API ====================

TopologyBrain* topobrain_create(int initial_nodes) {
    TopologyBrain* tb = (TopologyBrain*)calloc(1, sizeof(TopologyBrain));
    if (!tb) return NULL;

    // 默认配置
    TopoBrainConfig d = TOPOBRAIN_DEFAULT_CONFIG;
    tb->cfg = d;

    int cap = initial_nodes > 0 ? initial_nodes : 1024;
    tb->entries = (TopoBrainEntry*)malloc(cap * sizeof(TopoBrainEntry));
    if (!tb->entries) { free(tb); return NULL; }
    tb->capacity = cap;
    tb->count = 0;
    tb->total_updates = 0;
    tb->total_migrations = 0;

    return tb;
}

void topobrain_destroy(TopologyBrain* tb) {
    if (!tb) return;
    free(tb->entries);
    free(tb);
}

void topobrain_set_config(TopologyBrain* tb, TopoBrainConfig* cfg) {
    if (!tb || !cfg) return;
    tb->cfg = *cfg;
}

int topobrain_add_node(TopologyBrain* tb, int node_id) {
    if (!tb) return -2;
    // 查重
    if (_tb_find(tb, node_id) >= 0) return -1;

    // 扩容
    if (tb->count >= tb->capacity) {
        int new_cap = tb->capacity ? tb->capacity * 2 : 1024;
        TopoBrainEntry* tmp = (TopoBrainEntry*)realloc(
            tb->entries, new_cap * sizeof(TopoBrainEntry));
        if (!tmp) return -2;
        tb->entries = tmp;
        tb->capacity = new_cap;
    }

    // 插入到有序位置
    int pos = _tb_insert_pos(tb, node_id);
    // 后移元素
    if (pos < tb->count) {
        memmove(&tb->entries[pos + 1], &tb->entries[pos],
                (tb->count - pos) * sizeof(TopoBrainEntry));
    }

    TopoBrainEntry* e = &tb->entries[pos];
    e->node_id = node_id;
    memset(e->ema, 0, sizeof(e->ema));
    e->ema[TOPOBRAIN_VOCAB] = 1.0f;  // 新词默认词汇区
    e->last_update_tick = 0;
    tb->count++;

    return 0;
}

void topobrain_update_by_node(TopologyBrain* tb, int node_id, int target_node_id) {
    if (!tb) return;

    // 查目标脑区
    TopoBrainRegion target_region = topobrain_query(tb, target_node_id);

    // 更新源节点
    topobrain_update_by_region(tb, node_id, target_region);
}

void topobrain_update_by_region(TopologyBrain* tb, int node_id, TopoBrainRegion region) {
    if (!tb) return;

    int idx = _tb_find(tb, node_id);
    if (idx < 0) {
        // 节点尚未注册 → 自动注册
        if (topobrain_add_node(tb, node_id) != 0) return;
        idx = _tb_find(tb, node_id);
        if (idx < 0) return;
    }

    TopoBrainEntry* e = &tb->entries[idx];

    // 迁移前脑区（用于判断是否迁移）
    TopoBrainRegion old_region = TOPOBRAIN_VOCAB;
    {
        float max_val = 0;
        for (int i = 0; i < TOPOBRAIN_NUM_REGIONS; i++) {
            if (e->ema[i] > max_val) {
                max_val = e->ema[i];
                old_region = (TopoBrainRegion)i;
            }
        }
    }

    // EMA 更新
    _tb_shift(e, region, tb->cfg.ema_alpha);
    tb->total_updates++;

    // 迁移检测
    {
        float max_val = 0;
        TopoBrainRegion new_region = TOPOBRAIN_VOCAB;
        for (int i = 0; i < TOPOBRAIN_NUM_REGIONS; i++) {
            if (e->ema[i] > max_val) {
                max_val = e->ema[i];
                new_region = (TopoBrainRegion)i;
            }
        }
        if (max_val > tb->cfg.converge_threshold && new_region != old_region) {
            tb->total_migrations++;
            if (tb->cfg.verbose) {
                printf("[脑区] 节点%d 迁移: %s → %s\n",
                       node_id, topobrain_region_name(old_region),
                       topobrain_region_name(new_region));
            }
        }
    }
}

TopoBrainRegion topobrain_query(TopologyBrain* tb, int node_id) {
    if (!tb) return TOPOBRAIN_VOCAB;

    int idx = _tb_find(tb, node_id);
    if (idx < 0) return TOPOBRAIN_VOCAB;

    float max_val = 0;
    TopoBrainRegion best = TOPOBRAIN_VOCAB;
    for (int i = 0; i < TOPOBRAIN_NUM_REGIONS; i++) {
        if (tb->entries[idx].ema[i] > max_val) {
            max_val = tb->entries[idx].ema[i];
            best = (TopoBrainRegion)i;
        }
    }

    // 未收敛 → 返回词汇区
    if (max_val < tb->cfg.converge_threshold) return TOPOBRAIN_VOCAB;

    return best;
}

int topobrain_scan(TopologyBrain* tb, MasterTopology* master) {
    if (!tb || !master) return 0;

    // 获取词汇拓扑
    SubTopology* vocab = NULL;
    for (int t = 0; t < master->sub_topo_count; t++) {
        if (master->sub_topologies[t] &&
            master->sub_topologies[t]->type == TOPO_VOCABULARY) {
            vocab = master->sub_topologies[t];
            break;
        }
    }
    if (!vocab || !vocab->net) return 0;

    HuarongTopologyNet* net = vocab->net;
    int migrations = 0;

    // 遍历所有词汇节点
    for (int i = 0; i < net->node_count; i++) {
        ReasoningNode* node = net->nodes[i];
        if (!node || !node->concept) continue;

        // 确保已注册
        if (_tb_find(tb, i) < 0) {
            topobrain_add_node(tb, i);
        }

        // 只处理有连接的节点
        if (node->edge_count == 0) continue;

        // 对每条连接，更新隶属度
        for (int c = 0; c < node->edge_count; c++) {
            if (node->edges[c].target) {
                int target_id = node->edges[c].target->node_id;
                if (target_id >= 0) {
                    TopoBrainRegion tr = topobrain_query(tb, target_id);
                    topobrain_update_by_region(tb, i, tr);
                }
            }
        }

        int idx = _tb_find(tb, i);
        if (idx >= 0) {
            float max_val = 0;
            for (int r = 0; r < TOPOBRAIN_NUM_REGIONS; r++) {
                if (tb->entries[idx].ema[r] > max_val)
                    max_val = tb->entries[idx].ema[r];
            }
            // 记录迁移
            if (max_val > tb->cfg.converge_threshold) {
                migrations++;
            }
        }
    }

    return migrations;
}

const char* topobrain_region_name(TopoBrainRegion r) {
    if (r >= 0 && r < TOPOBRAIN_NUM_REGIONS) return TOPOBRAIN_NAMES[(int)r];
    return "未知";
}

void topobrain_get_stats(TopologyBrain* tb,
                          int* out_entries,
                          int* out_updates,
                          int* out_migrations) {
    if (!tb) return;
    if (out_entries)    *out_entries    = tb->count;
    if (out_updates)    *out_updates    = tb->total_updates;
    if (out_migrations) *out_migrations = tb->total_migrations;
}
