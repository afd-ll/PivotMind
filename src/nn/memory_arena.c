/**
 * @file memory_arena.c
 * @brief 统一内存分配器实现
 */

#include "memory_arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ==================== 内部常量 ====================

#define DEFAULT_CHUNK_SIZE     (64 * 1024)   // 64KB 块大小
#define DEFAULT_POOL_CAPACITY  128           // 对象池默认容量
#define ALIGN_SIZE             8              // 内存对齐

// ==================== 内存块（区域内部） ====================

typedef struct MemChunk {
    char* data;                // 数据区
    size_t size;               // 块大小
    size_t used;               // 已使用字节
    struct MemChunk* next;      // 下一块
} MemChunk;

// ==================== 内存区域 ====================

struct MemoryArena {
    MemChunk* first_chunk;      // 第一个块
    MemChunk* last_chunk;       // 当前块（快速追加）
    size_t chunk_size;          // 每次扩容大小
    size_t max_size;            // 最大总大小（0=无限制）
    bool track_leaks;           // 追踪泄漏
    bool zero_memory;           // 清零内存

    // 统计
    size_t total_allocated;     // 总分配字节
    size_t total_used;          // 当前使用字节
    size_t peak_usage;          // 峰值使用
    int chunk_count;            // 块数量

    // 泄漏追踪
    MemAllocation* allocs;      // 分配记录链表
    int alloc_count;            // 分配计数
};

// ==================== 工具函数 ====================

static size_t align_size(size_t size) {
    return (size + ALIGN_SIZE - 1) & ~(ALIGN_SIZE - 1);
}

static void default_config(ArenaConfig* cfg) {
    cfg->chunk_size = DEFAULT_CHUNK_SIZE;
    cfg->max_size = 0;
    cfg->track_leaks = false;
    cfg->zero_memory = true;
}

// ==================== 内存区域实现 ====================

MemoryArena* arena_create(const ArenaConfig* cfg) {
    ArenaConfig def;
    if (cfg == NULL) {
        default_config(&def);
        cfg = &def;
    }

    MemoryArena* arena = (MemoryArena*)malloc(sizeof(MemoryArena));
    if (arena == NULL) return NULL;

    memset(arena, 0, sizeof(MemoryArena));
    arena->chunk_size = cfg->chunk_size;
    arena->max_size = cfg->max_size;
    arena->track_leaks = cfg->track_leaks;
    arena->zero_memory = cfg->zero_memory;

    // 预分配第一个块
    MemChunk* first = (MemChunk*)malloc(sizeof(MemChunk) + cfg->chunk_size);
    if (first == NULL) {
        free(arena);
        return NULL;
    }

    first->data = (char*)(first + 1);
    first->size = cfg->chunk_size;
    first->used = 0;
    first->next = NULL;

    arena->first_chunk = first;
    arena->last_chunk = first;
    arena->total_allocated = cfg->chunk_size;

    return arena;
}

