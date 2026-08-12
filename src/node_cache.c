/**
 * @file node_cache.c
 * @brief 大脑式节点冷热缓存实现
 */

#include "node_cache.h"
#include "multi_topology.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* 文件格式常量 */
#define NC_MAGIC      0x42434950  /* "PCIB" = PivotMind Cold-Brain Index */
#define NC_VERSION    1
#define NC_BITMAP_SIZE(node_cap)  (((node_cap) + 7) / 8)

/* 每个节点的文件头 */
#define NC_NODE_HDR_SZ  12  /* concept_len(4) + feat_dim(4) + conn_count(4) */

/* ================================================================
 *  内部工具
 * ================================================================ */

/* 位图操作 */
static inline int bitmap_test(uint8_t* bm, int idx) {
    return (bm[idx >> 3] >> (idx & 7)) & 1;
}
static inline void bitmap_set(uint8_t* bm, int idx) {
    bm[idx >> 3] |= (uint8_t)(1 << (idx & 7));
}

/* 基于 node_id 在拓扑网络中查找节点指针 */
static ReasoningNode* nc_lookup_node(HuarongTopologyNet* net, int node_id) {
    if (!net || node_id < 0 || node_id >= net->node_count) return NULL;
    return net->nodes[node_id];
}

/* ================================================================
 *  创建 / 销毁
 * ================================================================ */

NodeCache* node_cache_create(const char* filepath, int node_cap) {
    if (!filepath || node_cap < 1) return NULL;

    NodeCache* nc = (NodeCache*)calloc(1, sizeof(NodeCache));
    if (!nc) return NULL;

    strncpy(nc->filepath, filepath, sizeof(nc->filepath) - 1);
    nc->node_count = node_cap;

    /* 分配内存索引表 */
    int bm_sz = NC_BITMAP_SIZE(node_cap);
    nc->bitmap  = (uint8_t*)calloc(1, bm_sz);
    nc->offsets = (int64_t*)calloc(node_cap, sizeof(int64_t));
    nc->sizes   = (int32_t*)calloc(node_cap, sizeof(int32_t));
    if (!nc->bitmap || !nc->offsets || !nc->sizes) {
        node_cache_destroy(nc);
        return NULL;
    }

    pthread_mutex_init(&nc->lock, NULL);

    /* 打开/创建文件 */
    int is_new = 0;
    FILE* f = fopen(filepath, "rb");
    if (!f) {
        is_new = 1;
    } else {
        /* 读取已有文件头 */
        uint32_t hdr[4];
        if (fread(hdr, 4, 4, f) != 4 || hdr[0] != NC_MAGIC || hdr[1] != NC_VERSION) {
            fclose(f);
            is_new = 1;
        } else {
            int file_nodes = (int)hdr[2];
            if (file_nodes != node_cap) {
                fprintf(stderr, "[node_cache] 警告: 文件节点数(%d) != 预期(%d), 重建索引\n",
                        file_nodes, node_cap);
                fclose(f);
                is_new = 1;
            } else {
                /* 读取 bitmap */
                size_t _nr = fread(nc->bitmap, 1, bm_sz, f);
                /* 读取索引表 */
                _nr += fread(nc->offsets, sizeof(int64_t), node_cap, f);
                _nr += fread(nc->sizes,   sizeof(int32_t), node_cap, f);
                (void)_nr;
                fclose(f);
            }
        }
    }

    if (is_new) {
        /* 创建新文件 */
        f = fopen(filepath, "wb+");
        if (!f) { node_cache_destroy(nc); return NULL; }
        uint32_t hdr[4] = { NC_MAGIC, NC_VERSION, (uint32_t)node_cap, 0 };
        size_t _nw = fwrite(hdr, 4, 4, f);
        _nw += fwrite(nc->bitmap, 1, bm_sz, f);
        _nw += fwrite(nc->offsets, sizeof(int64_t), node_cap, f);
        _nw += fwrite(nc->sizes,   sizeof(int32_t), node_cap, f);
        (void)_nw;
        fflush(f);
    } else {
        /* 以读写模式打开 */
        f = fopen(filepath, "r+b");
        if (!f) { node_cache_destroy(nc); return NULL; }
    }

    nc->fp = f;
    /* 计算索引区结束位置（数据区起始） */
    int64_t data_start = 16 + bm_sz + node_cap * 16;
    fseeko(f, data_start, SEEK_SET);

    printf("[node_cache] 就绪: %s (%s, %d 节点)\n",
           filepath, is_new ? "新建" : "已存在", node_cap);
    return nc;
}

