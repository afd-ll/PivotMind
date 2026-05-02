#ifndef MEMORY_SYSTEM_H
#define MEMORY_SYSTEM_H

#include "huarong_topology.h"
#include "causal_reasoning.h"
#include <time.h>

// ==================== 记忆系统核心结构 ==================== 

/**
 * 记忆类型枚举
 */
typedef enum {
    MEMORY_TYPE_FLOAT = 0,      // 浮点数
    MEMORY_TYPE_INT = 1,        // 整数
    MEMORY_TYPE_STRING = 2,     // 字符串
    MEMORY_TYPE_FLOAT_ARRAY = 3, // 浮点数组
    MEMORY_TYPE_INT_ARRAY = 4,   // 整型数组
    MEMORY_TYPE_BINARY = 5,     // 二进制数据
    MEMORY_TYPE_USER_PROFILE = 6, // 用户画像数据
    MEMORY_TYPE_CAUSAL_RULE = 7,  // 因果规则
    MEMORY_TYPE_CAUSAL_GRAPH = 8  // 因果图片段
} MemoryType;

/**
 * 记忆级别枚举
 */
typedef enum {
    MEMORY_LEVEL_CONTEXT = 0,   // 上下文记忆 (置信度 < 0.3)
    MEMORY_LEVEL_SHORT_TERM = 1, // 短期记忆 (0.3 <= 置信度 < 0.6)
    MEMORY_LEVEL_PERMANENT = 2   // 永久记忆 (置信度 >= 0.6)
} MemoryLevel;

/**
 * 记忆来源枚举
 */
typedef enum {
    MEMORY_SOURCE_SHORT_TERM = 0,  // 短期记忆
    MEMORY_SOURCE_LONG_TERM = 1    // 长期记忆
} MemorySource;

/**
 * 记忆条目结构
 */
typedef struct MemoryEntry {
    char* key;                 // 记忆键名
    void* data;                // 记忆数据
    size_t data_size;          // 数据大小
    MemoryType type;           // 数据类型
    float importance;          // 重要性权重 (0.0-1.0)
    int access_count;          // 访问次数
    time_t last_access;        // 最后访问时间
    time_t created_at;         // 创建时间
    float decay_factor;        // 衰减因子
} MemoryEntry;

/**
 * 哈希表条目（用于 O(1) key 查找）
 */
typedef struct MemHashEntry {
    char* key;           // 键（与 MemoryEntry->key 相同）
    int entry_index;     // 对应 entries 数组的索引，-1 表示空槽位
} MemHashEntry;

/**
 * 短期记忆结构（工作记忆）
 */
typedef struct ShortTermMemory {
    MemoryEntry** entries;     // 记忆条目数组
    MemHashEntry* hash_table;  // 哈希表（O(1) 查找）
    int capacity;              // 最大容量
    int size;                  // 当前大小
    int current_index;         // 当前索引（用于循环缓冲区）
} ShortTermMemory;

/**
 * 长期记忆索引结构
 */
typedef struct MemoryIndex {
    char* key;                 // 索引键
    int entry_index;           // 对应条目索引
} MemoryIndex;

/**
 * 长期记忆结构
 */
typedef struct LongTermMemory {
    MemoryEntry** entries;     // 记忆条目数组
    MemoryIndex* index_map;   // 索引映射（保留兼容）
    MemHashEntry* hash_table;   // 哈希表（O(1) 查找）
    int max_entries;           // 最大条目数
    int size;                  // 当前大小
    int index_size;            // 索引大小
} LongTermMemory;

/**
 * 搜索结果结构
 */
typedef struct MemorySearchResult {
    MemoryEntry* entry;        // 找到的记忆条目
    float similarity;          // 相似度得分
    MemorySource source;       // 记忆来源
} MemorySearchResult;

/**
 * 记忆系统主结构
 */
typedef struct MemorySystem {
    ShortTermMemory* context_memory;    // 上下文记忆
    ShortTermMemory* short_term;       // 短期记忆
    LongTermMemory* permanent_memory;   // 永久记忆
    long total_operations;              // 总操作次数
    time_t last_consolidation;          // 上次巩固时间
    float confidence_threshold_1;       // 第一阈值 (0.3)
    float confidence_threshold_2;       // 第二阈值 (0.6)

    // P0-1: 批量延迟保存机制
    int pending_updates;                // 待保存的更新计数
    int batch_threshold;                // 触发批量保存的阈值
    time_t last_save_time;              // 上次保存时间
    time_t save_interval;               // 定时保存间隔(秒)
    int save_enabled;                   // 是否启用延迟保存
} MemorySystem;

// P0-1: 批量延迟保存API

/**
 * 启用/禁用批量延迟保存
 */
void memory_set_batch_save(MemorySystem* memory, int enabled, int threshold, int interval_seconds);

