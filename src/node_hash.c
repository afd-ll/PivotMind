#include "node_hash.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/**
 * DJB2 字符串哈希函数
 * 效果好且计算快速
 */
static unsigned int hash_string(const char* str, int bucket_count) {
    if (!str) return 0;
    
    unsigned int hash = 5381;
    int c;
    
    while ((c = (unsigned char)*str++)) {
        // hash * 33 + c
        hash = ((hash << 5) + hash) + c;
    }
    
    return hash % bucket_count;
}

/**
 * 创建节点哈希表
 */
NodeHashTable* node_hash_create(int bucket_count) {
    if (bucket_count <= 0) {
        bucket_count = 1009;  // 默认使用素数
    }
    
    NodeHashTable* hash = (NodeHashTable*)calloc(1, sizeof(NodeHashTable));
    if (!hash) {
        return NULL;
    }
    
    hash->buckets = (NodeHashEntry**)calloc(bucket_count, sizeof(NodeHashEntry*));
    if (!hash->buckets) {
        free(hash);
        return NULL;
    }
    
    hash->bucket_count = bucket_count;
    hash->node_count = 0;
    
    return hash;
}

/**
 * 释放哈希表
 */
void node_hash_free(NodeHashTable* hash) {
    if (!hash) return;
    
    // 释放所有链表节点
    for (int i = 0; i < hash->bucket_count; i++) {
        NodeHashEntry* entry = hash->buckets[i];
        while (entry) {
            NodeHashEntry* next = entry->next;
            free(entry);
            entry = next;
        }
    }
    
    free(hash->buckets);
    free(hash);
}

/**
 * 添加节点到哈希表
 */
int node_hash_add(NodeHashTable* hash, ReasoningNode* node) {
    if (!hash || !node || !node->concept) {
        return -1;
    }
    
    // 检查是否已存在
    if (node_hash_find(hash, node->concept) != NULL) {
        // 节点已存在，不重复添加
        return 0;
    }
    
    // 计算哈希值
    unsigned int idx = hash_string(node->concept, hash->bucket_count);
    
    // 创建新的哈希条目
    NodeHashEntry* new_entry = (NodeHashEntry*)malloc(sizeof(NodeHashEntry));
    if (!new_entry) {
        return -1;
    }
    
    new_entry->node = node;
    new_entry->next = hash->buckets[idx];
    hash->buckets[idx] = new_entry;
    hash->node_count++;
    
    return 0;
}

/**
 * 从哈希表中查找节点
 */
ReasoningNode* node_hash_find(NodeHashTable* hash, const char* concept) {
    if (!hash || !concept) {
        return NULL;
    }
    
    unsigned int idx = hash_string(concept, hash->bucket_count);
    NodeHashEntry* entry = hash->buckets[idx];
    
    // 遍历链表查找
    while (entry) {
        if (entry->node && entry->node->concept) {
            if (strcmp(entry->node->concept, concept) == 0) {
                return entry->node;
            }
        }
        entry = entry->next;
    }
    
    return NULL;
}

/**
 * 从哈希表中删除节点
 */
int node_hash_remove(NodeHashTable* hash, const char* concept) {
    if (!hash || !concept) {
        return -1;
    }
    
    unsigned int idx = hash_string(concept, hash->bucket_count);
    NodeHashEntry* entry = hash->buckets[idx];
    NodeHashEntry* prev = NULL;
    
    while (entry) {
        if (entry->node && entry->node->concept) {
            if (strcmp(entry->node->concept, concept) == 0) {
                // 找到，从链表中删除
                if (prev) {
                    prev->next = entry->next;
                } else {
                    hash->buckets[idx] = entry->next;
                }
                
                free(entry);
                hash->node_count--;
                return 0;
            }
        }
        prev = entry;
        entry = entry->next;
    }
    
    return -1;  // 未找到
}

/**
 * 清空哈希表
 */
void node_hash_clear(NodeHashTable* hash) {
    if (!hash) return;
    
    for (int i = 0; i < hash->bucket_count; i++) {
        NodeHashEntry* entry = hash->buckets[i];
        while (entry) {
            NodeHashEntry* next = entry->next;
            free(entry);
            entry = next;
        }
        hash->buckets[i] = NULL;
    }
    
    hash->node_count = 0;
}

