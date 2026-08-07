/**
 * @file node_cache.h
 * @brief 大脑式节点冷热缓存 — 激活的进内存，不激活的完整保存在文件
 *
 * 设计哲学（模仿大脑工作机制）：
 *   - 海马体类比：所有神经回路物理存在，不会"删除"或"压缩"
 *   - 激活扩散类比：只有激活值 > 阈值的节点在内存中（热节点）
 *   - 静息电位类比：冷却节点结构完整保存在磁盘，需要时 fseek+fread 唤醒
 *   - NO swap/NO compress/NO rebuild — 数据和连接永不丢失，只是不在内存
 *
 * 文件格式（索引式随机访问）：
 *   [Header: 4B magic + 4B version + 4B node_count + 4B reserved]
 *   [Index: 256KB bitmap (每 bit 标记一个节点的数据是否已写入)]
 *   [Index table: node_id(4B) + file_offset(8B) + data_size(4B) = 16B * node_count]
 *   [Data blocks: 每个节点一块, 可变长]
 *
 * 线程安全：内部 mutex 保护所有文件操作
 */

#ifndef NODE_CACHE_H
#define NODE_CACHE_H

#include "huarong_topology.h"
typedef struct MasterTopology MasterTopology;
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 节点缓存句柄 */
typedef struct NodeCache {
    char   filepath[512];       /* 状态文件路径 */
    FILE*  fp;                  /* 随机访问文件句柄 (r+b) */
    int    node_count;          /* 索引表容量 */
    int    total_hot;           /* 当前热节点数（大致计数） */
    int    total_cooled;        /* 当前冷却节点数 */
    long   total_thaws;         /* 累计唤醒次数 */
    long   total_freezes;       /* 累计冻结次数 */
    int    auto_thaw_ok;        /* 自动解冻开关：内存充足=1，紧张=0 */

    /* 索引表（内存驻留，O(1) 定位） */
    uint8_t*  bitmap;           /* 节点存在位图 (node_count bits) */
    int64_t*  offsets;          /* 文件偏移表 [node_id] */
    int32_t*  sizes;            /* 数据块大小表 [node_id] */

    pthread_mutex_t lock;       /* 文件操作互斥锁 */
} NodeCache;

/**
 * 创建节点缓存（打开/初始化状态文件）
 * @param filepath  状态文件路径（如 "brain_state.dat"）
 * @param node_cap  预期最大节点数（用于预分配索引表）
 * @return          缓存句柄，失败返回 NULL
 */
NodeCache* node_cache_create(const char* filepath, int node_cap);

/**
 * 关闭节点缓存（确保所有热节点写回文件）
 */
void node_cache_destroy(NodeCache* nc);

/**
 * 保存一个热节点到文件（首次写入或更新）
 * @param nc    缓存句柄
 * @param net   节点所属拓扑网络（用于解析连接指针 → node_id）
 * @param node  要保存的节点
 * @return      0 成功, -1 失败
 */
int node_cache_save_node(NodeCache* nc, HuarongTopologyNet* net, ReasoningNode* node);

/**
 * 冻结节点：保存到文件 + 释放连接内存
 * 仅释放 connections/weights/hash 等数组；struct 和 concept 字符串保留
 * @return 0 成功, -1 失败
 */
int node_cache_freeze(NodeCache* nc, HuarongTopologyNet* net, ReasoningNode* node);

/**
 * 唤醒节点：从文件读回连接数据 + 重建内存结构
 * 需要 net 来解析 node_id → ReasoningNode 指针（用于 conn_hash）
 * @return 0 成功, -1 失败
 */
int node_cache_thaw(NodeCache* nc, HuarongTopologyNet* net, ReasoningNode* node);
int node_cache_thaw_all(NodeCache* nc, MasterTopology* master);
/* v0.5.10: 存盘导出冻结边（master_save_state 调用，缺声明→gcc14 隐式声明报错） */
int node_cache_export_frozen_edges(NodeCache* nc, MasterTopology* master,
                                   HuarongTopologyNet* net, ReasoningNode* node,
                                   FILE* fp);

/**
 * 查询节点是否已冷却
 */
static inline int node_is_cooled(ReasoningNode* node) {
    return node ? node->is_cooled : 0;
}

/**
 * 设置自动解冻开关
 * @param nc      缓存句柄
 * @param allowed  1=允许自动解冻, 0=禁止
 */
static inline void node_cache_set_auto_thaw(NodeCache* nc, int allowed) {
    if (nc) nc->auto_thaw_ok = allowed;
}

#ifdef __cplusplus
}
#endif

#endif /* NODE_CACHE_H */
