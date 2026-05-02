/**
 * @file memory_arena.h
 * @brief 统一内存分配器 - 提供区域式内存管理和对象池
 *
 * 设计目标:
 * 1. 减少内存碎片 - 区域式分配，整块释放
 * 2. 提高分配效率 - 对象池预分配，避免频繁 malloc
 * 3. 统一管理 - 贯穿所有模块的内存生命周期
 * 4. 调试友好 - 内存泄漏检测、统计信息
 */

#ifndef MEMORY_ARENA_H
#define MEMORY_ARENA_H

#include <stddef.h>
#include <stdbool.h>

// ==================== 内存块标记（用于泄漏检测） ====================

/**
 * 内存分配标记
 */
typedef enum {
    MEM_TAG_GENERAL = 0,      // 通用分配
    MEM_TAG_TENSOR,           // 张量数据
    MEM_TAG_TOPOLOGY_NODE,    // 拓扑节点
    MEM_TAG_TOPOLOGY_EDGE,    // 拓扑边
    MEM_TAG_MEMORY_ENTRY,     // 记忆条目
    MEM_TAG_CAUSAL_GRAPH,     // 因果图
    MEM_TAG_REPLAY_BUFFER,    // 回放缓冲区
    MEM_TAG_STRING,           // 字符串数据
    MEM_TAG_NETWORK_PARAMS,   // 网络参数
    MEM_TAG_COUNT             // 标记类型总数
} MemoryTag;

/**
 * 内存分配记录（用于泄漏追踪）
 */
typedef struct MemAllocation {
    void* ptr;                // 分配的指针
    size_t size;              // 分配大小
    MemoryTag tag;             // 分配标记
    const char* file;          // 来源文件
    int line;                 // 来源行号
    struct MemAllocation* next;
} MemAllocation;

/**
 * 内存分配链表头
 */
typedef struct MemAllocationList {
    MemAllocation* head;
    int count;
} MemAllocationList;

// ==================== 对象池 ====================

/**
 * 对象池 - 用于固定大小对象的快速分配
 */
typedef struct ObjectPool ObjectPool;

/**
 * 创建对象池
 * @param object_size 对象大小
 * @param initial_capacity 初始容量
 * @return 对象池，NULL 失败
 */
ObjectPool* object_pool_create(size_t object_size, int initial_capacity);

/**
 * 从池中获取对象
 * @param pool 对象池
 * @return 对象指针（已构造）
 */
void* object_pool_acquire(ObjectPool* pool);

/**
 * 归还对象到池
 * @param pool 对象池
 * @param obj 对象指针
 */
void object_pool_release(ObjectPool* pool, void* obj);

/**
 * 销毁对象池
 * @param pool 对象池
 */
void object_pool_destroy(ObjectPool* pool);

/**
 * 获取池统计信息
 * @param pool 对象池
 * @param total 总容量
 * @param used 已使用数量
 */
void object_pool_get_stats(ObjectPool* pool, int* total, int* used);

// ==================== 内存区域 ====================

/**
 * 内存区域配置
 */
typedef struct ArenaConfig {
    size_t chunk_size;         // 每次扩容块大小
    size_t max_size;           // 最大总大小（0=无限制）
    bool track_leaks;          // 是否追踪泄漏
    bool zero_memory;          // 是否清零内存
} ArenaConfig;

/**
 * 内存区域 - 区域式分配器
 */
typedef struct MemoryArena MemoryArena;

/**
 * 创建内存区域
 * @param config 区域配置（NULL 使用默认）
 * @return 内存区域，NULL 失败
 */
MemoryArena* arena_create(const ArenaConfig* config);

/**
 * 从区域分配内存
 * @param arena 内存区域
 * @param size 分配大小
 * @param tag 分配标记
 * @return 分配指针，NULL 失败
 */
void* arena_alloc(MemoryArena* arena, size_t size, MemoryTag tag);

/**
 * 分配并复制字符串（自动包含终止符）
 * @param arena 内存区域
 * @param str 源字符串
 * @return 分配的字符串副本
 */
