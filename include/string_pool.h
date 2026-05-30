#ifndef STRING_POOL_H
#define STRING_POOL_H

#include <stddef.h>

/**
 * 字符串池 - 共享字符串存储，节省内存
 * 
 * 优化点：
 * - 相同字符串只存储一次
 * - 使用引用计数管理生命周期
 * - 自动扩容
 */

typedef struct StringPool StringPool;

// 创建字符串池
StringPool* string_pool_create(int initial_capacity);

// 销毁字符串池
void string_pool_destroy(StringPool* pool);

// 获取或创建字符串（自动去重）
const char* string_pool_intern(StringPool* pool, const char* str);

// 增加字符串引用计数
void string_pool_retain(StringPool* pool, const char* str);

// 减少字符串引用计数（引用为0时释放）
void string_pool_release(StringPool* pool, const char* str);

// 获取字符串引用计数
int string_pool_get_ref_count(StringPool* pool, const char* str);

// 获取池中字符串数量
int string_pool_get_count(StringPool* pool);

// 获取池中总内存占用（字节）
size_t string_pool_get_memory_usage(StringPool* pool);

// 打印池状态（调试用）
void string_pool_print_stats(StringPool* pool);

#endif // STRING_POOL_H
