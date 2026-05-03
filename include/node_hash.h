#ifndef NODE_HASH_H
#define NODE_HASH_H

#include "huarong_topology.h"

/**
 * 节点哈希表 - 加速节点查找
 * 使用链地址法处理冲突
 */

/**
 * 哈希表中的链表节点
 */
typedef struct NodeHashEntry {
    ReasoningNode* node;          // 指向实际节点
    struct NodeHashEntry* next;   // 链表下一个节点
} NodeHashEntry;

/**
 * 节点哈希表结构
 */
typedef struct NodeHashTable {
    NodeHashEntry** buckets;      // 哈希桶数组
    int bucket_count;             // 桶数量
    int node_count;               // 节点总数
} NodeHashTable;

/**
 * 创建节点哈希表
 * @param bucket_count 桶数量（建议使用素数，如 1009, 2003, 5003）
 * @return 哈希表指针
 */
NodeHashTable* node_hash_create(int bucket_count);

/**
 * 释放哈希表
 * @param hash 哈希表指针
 */
void node_hash_free(NodeHashTable* hash);

/**
 * 添加节点到哈希表
 * @param hash 哈希表指针
 * @param node 要添加的节点
 * @return 0 成功, -1 失败
 */
int node_hash_add(NodeHashTable* hash, ReasoningNode* node);

/**
 * 从哈希表中查找节点
 * @param hash 哈希表指针
 * @param concept 概念名称
 * @return 节点指针，未找到返回 NULL
 */
ReasoningNode* node_hash_find(NodeHashTable* hash, const char* concept);

/**
 * 从哈希表中删除节点
 * @param hash 哈希表指针
 * @param concept 概念名称
 * @return 0 成功, -1 未找到
 */
int node_hash_remove(NodeHashTable* hash, const char* concept);

/**
 * 清空哈希表（不释放节点本身）
 * @param hash 哈希表指针
 */
void node_hash_clear(NodeHashTable* hash);

/**
 * 获取哈希表统计信息
 * @param hash 哈希表指针
 * @param max_chain_length 最大链长度（输出）
 * @param avg_chain_length 平均链长度（输出）
 */
void node_hash_stats(NodeHashTable* hash, 
                    int* max_chain_length, 
                    float* avg_chain_length);

/**
 * 批量添加节点（从 HuarongTopologyNet）
 * @param hash 哈希表指针
 * @param net 拓扑网络指针
 * @return 成功添加的节点数量
 */
int node_hash_add_all_from_net(NodeHashTable* hash, HuarongTopologyNet* net);

// P0-2: 扩展API

/**
 * 预留节点容量（提前分配内存，避免多次扩容）
 * @param hash 哈希表指针
 * @param node_count 预计节点数量
 */
void node_hash_reserve(NodeHashTable* hash, int node_count);

/**
 * 获取哈希表统计信息（扩展版）
 * @param hash 哈希表指针
 * @param max_chain_length 最大链长度（输出）
 * @param avg_chain_length 平均链长度（输出）
 * @param load_factor 负载因子（输出）：node_count / bucket_count
 * @param memory_usage 估算内存使用（输出，字节）
 */
void node_hash_stats_ex(NodeHashTable* hash,
                       int* max_chain_length,
                       float* avg_chain_length,
                       float* load_factor,
                       size_t* memory_usage);

/**
 * 打印哈希表详细信息（调试用）
 * @param hash 哈希表指针
 */
void node_hash_print_info(NodeHashTable* hash);

/**
 * 检查哈希表完整性
 * @param hash 哈希表指针
 * @param net 关联的拓扑网络（用于验证节点引用）
 * @return 0 正常, -1 存在问题
 */
int node_hash_validate(NodeHashTable* hash, HuarongTopologyNet* net);

/**
 * 哈希表关键字搜索（前缀匹配）
 * @param hash 哈希表指针
 * @param prefix 前缀字符串
 * @param results 输出结果数组（需外部分配）
 * @param max_results 最大结果数量
 * @return 实际结果数量
 */
int node_hash_search_by_prefix(NodeHashTable* hash, const char* prefix,
                               ReasoningNode** results, int max_results);

#endif // NODE_HASH_H