void node_cache_destroy(NodeCache* nc) {
    if (!nc) return;
    if (nc->fp) fclose(nc->fp);
    free(nc->bitmap);
    free(nc->offsets);
    free(nc->sizes);
    pthread_mutex_destroy(&nc->lock);
    free(nc);
}

/* ================================================================
 *  序列化 / 反序列化
 * ================================================================ */

/** 计算节点数据块大小 */
static int nc_node_data_size(ReasoningNode* node) {
    if (!node) return 0;
    int sz = NC_NODE_HDR_SZ;  /* header */
    sz += (int)strlen(node->concept ? node->concept : "") + 1;  /* concept string */
    sz += node->feature_dim * (int)sizeof(float);              /* features */
    /* connections: target_id(4) + weight(4) + bias(4) + conf(4) = 16B each */
    sz += node->edge_count * 16;
    return sz;
}

/** 序列化一个节点到内存缓冲区 */
static int nc_serialize_node(HuarongTopologyNet* net, ReasoningNode* node, uint8_t* buf, int buf_sz) {
    if (!node || !buf) return -1;

    const char* concept = node->concept ? node->concept : "";
    int concept_len = (int)strlen(concept);
    int needed = NC_NODE_HDR_SZ + concept_len + 1
               + node->feature_dim * (int)sizeof(float)
               + node->edge_count * 16;

    if (needed > buf_sz) return -1;

    uint8_t* p = buf;

    /* 写入 header */
    memcpy(p, &concept_len, 4);                   p += 4;
    memcpy(p, &node->feature_dim, 4);             p += 4;
    memcpy(p, &node->edge_count, 4);        p += 4;

    /* concept 字符串 */
    memcpy(p, concept, concept_len + 1);          p += concept_len + 1;

    /* features */
    if (node->features && node->feature_dim > 0) {
        memcpy(p, node->features, node->feature_dim * sizeof(float));
    }
    p += node->feature_dim * sizeof(float);

    /* connections: 每条边保存 target 的 node_id + weight + bias + confidence
     * v0.5.7: node_id 回查防御——target 可能是悬垂指针（RED 修剪删节点后
     * 跨拓扑入边残留），非 NULL 但已 free。回查 net->nodes 确认身份，
     * 不通过则置 0，宁可丢边不崩溃（实测 SIGSEGV @ 此处）。 */
    for (int i = 0; i < node->edge_count; i++) {
        int target_id = 0;
        if (node->edges && node->edges[i].target && net) {
            ReasoningNode* tgt = node->edges[i].target;
            if (tgt->node_id >= 0 && tgt->node_id < net->node_count &&
                net->nodes[tgt->node_id] == tgt) {
                target_id = tgt->node_id;
            }
        }
        float w  = (node->edges && i < node->edge_capacity) ? node->edges[i].weight : 0.0f;
        float mb = (node->edges && i < node->edge_capacity) ? node->edges[i].motivational_bias : 0.0f;
        float cf = (node->edges && i < node->edge_capacity) ? node->edges[i].confidence : 0.0f;

        memcpy(p, &target_id, 4);  p += 4;
        memcpy(p, &w, 4);          p += 4;
        memcpy(p, &mb, 4);         p += 4;
        memcpy(p, &cf, 4);         p += 4;
    }

    return (int)(p - buf);
}

/** 写入节点到文件的指定偏移 */
static int nc_write_node_at(NodeCache* nc, HuarongTopologyNet* net, ReasoningNode* node, int64_t offset) {
    (void)net;
    int size = nc_node_data_size(node);
    if (size <= 0 || size > (1<<24)) return -1;  /* 16MB 上限 */

    uint8_t* buf = (uint8_t*)malloc(size);
    if (!buf) return -1;

    int written = nc_serialize_node(net, node, buf, size);
    if (written > 0) {
        fseeko(nc->fp, offset, SEEK_SET);
        size_t _nw2 = fwrite(buf, 1, (size_t)written, nc->fp);
        (void)_nw2;
        fflush(nc->fp);
    }
    free(buf);
    return (written > 0) ? 0 : -1;
}

/* ================================================================
 *  公共 API
 * ================================================================ */