void* arena_alloc(MemoryArena* arena, size_t size, MemoryTag tag) {
    if (arena == NULL || size == 0) return NULL;

    size = align_size(size);

    // 尝试在当前块分配
    MemChunk* chunk = arena->last_chunk;
    if (chunk->used + size <= chunk->size) {
        void* ptr = chunk->data + chunk->used;
        chunk->used += size;

        if (arena->zero_memory) {
            memset(ptr, 0, size);
        }

        arena->total_used += size;
        if (arena->total_used > arena->peak_usage) {
            arena->peak_usage = arena->total_used;
        }

        // 泄漏追踪
        if (arena->track_leaks) {
            MemAllocation* alloc = (MemAllocation*)malloc(sizeof(MemAllocation));
            if (alloc) {
                alloc->ptr = ptr;
                alloc->size = size;
                alloc->tag = tag;
                alloc->file = NULL;
                alloc->line = 0;
                alloc->next = arena->allocs;
                arena->allocs = alloc;
                arena->alloc_count++;
            }
        }

        return ptr;
    }

    // 当前块不够，需要新块
    size_t new_size = (size > arena->chunk_size) ? size : arena->chunk_size;
    if (arena->max_size > 0 &&
        arena->total_allocated + new_size > arena->max_size) {
        return NULL;  // 超过最大限制
    }

    MemChunk* new_chunk = (MemChunk*)malloc(sizeof(MemChunk) + new_size);
    if (new_chunk == NULL) return NULL;

    new_chunk->data = (char*)(new_chunk + 1);
    new_chunk->size = new_size;
    new_chunk->used = size;
    new_chunk->next = NULL;

    chunk->next = new_chunk;
    arena->last_chunk = new_chunk;
    arena->total_allocated += new_size;
    arena->chunk_count++;

    void* ptr = new_chunk->data;
    if (arena->zero_memory) {
        memset(ptr, 0, size);
    }

    arena->total_used += size;
    if (arena->total_used > arena->peak_usage) {
        arena->peak_usage = arena->total_used;
    }

    // 泄漏追踪
    if (arena->track_leaks) {
        MemAllocation* alloc = (MemAllocation*)malloc(sizeof(MemAllocation));
        if (alloc) {
            alloc->ptr = ptr;
            alloc->size = size;
            alloc->tag = tag;
            alloc->file = NULL;
            alloc->line = 0;
            alloc->next = arena->allocs;
            arena->allocs = alloc;
            arena->alloc_count++;
        }
    }

    return ptr;
}

char* arena_strdup(MemoryArena* arena, const char* str) {
    if (str == NULL) return NULL;
    size_t len = strlen(str) + 1;
    char* dup = (char*)arena_alloc(arena, len, MEM_TAG_STRING);
    if (dup) memcpy(dup, str, len);
    return dup;
}

void* arena_memdup(MemoryArena* arena, const void* src, size_t size) {
    if (src == NULL || size == 0) return NULL;
    void* dup = arena_alloc(arena, size, MEM_TAG_GENERAL);
    if (dup) memcpy(dup, src, size);
    return dup;
}

void arena_reset(MemoryArena* arena) {
    if (arena == NULL) return;

    // 释放除第一个外的所有块
    MemChunk* chunk = arena->first_chunk->next;
    while (chunk) {
        MemChunk* next = chunk->next;
        free(chunk);
        chunk = next;
    }

    // 重置第一个块
    arena->first_chunk->used = 0;
    arena->last_chunk = arena->first_chunk;
    arena->total_used = 0;
    arena->chunk_count = 0;

    // 清理泄漏追踪
    if (arena->track_leaks) {
        MemAllocation* alloc = arena->allocs;
        while (alloc) {
            MemAllocation* next = alloc->next;
            free(alloc);
            alloc = next;
        }
        arena->allocs = NULL;
        arena->alloc_count = 0;
    }
}

void arena_destroy(MemoryArena* arena) {
    if (arena == NULL) return;

    // 释放所有块
    MemChunk* chunk = arena->first_chunk;
    while (chunk) {
        MemChunk* next = chunk->next;
        free(chunk);
        chunk = next;
    }

    // 释放泄漏追踪链表
    if (arena->track_leaks) {
        MemAllocation* alloc = arena->allocs;
        while (alloc) {
            MemAllocation* next = alloc->next;
            free(alloc);
            alloc = next;
        }
    }

    free(arena);
}

void arena_get_stats(MemoryArena* arena,
                     size_t* total_allocated,
                     size_t* total_used,
                     size_t* peak_usage) {
    if (arena == NULL) return;
    if (total_allocated) *total_allocated = arena->total_allocated;
    if (total_used) *total_used = arena->total_used;
    if (peak_usage) *peak_usage = arena->peak_usage;
}

void arena_set_leak_tracking(MemoryArena* arena, bool enabled) {
    if (arena) arena->track_leaks = enabled;
}

// ==================== 泄漏检测实现 ====================

static const char* tag_name(MemoryTag tag) {
    static const char* names[MEM_TAG_COUNT] = {
        "GENERAL", "TENSOR", "TOPOLOGY_NODE", "TOPOLOGY_EDGE",
        "MEMORY_ENTRY", "CAUSAL_GRAPH", "REPLAY_BUFFER", "STRING",
        "NETWORK_PARAMS"
    };
    if (tag < 0 || tag >= MEM_TAG_COUNT) return "UNKNOWN";
    return names[tag];
}