/**
 * 获取哈希表统计信息
 */
void node_hash_stats(NodeHashTable* hash, 
                    int* max_chain_length, 
                    float* avg_chain_length) {
    if (!hash) {
        if (max_chain_length) *max_chain_length = 0;
        if (avg_chain_length) *avg_chain_length = 0.0f;
        return;
    }
    
    int max_len = 0;
    int total_len = 0;
    int non_empty_buckets = 0;
    
    for (int i = 0; i < hash->bucket_count; i++) {
        int len = 0;
        NodeHashEntry* entry = hash->buckets[i];
        while (entry) {
            len++;
            entry = entry->next;
        }
        
        if (len > 0) {
            non_empty_buckets++;
            total_len += len;
            if (len > max_len) {
                max_len = len;
            }
        }
    }
    
    if (max_chain_length) {
        *max_chain_length = max_len;
    }
    
    if (avg_chain_length) {
        if (non_empty_buckets > 0) {
            *avg_chain_length = (float)total_len / non_empty_buckets;
        } else {
            *avg_chain_length = 0.0f;
        }
    }
}

/**
 * 批量添加节点（从 HuarongTopologyNet）
 */
int node_hash_add_all_from_net(NodeHashTable* hash, HuarongTopologyNet* net) {
    if (!hash || !net) {
        return 0;
    }

    int added_count = 0;

    for (int i = 0; i < net->node_count; i++) {
        ReasoningNode* node = net->nodes[i];
        if (node && node_hash_add(hash, node) == 0) {
            added_count++;
        }
    }

    return added_count;
}

// P0-2: 扩展API实现 ====================

/**
 * 预留节点容量（提前分配内存，避免多次扩容）
 */
void node_hash_reserve(NodeHashTable* hash, int node_count) {
    if (!hash || node_count <= 0) return;

    // 计算合适的桶数量（负载因子约0.75）
    int ideal_buckets = (int)(node_count / 0.75f) + 1;

    // 如果当前桶数足够，不需要扩容
    if (hash->bucket_count >= ideal_buckets) return;

    // 找到下一个素数作为桶数量
    int new_buckets = ideal_buckets | 1;  // 确保为奇数
    while (1) {
        int is_prime = 1;
        for (int i = 3; i * i <= new_buckets; i += 2) {
            if (new_buckets % i == 0) {
                is_prime = 0;
                break;
            }
        }
        if (is_prime) break;
        new_buckets += 2;
    }

    // 重新分配桶数组
    NodeHashEntry** new_buckets_arr = (NodeHashEntry**)calloc(new_buckets, sizeof(NodeHashEntry*));
    if (!new_buckets_arr) return;

    // 重新哈希所有现有条目
    for (int i = 0; i < hash->bucket_count; i++) {
        NodeHashEntry* entry = hash->buckets[i];
        while (entry) {
            NodeHashEntry* next = entry->next;
            unsigned int new_idx = hash_string(entry->node->concept, new_buckets);
            entry->next = new_buckets_arr[new_idx];
            new_buckets_arr[new_idx] = entry;
            entry = next;
        }
    }

    free(hash->buckets);
    hash->buckets = new_buckets_arr;
    hash->bucket_count = new_buckets;
}

/**
 * 获取哈希表统计信息（含链长分布和负载因子）
 */
void node_hash_stats_ex(NodeHashTable* hash,
                       int* max_chain_length,
                       float* avg_chain_length,
                       float* load_factor,
                       size_t* memory_usage) {
    if (!hash) {
        if (max_chain_length) *max_chain_length = 0;
        if (avg_chain_length) *avg_chain_length = 0.0f;
        if (load_factor) *load_factor = 0.0f;
        if (memory_usage) *memory_usage = 0;
        return;
    }

    int max_len = 0;
    int total_len = 0;
    int non_empty_buckets = 0;

    for (int i = 0; i < hash->bucket_count; i++) {
        int len = 0;
        NodeHashEntry* entry = hash->buckets[i];
        while (entry) {
            len++;
            entry = entry->next;
        }

        if (len > 0) {
            non_empty_buckets++;
            total_len += len;
            if (len > max_len) {
                max_len = len;
            }
        }
    }

    if (max_chain_length) *max_chain_length = max_len;
    if (avg_chain_length) {
        *avg_chain_length = (non_empty_buckets > 0) ?
                           (float)total_len / non_empty_buckets : 0.0f;
    }
    if (load_factor) {
        *load_factor = (float)hash->node_count / hash->bucket_count;
    }
    if (memory_usage) {
        // 估算内存使用：桶数组 + 条目 + 节点指针
        *memory_usage = hash->bucket_count * sizeof(NodeHashEntry*) +
                       hash->node_count * sizeof(NodeHashEntry);
    }
}