int node_cache_save_node(NodeCache* nc, HuarongTopologyNet* net, ReasoningNode* node) {
    if (!nc || !node) return -1;

    pthread_mutex_lock(&nc->lock);

    int64_t offset;
    if (!bitmap_test(nc->bitmap, node->node_id)) {
        /* 首次写入：追加到文件末尾 */
        fseeko(nc->fp, 0, SEEK_END);
        offset = ftello(nc->fp);
        nc->offsets[node->node_id] = offset;
    } else {
        offset = nc->offsets[node->node_id];
    }

    if (nc_write_node_at(nc, net, node, offset) < 0) {
        pthread_mutex_unlock(&nc->lock);
        return -1;
    }

    int size = nc_node_data_size(node);
    nc->sizes[node->node_id] = size;
    bitmap_set(nc->bitmap, node->node_id);

    pthread_mutex_unlock(&nc->lock);
    return 0;
}

int node_cache_freeze(NodeCache* nc, HuarongTopologyNet* net, ReasoningNode* node) {
    if (!nc || !node || node->is_cooled) return -1;
    /* v0.5.13 fix: node_id 越界保护（cap < 实际节点时写越界） */
    if (node->node_id < 0 || node->node_id >= nc->node_count) return -1;

    /* 1. 保存到文件 */
    if (node_cache_save_node(nc, net, node) < 0) return -1;

    /* 持节点锁后释放连接内存（防止与 self_learner / brainstem 竞争） */
    int lock_idx = node->node_id & (PM_NODE_LOCK_COUNT - 1);
    pthread_mutex_lock(&net->node_locks[lock_idx]);

    /* 2. 释放连接相关内存（Edges 和 conn_hash 都走延迟释放防 use-after-free）
     *    先置 NULL 断活引用，再入退役链表——防止 read 线程在 retire→free
     *    窗口内读到悬空指针。 */
    {
        Edge*     old_edges = node->edges;
        void*     old_hash  = node->conn_hash;
        node->edges       = NULL;
        node->edge_count  = 0;
        node->edge_capacity = 0;
        node->conn_hash   = NULL;
        node->conn_hash_mask = -1;
        node->conn_hash_entries = 0;
        if (old_edges) huarong_net_retire_blob(net, old_edges);
        if (old_hash)  huarong_net_retire_blob(net, old_hash);
    }
    node->is_cooled = 1;

    /* 3. 释放特征向量（惰性分配后也需腾出内存） */
    if (node->features) {
        free(node->features);
        node->features = NULL;
    }

    __sync_fetch_and_add(&nc->total_freezes, 1);

    pthread_mutex_unlock(&net->node_locks[lock_idx]);
    return 0;
}

/* v0.5.13: restore_features=0 时跳过特征向量恢复——对话路径只需要边，
 * 省内存、加快解冻（热-冷呼吸：对话后 activation 衰减自然再冻） */
