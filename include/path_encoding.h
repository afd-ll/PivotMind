#ifndef PATH_ENCODING_H
#define PATH_ENCODING_H

#include "constants.h"
#include <pthread.h>
#include <stdint.h>

#define PATH_TRIPLET_TABLE_SIZE PM_PATH_TRIPLET_TABLE

/* ================================================================
 *  路径三元组频率统计
 *
 *  目标: 在拓扑走边过程中增量记录三元组 (A→B→C) 的出现频率，
 *  用于后续模板抽象。不可分解的三元组代表稳定的低阶路径模式。
 * ================================================================ */

/**
 * 路径三元组记录（开放寻址哈希桶槽位）
 */
typedef struct {
    int node_a;             /* 路径第 n-2 步节点 */
    int node_b;             /* 路径第 n-1 步节点 */
    int node_c;             /* 路径第 n 步节点   */
    int topo_id;            /* 所属拓扑 ID */
    int count;              /* 累积出现次数 */
    int last_seen;          /* 最后出现轮次 (用于 LRU 淘汰) */
    int is_active;          /* 1=有效槽位, 0=空槽 */
} PathTripletRecord;

/**
 * 路径频率表
 *
 * 开放寻址哈希表，线性探测冲突处理。
 * 容量 PATH_TRIPLET_TABLE_SIZE (65536) 桶。
 * LRU 淘汰: 表满时淘汰 last_seen 最早的条目。
 */
typedef struct PathFrequencyTable {
    PathTripletRecord* buckets;   /* 桶数组 */
    int capacity;                 /* 桶数 (= PATH_TRIPLET_TABLE_SIZE) */
    int entry_count;              /* 当前活跃条目数 */
    int64_t total_triplets;       /* 累计三元组总计数 (含重复, int64 防溢出) */
    int round;                    /* 当前轮次计数器 */
    pthread_mutex_t mutex;        /* 线程安全锁 */
} PathFrequencyTable;

/**
 * 不可分解性分析结果
 *
 * ir_ratio = P(abc) / (P(ab) * P(bc))
 * ir_ratio > 1.0 表示三元组比独立边概率乘积更频繁出现，
 * 即该路径模式不可分解为一阶链。
 */
typedef struct {
    int node_a;
    int node_b;
    int node_c;
    float ir_ratio;         /* 不可分解性比值 */
    int count;              /* 三元组出现次数 */
} IrreducibilityResult;

/* ================================================================
 *  API
 * ================================================================ */

/**
 * 创建路径频率表
 * @param capacity 桶数量 (建议 PATH_TRIPLET_TABLE_SIZE)
 * @return 新表指针，失败返回 NULL
 */
PathFrequencyTable* path_freq_table_create(int capacity);

/**
 * 销毁路径频率表
 */
void path_freq_table_destroy(PathFrequencyTable* table);

/**
 * 记录一条三元组
 *
 * 若 (node_a, node_b, node_c, topo_id) 已存在则 count++，
 * 否则插入新槽位（满时触发 LRU 淘汰）。
 *
 * @param table  频率表
 * @param topo_id 拓扑ID
 * @param a      节点A
 * @param b      节点B
 * @param c      节点C
 * @return 该三元组的当前计数值，失败返回 -1
 */
int path_freq_table_record(PathFrequencyTable* table,
                           int topo_id, int a, int b, int c);

/**
 * 直接插入/设置三元组（无需加锁，用于状态恢复）
 * 
 * 与 record 不同，此函数不使用 mutex（调用者确保单线程），
 * 直接设置 count 而非 incremental。
 *
 * @return 1 成功, -1 失败
 */
int path_freq_table_set(PathFrequencyTable* table,
                        int topo_id, int a, int b, int c,
                        int count);

/**
 * 查找三元组计数
 * @param count_out 输出: 计数
 * @return 1 找到, 0 未找到, -1 参数错误
 */
int path_freq_table_lookup(PathFrequencyTable* table,
                           int topo_id, int a, int b, int c,
                           int* count_out);

/**
 * 频率表遍历迭代器
 *
 * 用法:
 *   int iter = 0;
 *   const PathTripletRecord* rec;
 *   while ((rec = path_freq_table_iter(table, &iter)) != NULL) {
 *       // 使用 rec->node_a, rec->node_b, ...
 *   }
 *
 * @param iter 迭代状态指针，首次调用前置 *iter = 0
 * @return 下一条记录指针，遍历结束返回 NULL
 */
const PathTripletRecord* path_freq_table_iter(
    const PathFrequencyTable* table, int* iter);

/**
 * 不可分解性分析
 *
 * 遍历频率表中所有三元组，计算 ir_ratio = P(abc) / (P(ab) * P(bc))。
 * 仅返回 ir_ratio > 1.0 的结果（即真实频率高于独立概率预期的三元组）。
 *
 * @param table        频率表
 * @param nodes        ReasoningNode* 数组 (传入 void* 避免头文件循环依赖)
 * @param node_count   节点数
 * @param result_count 输出: 结果数量
 * @return 排序后的 IrreducibilityResult 数组 (需调用者 free)
 */
IrreducibilityResult* path_analyze_irreducibility(
    PathFrequencyTable* table,
    void* const* nodes,
    int node_count,
    int* result_count);

#endif /* PATH_ENCODING_H */