MemAllocationList* arena_get_leak_report(MemoryArena* arena, int* out_count) {
    if (arena == NULL || out_count == NULL) return NULL;

    MemAllocationList* list = (MemAllocationList*)malloc(sizeof(MemAllocationList));
    if (list == NULL) return NULL;

    list->head = arena->allocs;
    list->count = arena->alloc_count;
    *out_count = arena->alloc_count;

    return list;
}

void arena_print_leaks(MemoryArena* arena) {
    if (arena == NULL) return;

    fprintf(stderr, "\n=== Memory Leak Report ===\n");
    fprintf(stderr, "Total leaks: %d\n", arena->alloc_count);
    fprintf(stderr, "Total unfreed: %zu bytes\n\n", arena->total_used);

    MemAllocation* alloc = arena->allocs;
    while (alloc) {
        fprintf(stderr, "  [%s] %p (%zu bytes)",
                tag_name(alloc->tag), alloc->ptr, alloc->size);
        if (alloc->file) {
            fprintf(stderr, " at %s:%d", alloc->file, alloc->line);
        }
        fprintf(stderr, "\n");
        alloc = alloc->next;
    }
    fprintf(stderr, "=========================\n\n");
}

void arena_get_usage_by_tag(MemoryArena* arena, size_t usage[MEM_TAG_COUNT]) {
    if (arena == NULL || usage == NULL) return;

    memset(usage, 0, sizeof(size_t) * MEM_TAG_COUNT);

    MemAllocation* alloc = arena->allocs;
    while (alloc) {
        usage[alloc->tag] += alloc->size;
        alloc = alloc->next;
    }
}

void arena_print_global_report(void) {
    fprintf(stderr, "\n=== Global Memory Report ===\n");
    fprintf(stderr, "Tag usage breakdown:\n");

    // 全局区域需要单独统计，这里提供占位
    fprintf(stderr, "  (call arena_print_leaks on each arena for details)\n");
    fprintf(stderr, "============================\n\n");
}

// ==================== 对象池实现 ====================

struct ObjectPool {
    size_t object_size;         // 对象大小
    int total_capacity;         // 总容量
    int used_count;             // 已使用数量
    void** free_list;           // 空闲对象列表
    int free_count;             // 空闲对象数量
};

ObjectPool* object_pool_create(size_t object_size, int initial_capacity) {
    // 对齐对象大小
    size_t aligned_size = align_size(object_size);

    ObjectPool* pool = (ObjectPool*)malloc(sizeof(ObjectPool));
    if (pool == NULL) return NULL;

    memset(pool, 0, sizeof(ObjectPool));
    pool->object_size = aligned_size;
    pool->total_capacity = initial_capacity > 0 ? initial_capacity : DEFAULT_POOL_CAPACITY;
    pool->free_list = (void**)malloc(sizeof(void*) * pool->total_capacity);
    if (pool->free_list == NULL) {
        free(pool);
        return NULL;
    }

    // 预分配对象
    for (int i = 0; i < pool->total_capacity; i++) {
        pool->free_list[i] = malloc(aligned_size);
        if (pool->free_list[i] == NULL) {
            // 分配失败，清理并返回部分初始化的池
            for (int j = 0; j < i; j++) {
                free(pool->free_list[j]);
            }
            pool->total_capacity = i;
            break;
        }
    }
    pool->free_count = pool->total_capacity;

    return pool;
}

void* object_pool_acquire(ObjectPool* pool) {
    if (pool == NULL) return NULL;

    // 池中有空闲对象
    if (pool->free_count > 0) {
        pool->free_count--;
        pool->used_count++;
        return pool->free_list[pool->free_count];
    }

    // 池空，扩容
    int new_capacity = pool->total_capacity * 2;
    void** new_list = (void**)realloc(pool->free_list, sizeof(void*) * new_capacity);
    if (new_list == NULL) return NULL;

    pool->free_list = new_list;

    // 分配新对象
    for (int i = pool->total_capacity; i < new_capacity; i++) {
        pool->free_list[i] = malloc(pool->object_size);
        if (pool->free_list[i] == NULL) {
            pool->total_capacity = i;
            break;
        }
    }

    pool->total_capacity = new_capacity;
    pool->free_count = new_capacity - pool->total_capacity;
    pool->used_count++;

    // 取最后一个
    return pool->free_list[--pool->free_count];
}