int node_cache_thaw(NodeCache* nc, HuarongTopologyNet* net, ReasoningNode* node,
                    int restore_features) {
    if (!nc || !node || !node->is_cooled) return -1;
    if (!node->concept) return -1;
    /* v0.5.13 fix: node_id 越界保护（cap < 实际节点时 bitmap_test 越界读） */
    if (node->node_id < 0 || node->node_id >= nc->node_count) return -1;
    if (!bitmap_test(nc->bitmap, node->node_id)) {
        /* v0.5.13 fix: 假解冻——"从未保存过"不再清 is_cooled（热但 0 边
         * 比冻着更糟：冻着对话会尝试 thaw，0 边彻底无路）。返回 -1 保留
         * 冻结标记，等 cache 重建/有数据后重试。 */
        return -1;
    }

    pthread_mutex_lock(&nc->lock);

    int64_t offset = nc->offsets[node->node_id];
    int size = nc->sizes[node->node_id];
    if (offset <= 0 || size <= 0) {
        pthread_mutex_unlock(&nc->lock);
        return -1;
    }

    uint8_t* buf = (uint8_t*)malloc(size);
    if (!buf) { pthread_mutex_unlock(&nc->lock); return -1; }

    fseeko(nc->fp, offset, SEEK_SET);
    int nread = (int)fread(buf, 1, size, nc->fp);
    pthread_mutex_unlock(&nc->lock);

    if (nread < NC_NODE_HDR_SZ) { free(buf); return -1; }

    /* v0.5.10 fix: 重建段持 node_locks——与 freeze（node_cache.c:259）
     * 对称。thaw 写 node->features/edges/conn_hash 期间，brainstem freeze
     * 或学习线程（autonomic_learner 持 node_locks 加边）并发作用于同一
     * 节点会丢学习成果/状态横跳。锁序: nc->lock → node_locks，freeze
     * 只持 node_locks 不碰 nc->lock，无环。 */
    int li = node->node_id & (PM_NODE_LOCK_COUNT - 1);
    pthread_mutex_lock(&net->node_locks[li]);

    /* 解析数据块 */
    uint8_t* p = buf;
    int concept_len, feat_dim, conn_count;
    memcpy(&concept_len, p, 4);  p += 4;
    memcpy(&feat_dim,    p, 4);  p += 4;
    memcpy(&conn_count,  p, 4);  p += 4;

    /* concept（已在内存，跳过比对 — 文件中的和 struct 中的应该一致） */
    p += concept_len + 1;

    /* features（v0.5.13: restore_features=0 时跳过——对话路径只走边） */
    if (restore_features && feat_dim > 0 && !node->features) {
        node->features = (float*)calloc(feat_dim, sizeof(float));
        if (node->features) {
            memcpy(node->features, p, feat_dim * sizeof(float));
            node->feature_dim = feat_dim;
        }
    }
    p += feat_dim * sizeof(float);

    /* 重建 connections 数组 */
    if (conn_count > 0) {
        if (node->edges) free(node->edges);   /* v0.5.13 fix: 防泄漏——学习线程可能已分配 */
        int cap = conn_count + 4;  /* 留一点扩容空间 */
        node->edges = (Edge*)calloc(cap, sizeof(Edge));
            
        if (!node->edges) {
            pthread_mutex_unlock(&net->node_locks[li]);
            free(buf);
            return -1;
        }
        node->edge_capacity = cap;
        node->edge_count = conn_count;

        for (int i = 0; i < conn_count; i++) {
            int target_id;
            float w, mb, cf;
            memcpy(&target_id, p, 4);  p += 4;
            memcpy(&w, p, 4);          p += 4;
            memcpy(&mb, p, 4);         p += 4;
            memcpy(&cf, p, 4);         p += 4;

            node->edges[i].target = nc_lookup_node(net, target_id);
            node->edges[i].weight = w;
            if (node->edges) node->edges[i].motivational_bias = mb;
            if (node->edges) node->edges[i].confidence = cf;
        }

        /* 重建连接哈希表 */
        if (node->edges[0].target) {
            int hash_cap = 16;
            while (hash_cap < conn_count * 2) hash_cap *= 2;
            node->conn_hash = (ConnHashEntry*)calloc(hash_cap, sizeof(ConnHashEntry));
            if (node->conn_hash) {
                node->conn_hash_mask = hash_cap - 1;
                for (int i = 0; i < conn_count; i++) {
                    if (node->edges[i].target) {
                        node_conn_hash_insert(NULL, node, node->edges[i].target, i);
                    }
                }
            }
        }
    }

    free(buf);
    pthread_mutex_unlock(&net->node_locks[li]);
    node->is_cooled = 0;
    __sync_fetch_and_add(&nc->total_thaws, 1);
    return 0;
}

/* v0.5.7: 批量解冻所有冻结节点（存盘前调用）。
 * 此前冻结（lazy memory）的边存 brain_state.dat，但主状态
 * 存盘/加载不回读 → 冻结 = 永久丢边 → 运行越久边越少 →
 * 存盘边少 → 加载孤立 → prune 删光（vocab 3万→52 实测）。
 * 存盘前全部 thaw 回内存，主状态包含所有边，闭环完整。 */
int node_cache_thaw_all(NodeCache* nc, MasterTopology* master) {
    if (!nc || !master) return 0;
    int thawed = 0;
    for (int t = 0; t < master->sub_topo_count; t++) {
        SubTopology* sub = master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        for (int n = 0; n < sub->net->node_count; n++) {
            ReasoningNode* node = sub->net->nodes[n];
            if (!node || !node->is_cooled) continue;
            if (node_cache_thaw(nc, sub->net, node, 1) == 0) thawed++;
        }
    }
    return thawed;
}