char* arena_strdup(MemoryArena* arena, const char* str);

/**
 * 分配并复制内存块
 * @param arena 内存区域
 * @param src 源数据
 * @param size 复制大小
 * @return 分配并复制的内存
 */
void* arena_memdup(MemoryArena* arena, const void* src, size_t size);

/**
 * 重置区域（释放所有分配，保留区域）
 * @param arena 内存区域
 */
void arena_reset(MemoryArena* arena);

/**
 * 销毁内存区域
 * @param arena 内存区域
 */
void arena_destroy(MemoryArena* arena);

/**
 * 获取区域统计
 * @param arena 内存区域
 * @param total_allocated 总分配字节
 * @param total_used 当前使用字节
 * @param peak_usage 峰值使用
 */
void arena_get_stats(MemoryArena* arena,
                     size_t* total_allocated,
                     size_t* total_used,
                     size_t* peak_usage);

// ==================== 全局默认分配器 ====================

/**
 * 获取全局内存区域（用于一般分配）
 * @return 全局内存区域
 */
MemoryArena* arena_get_global(void);

/**
 * 获取张量专用区域
 * @return 张量内存区域
 */
MemoryArena* arena_get_tensor(void);

/**
 * 获取拓扑网络专用区域
 * @return 拓扑内存区域
 */
MemoryArena* arena_get_topology(void);

/**
 * 初始化所有全局分配器（应在 main 开始时调用）
 */
void arena_init_all(void);

/**
 * 销毁所有全局分配器（应在 main 结束时调用）
 */
void arena_shutdown_all(void);

// ==================== 泄漏检测 ====================

/**
 * 启用/禁用泄漏追踪
 * @param arena 内存区域
 * @param enabled 是否启用
 */
void arena_set_leak_tracking(MemoryArena* arena, bool enabled);

/**
 * 获取未释放的分配列表
 * @param arena 内存区域
 * @param out_count 输出分配数量
 * @return 分配列表（ caller 负责释放 ）
 */
MemAllocationList* arena_get_leak_report(MemoryArena* arena, int* out_count);

/**
 * 打印泄漏报告到 stderr
 * @param arena 内存区域
 */
void arena_print_leaks(MemoryArena* arena);

/**
 * 按标记类型统计内存使用
 * @param arena 内存区域
 * @param usage 输出数组 [MEM_TAG_COUNT]，需预先分配
 */
void arena_get_usage_by_tag(MemoryArena* arena, size_t usage[MEM_TAG_COUNT]);

/**
 * 打印全局内存使用报告
 */
void arena_print_global_report(void);

// ==================== 便捷宏 ====================

/**
 * 在指定区域分配内存（带文件/行号追踪）
 */
#define ARENA_ALLOC(arena, size) \
    arena_alloc((arena), (size), MEM_TAG_GENERAL)

/**
 * 在全局区域分配内存
 */
#define ARENA_ALLOC_GLOBAL(size) \
    arena_alloc(arena_get_global(), (size), MEM_TAG_GENERAL)

/**
 * 复制字符串到全局区域
 */
#define ARENA_STRDUP(str) \
    arena_strdup(arena_get_global(), (str))

/**
 * 对象池获取/归还便捷宏
 */
#define POOL_ACQUIRE(pool, type) \
    ((type*)object_pool_acquire(pool))

#define POOL_RELEASE(pool, obj) \
    object_pool_release((pool), (obj))

// ==================== 统一入口（替换 malloc/free） ====================

/**
 * 统一内存分配（全局区域）
 * 用途：逐步替换代码中的 malloc/free 调用
 */
void* unified_malloc(size_t size, MemoryTag tag);

/**
 * 统一内存释放
 */
void unified_free(void* ptr, MemoryTag tag);

/**
 * 统一分配并清零
 */
void* unified_calloc(size_t nmemb, size_t size, MemoryTag tag);

/**
 * 统一重新分配
 */
void* unified_realloc(void* ptr, size_t size, MemoryTag tag);

#endif // MEMORY_ARENA_H
