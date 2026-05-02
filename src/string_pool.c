
#include "../include/string_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

                        // 字符串池内部结构
                        struct StringPool {
    char** strings;          // 字符串指针数组
    int* ref_counts;         // 引用计数数组
    unsigned int* hash_values;  // 哈希值缓存（加速查找）
    int count;               // 当前字符串数量
    int capacity;            // 当前容量
    size_t total_memory;     // 总内存占用
};

// 简单哈希函数
static unsigned int hash_string(const char* str) {
    unsigned int hash = 5381;
    while (*str) {
        hash = ((hash << 5) + hash) + (unsigned char)*str++;
    }
    return hash;
}

StringPool* string_pool_create(int initial_capacity) {
    StringPool* pool = (StringPool*)malloc(sizeof(StringPool));
    if (!pool) return NULL;
    
    int capacity = (initial_capacity > 0) ? initial_capacity : 1000;
    
    pool->strings = (char**)calloc(capacity, sizeof(char*));
    pool->ref_counts = (int*)calloc(capacity, sizeof(int));
    pool->hash_values = (unsigned int*)calloc(capacity, sizeof(unsigned int));
    
    if (!pool->strings || !pool->ref_counts || !pool->hash_values) {
        free(pool->strings);
        free(pool->ref_counts);
        free(pool->hash_values);
        free(pool);
        return NULL;
    }
    
    pool->count = 0;
    pool->capacity = capacity;
    pool->total_memory = 0;
    
    return pool;
}

void string_pool_destroy(StringPool* pool) {
    if (!pool) return;
    
    for (int i = 0; i < pool->count; i++) {
        if (pool->strings[i]) {
            free(pool->strings[i]);
        }
    }
    
    free(pool->strings);
    free(pool->ref_counts);
    free(pool->hash_values);
    free(pool);
}

const char* string_pool_intern(StringPool* pool, const char* str) {
    if (!pool || !str) return NULL;
    
    // 计算哈希值
    unsigned int hash = hash_string(str);
    size_t len = strlen(str);
    
    // 查找是否已存在
    for (int i = 0; i < pool->count; i++) {
        if (pool->hash_values[i] == hash && 
            strcmp(pool->strings[i], str) == 0) {
            pool->ref_counts[i]++;
            return pool->strings[i];
        }
    }
    
    // 不存在，添加新字符串
    
    // 检查容量，动态扩容
    if (pool->count >= pool->capacity) {
        int new_capacity = pool->capacity * 2;
        
        char** new_strings = (char**)realloc(pool->strings, 
                                             new_capacity * sizeof(char*));
        int* new_refs = (int*)realloc(pool->ref_counts, 
                                      new_capacity * sizeof(int));
        unsigned int* new_hashes = (unsigned int*)realloc(pool->hash_values, 
                                                new_capacity * sizeof(unsigned int));
        
        if (!new_strings || !new_refs || !new_hashes) {
            return NULL; // 扩容失败
        }
        
        pool->strings = new_strings;
        pool->ref_counts = new_refs;
        pool->hash_values = new_hashes;
        pool->capacity = new_capacity;
        
        printf("[字符串池] 扩容: %d -> %d\n", 
               pool->capacity / 2, new_capacity);
    }
    
    // 复制字符串
    char* new_str = strdup(str);
    if (!new_str) return NULL;
    
    pool->strings[pool->count] = new_str;
    pool->ref_counts[pool->count] = 1;
    pool->hash_values[pool->count] = hash;
    pool->total_memory += len + 1;
    
    return pool->strings[pool->count++];
}

void string_pool_retain(StringPool* pool, const char* str) {
    if (!pool || !str) return;
    
    unsigned int hash = hash_string(str);
    
    for (int i = 0; i < pool->count; i++) {
        if (pool->hash_values[i] == hash && 
            strcmp(pool->strings[i], str) == 0) {
            pool->ref_counts[i]++;
            return;
        }
    }
}

void string_pool_release(StringPool* pool, const char* str) {
    if (!pool || !str) return;
    
    unsigned int hash = hash_string(str);
    
    for (int i = 0; i < pool->count; i++) {
        if (pool->hash_values[i] == hash && 
            strcmp(pool->strings[i], str) == 0) {
            
            pool->ref_counts[i]--;
            
            if (pool->ref_counts[i] <= 0) {
                // 引用计数为0，释放字符串
                size_t len = strlen(pool->strings[i]) + 1;
                free(pool->strings[i]);
                pool->total_memory -= len;
                
                // 将最后一个字符串移到当前位置（避免数组空洞）
                if (i < pool->count - 1) {
                    pool->strings[i] = pool->strings[pool->count - 1];
                    pool->ref_counts[i] = pool->ref_counts[pool->count - 1];
                    pool->hash_values[i] = pool->hash_values[pool->count - 1];
                }
                
                pool->count--;
            }
            return;
        }
    }
}

int string_pool_get_ref_count(StringPool* pool, const char* str) {
    if (!pool || !str) return 0;
    
    unsigned int hash = hash_string(str);
    
    for (int i = 0; i < pool->count; i++) {
        if (pool->hash_values[i] == hash && 
            strcmp(pool->strings[i], str) == 0) {
            return pool->ref_counts[i];
        }
    }
    
    return 0;
}

int string_pool_get_count(StringPool* pool) {
    return pool ? pool->count : 0;
}

size_t string_pool_get_memory_usage(StringPool* pool) {
    if (!pool) return 0;
    
    return pool->total_memory + 
           pool->capacity * (sizeof(char*) + sizeof(int) + sizeof(int)) +
           sizeof(StringPool);
}

void string_pool_print_stats(StringPool* pool) {
    if (!pool) {
        printf("[字符串池] 未初始化\n");
        return;
    }
    
    printf("\n===== 字符串池统计 =====\n");
    printf("字符串数量: %d\n", pool->count);
    printf("容量: %d\n", pool->capacity);
    printf("字符串总内存: %.2f KB\n", pool->total_memory / 1024.0);
    printf("元数据内存: %.2f KB\n", 
           (pool->capacity * (sizeof(char*) + sizeof(int) * 2)) / 1024.0);
    printf("总内存: %.2f KB\n", 
           string_pool_get_memory_usage(pool) / 1024.0);
    
    // 显示前10个字符串
    printf("\n前10个字符串:\n");
    for (int i = 0; i < pool->count && i < 10; i++) {
        printf("  [%d] \"%s\" (引用=%d)\n", 
               i, pool->strings[i], pool->ref_counts[i]);
    }
    
    if (pool->count > 10) {
        printf("  ... 还有 %d 个字符串\n", pool->count - 10);
    }
    
    printf("========================\n\n");
}