/* v0.5.7: 存盘导出——冻结节点的边从 brain_state.dat 读回，
 * 直接写入主状态文件（不落地内存——thaw_all 的内存峰值替代方案）。
 * 冻结数据块: hdr(12B) + concept + features + edges(16B each)
 * 主状态边格式: tgt_len(4) + tgt_concept + w(4) + mb(4) + cf(4) */
int node_cache_export_frozen_edges(NodeCache* nc, MasterTopology* master,
                                   HuarongTopologyNet* net, ReasoningNode* node,
                                   FILE* fp) {
    /* v0.5.7: 失败返回 -1（不写任何字节），成功返回导出边数（≥0）。
     * 调用方 master_save_state 靠 <0 识别"文件流缺失"并补写 conn_count=0。
     * 此前失败返回 0 与"成功但 0 条边"无法区分→存盘文件流错位→
     * 加载只剩 1 节点（实测 216M 状态读完只剩"我"） */
    if (!nc || !node || !node->is_cooled || !fp) return -1;
    if (!bitmap_test(nc->bitmap, node->node_id)) return -1;

    pthread_mutex_lock(&nc->lock);
    int64_t offset = nc->offsets[node->node_id];
    int size = nc->sizes[node->node_id];
    if (offset <= 0 || size <= 0) { pthread_mutex_unlock(&nc->lock); return -1; }
    uint8_t* buf = (uint8_t*)malloc((size_t)size);
    if (!buf) { pthread_mutex_unlock(&nc->lock); return -1; }
    fseeko(nc->fp, offset, SEEK_SET);
    int nread = (int)fread(buf, 1, (size_t)size, nc->fp);
    pthread_mutex_unlock(&nc->lock);
    if (nread < 12) { free(buf); return -1; }

    uint8_t* p = buf;
    int concept_len, feat_dim, conn_count;
    memcpy(&concept_len, p, 4); p += 4;
    memcpy(&feat_dim, p, 4);    p += 4;
    memcpy(&conn_count, p, 4);  p += 4;
    if (concept_len < 0 || concept_len > 4096 || conn_count < 0 || conn_count > 65536) {
        free(buf); return -1;
    }
    p += concept_len + 1;
    p += (size_t)feat_dim * sizeof(float);

    /* 主状态格式：conn_count(4) + 边(tgt_len+concept+3f) */
    fwrite(&conn_count, sizeof(int), 1, fp);
    int written = 0;
    for (int i = 0; i < conn_count; i++) {
        int target_id; float w, mb, cf;
        memcpy(&target_id, p, 4);  p += 4;
        memcpy(&w, p, 4);          p += 4;
        memcpy(&mb, p, 4);         p += 4;
        memcpy(&cf, p, 4);         p += 4;
        /* v0.5.10 fix: 目标查找限定源拓扑（net）——此前全图按 node_id
         * 乱找，其他拓扑同 index 节点会被误当目标（边写错/写漏）。
         * 目标缺失时补写 tlen=0 + 3f 占位，保证文件流 conn_count 与
         * 实际记录数一致——否则加载 Pass1 错位 → "读取失败（数据
         * 不完整）" → 部分加载 0 边 → 级联塌缩（08-08 16:34→18:51
         * →08-09 08:32 加载 247万→42.8万→791 链接，实测）。 */
        const char* tgt_concept = NULL;
        if (target_id >= 0 && target_id < net->node_count) {
            ReasoningNode* tn = net->nodes[target_id];
            if (tn && tn->concept) tgt_concept = tn->concept;
        }
        if (tgt_concept) {
            int tlen = (int)strlen(tgt_concept) + 1;
            fwrite(&tlen, sizeof(int), 1, fp);
            fwrite(tgt_concept, 1, (size_t)tlen, fp);
            fwrite(&w, sizeof(float), 1, fp);
            fwrite(&mb, sizeof(float), 1, fp);
            fwrite(&cf, sizeof(float), 1, fp);
            written++;
        } else {
            /* 占位：目标缺失（已删除/悬垂）——tlen=0 + 3f，流不断 */
            int zero_tlen = 0;
            float zero = 0.0f;
            fwrite(&zero_tlen, sizeof(int), 1, fp);
            fwrite(&zero, sizeof(float), 1, fp);
            fwrite(&zero, sizeof(float), 1, fp);
            fwrite(&zero, sizeof(float), 1, fp);
        }
    }
    free(buf);
    return written;
}