/**
 * 打印哈希表详细信息（调试用）
 */
void node_hash_print_info(NodeHashTable* hash) {
    if (!hash) {
        printf("[哈希表] NULL\n");
        return;
    }

    int max_chain;
    float avg_chain, load_factor;
    size_t mem_usage;

    node_hash_stats_ex(hash, &max_chain, &avg_chain, &load_factor, &mem_usage);

    // 统计链长度分布
    int dist[10] = {0};  // 0-1, 1-2, 2-5, 5-10, 10+
    for (int i = 0; i < hash->bucket_count; i++) {
        int len = 0;
        NodeHashEntry* entry = hash->buckets[i];
        while (entry) { len++; entry = entry->next; }

        if (len == 0) dist[0]++;
        else if (len == 1) dist[1]++;
        else if (len <= 2) dist[2]++;
        else if (len <= 5) dist[3]++;
        else if (len <= 10) dist[4]++;
        else dist[5]++;
    }

    printf("\n=== 哈希表信息 ===\n");
    printf("  节点数: %d\n", hash->node_count);
    printf("  桶数量: %d\n", hash->bucket_count);
    printf("  负载因子: %.3f\n", load_factor);
    printf("  最大链长度: %d\n", max_chain);
    printf("  平均链长度: %.2f\n", avg_chain);
    printf("  内存使用: %zu bytes\n", mem_usage);
    printf("  桶分布: 空=%d, 1=%d, 2=%d, 3-5=%d, 6-10=%d, >10=%d\n",
           dist[0], dist[1], dist[2], dist[3], dist[4], dist[5]);
    printf("==================\n\n");
}

/**
 * 检查哈希表完整性
 */
int node_hash_validate(NodeHashTable* hash, HuarongTopologyNet* net) {
    if (!hash) return -1;

    // 检查节点计数一致性
    int actual_count = 0;
    for (int i = 0; i < hash->bucket_count; i++) {
        NodeHashEntry* entry = hash->buckets[i];
        while (entry) {
            actual_count++;
            entry = entry->next;
        }
    }

    if (actual_count != hash->node_count) {
        printf("[哈希表] 验证失败：记录节点数=%d，实际节点数=%d\n",
               hash->node_count, actual_count);
        return -1;
    }

    // 如果提供了网络指针，检查节点引用有效性
    if (net) {
        for (int i = 0; i < hash->bucket_count; i++) {
            NodeHashEntry* entry = hash->buckets[i];
            while (entry) {
                if (entry->node) {
                    // 检查节点是否在网络的有效节点数组中
                    int valid = 0;
                    for (int j = 0; j < net->node_count; j++) {
                        if (net->nodes[j] == entry->node) {
                            valid = 1;
                            break;
                        }
                    }
                    if (!valid) {
                        printf("[哈希表] 验证失败：节点 %p 不在网络中\n",
                               (void*)entry->node);
                        return -1;
                    }
                }
                entry = entry->next;
            }
        }
    }

    return 0;
}

/**
 * 哈希表关键字搜索（前缀匹配）
 */
int node_hash_search_by_prefix(NodeHashTable* hash, const char* prefix,
                               ReasoningNode** results, int max_results) {
    if (!hash || !prefix || !results || max_results <= 0) return 0;

    int prefix_len = strlen(prefix);
    if (prefix_len == 0) return 0;

    int found = 0;

    // 遍历所有桶
    for (int i = 0; i < hash->bucket_count && found < max_results; i++) {
        NodeHashEntry* entry = hash->buckets[i];
        while (entry && found < max_results) {
            if (entry->node && entry->node->concept) {
                // 检查前缀匹配
                if (strncmp(entry->node->concept, prefix, prefix_len) == 0) {
                    results[found++] = entry->node;
                }
            }
            entry = entry->next;
        }
    }

    return found;
}