/**
 * 手动触发批量保存（立即保存所有待定更新）
 * @return 保存的条目数量
 */
int memory_flush_pending(MemorySystem* memory);

/**
 * 检查是否需要触发自动保存
 * 应在每次 memory_store 后调用
 */
void memory_check_auto_save(MemorySystem* memory);

// ==================== 核心API函数 ==================== 

/**
 * 创建记忆系统
 */
MemorySystem* memory_system_create(int context_capacity, int stm_capacity, int permanent_capacity);

/**
 * 销毁记忆系统
 */
void memory_system_destroy(MemorySystem* memory);

/**
 * 存储记忆
 */
int memory_store(MemorySystem* memory, const char* key, void* data, 
                size_t data_size, MemoryType type, float confidence);

/**
 * 检索记忆
 */
MemoryEntry* memory_retrieve(MemorySystem* memory, const char* key);

/**
 * 搜索记忆
 */
MemorySearchResult* memory_search(MemorySystem* memory, const char* query, 
                                 int* result_count, float threshold);

/**
 * 记忆巩固（基于置信度的自动转移）
 */
void memory_consolidate(MemorySystem* memory);

/**
 * 更新记忆置信度
 */
void memory_update_confidence(MemorySystem* memory, const char* key, float new_confidence);

/**
 * 获取记忆级别
 */
MemoryLevel get_memory_level(float confidence);

/**
 * 记忆测验学习
 */
int memory_test_and_learn(MemorySystem* memory, const char* key, int test_result);

// ==================== 短期记忆API ==================== 

/**
 * 创建短期记忆
 */
ShortTermMemory* stm_create(int capacity);

/**
 * 销毁短期记忆
 */
void stm_destroy(ShortTermMemory* stm);

/**
 * 短期记忆存储
 */
int stm_store(ShortTermMemory* stm, const char* key, void* data, 
              size_t data_size, MemoryType type, float importance);

/**
 * 短期记忆检索
 */
MemoryEntry* stm_retrieve(ShortTermMemory* stm, const char* key);

// ==================== 长期记忆API ==================== 

/**
 * 创建长期记忆
 */
LongTermMemory* ltm_create(int max_entries);

/**
 * 销毁长期记忆
 */
void ltm_destroy(LongTermMemory* ltm);

/**
 * 长期记忆存储
 */
int ltm_store(LongTermMemory* ltm, const char* key, void* data, 
              size_t data_size, MemoryType type, float importance);

/**
 * 长期记忆检索
 */
MemoryEntry* ltm_retrieve(LongTermMemory* ltm, const char* key);

// ==================== 辅助函数 ==================== 

/**
 * 创建记忆条目
 */
MemoryEntry* create_memory_entry(const char* key, void* data, size_t data_size, 
                                MemoryType type, float importance);

/**
 * 销毁记忆条目
 */
void destroy_memory_entry(MemoryEntry* entry);

/**
 * 计算相似度
 */
float calculate_similarity(const char* query, const char* key);

// ==================== 溯智网络集成API ==================== 

/**
 * 将溯智网络集成到记忆系统
 */
int memory_integrate_with_huarong(MemorySystem* memory, HuarongTopologyNet* net);

// ==================== 因果规则记忆 API ====================

/**
 * 存储因果规则到记忆系统
 * @param memory 记忆系统
 * @param cause_key 原因概念键
 * @param effect_key 效果概念键
 * @param rule_strength 因果强度
 * @param rule 置信度结构（可为 NULL）
 * @return 0 成功
 */
int memory_store_causal_rule(MemorySystem* memory, const char* cause_key,
                            const char* effect_key, float rule_strength,
                            CausalConfidence* rule);

/**
 * 从记忆系统检索因果规则
 * @param memory 记忆系统
 * @param cause_key 原因概念键
 * @param effect_key 效果概念键
 * @param out_rule 输出置信度结构（可为 NULL）
 * @return 因果强度，-1 表示未找到
 */
float memory_get_causal_rule(MemorySystem* memory, const char* cause_key,
                            const char* effect_key, CausalConfidence* out_rule);

/**
 * 搜索包含指定概念的因果规则
 * @param memory 记忆系统
 * @param concept 概念键
 * @param max_results 最大结果数
 * @param results 输出结果数组（需预先分配）
 * @return 实际结果数
 */
int memory_search_causal_rules(MemorySystem* memory, const char* concept,
                              int max_results, char results[][512]);

/**
 * 获取因果规则的记忆级别
 * @param memory 记忆系统
 * @param cause_key 原因概念键
 * @param effect_key 效果概念键
 * @return 记忆级别
 */
MemoryLevel memory_get_causal_rule_level(MemorySystem* memory,
                                        const char* cause_key,
                                        const char* effect_key);

#endif // MEMORY_SYSTEM_H