void object_pool_release(ObjectPool* pool, void* obj) {
    if (pool == NULL || obj == NULL) return;

    // 放回空闲列表
    if (pool->free_count >= pool->total_capacity) {
        // 理论上不应发生，因为 acquire 会扩容
        free(obj);
        return;
    }

    pool->free_list[pool->free_count++] = obj;
    pool->used_count--;
}

void object_pool_destroy(ObjectPool* pool) {
    if (pool == NULL) return;

    // 释放所有对象
    for (int i = 0; i < pool->free_count; i++) {
        free(pool->free_list[i]);
    }
    free(pool->free_list);
    free(pool);
}

void object_pool_get_stats(ObjectPool* pool, int* total, int* used) {
    if (pool == NULL) return;
    if (total) *total = pool->total_capacity;
    if (used) *used = pool->used_count;
}

// ==================== 全局分配器 ====================

static MemoryArena* g_global_arena = NULL;
static MemoryArena* g_tensor_arena = NULL;
static MemoryArena* g_topology_arena = NULL;
static bool g_initialized = false;

MemoryArena* arena_get_global(void) {
    if (!g_initialized) arena_init_all();
    return g_global_arena;
}

MemoryArena* arena_get_tensor(void) {
    if (!g_initialized) arena_init_all();
    return g_tensor_arena;
}

MemoryArena* arena_get_topology(void) {
    if (!g_initialized) arena_init_all();
    return g_topology_arena;
}

void arena_init_all(void) {
    if (g_initialized) return;

    ArenaConfig cfg;
    default_config(&cfg);
    cfg.chunk_size = 256 * 1024;  // 256KB

    g_global_arena = arena_create(&cfg);
    cfg.track_leaks = true;
    g_tensor_arena = arena_create(&cfg);
    g_topology_arena = arena_create(&cfg);

    g_initialized = true;
}

void arena_shutdown_all(void) {
    if (!g_initialized) return;

    // 打印泄漏报告
    if (g_global_arena && g_global_arena->alloc_count > 0) {
        fprintf(stderr, "[memory_arena] global arena: ");
        arena_print_leaks(g_global_arena);
    }
    if (g_tensor_arena && g_tensor_arena->alloc_count > 0) {
        fprintf(stderr, "[memory_arena] tensor arena: ");
        arena_print_leaks(g_tensor_arena);
    }
    if (g_topology_arena && g_topology_arena->alloc_count > 0) {
        fprintf(stderr, "[memory_arena] topology arena: ");
        arena_print_leaks(g_topology_arena);
    }

    arena_destroy(g_global_arena);
    arena_destroy(g_tensor_arena);
    arena_destroy(g_topology_arena);

    g_global_arena = NULL;
    g_tensor_arena = NULL;
    g_topology_arena = NULL;
    g_initialized = false;
}

// ==================== 统一 malloc/free ====================

void* unified_malloc(size_t size, MemoryTag tag) {
    void* ptr = arena_alloc(arena_get_global(), size, tag);
    if (ptr == NULL) {
        ptr = malloc(size);  // 降级到系统 malloc
    }
    return ptr;
}

void unified_free(void* ptr, MemoryTag tag) {
    (void)tag;
    free(ptr);  // 统一分配器暂不支持精确释放，直接 free
}

void* unified_calloc(size_t nmemb, size_t size, MemoryTag tag) {
    size_t total = nmemb * size;
    void* ptr = arena_alloc(arena_get_global(), total, tag);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void* unified_realloc(void* ptr, size_t size, MemoryTag tag) {
    (void)tag;
    return realloc(ptr, size);  // 暂不支持精确追踪
}