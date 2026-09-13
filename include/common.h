#ifndef COMMON_H
#define COMMON_H

// ========== Standard Libraries ==========
// 仅保留 common.h 自身 inline 函数需要的头文件
// 各 .c 文件应显式 include 自己需要的标准库
#include <stdlib.h>   // srand, size_t
#include <string.h>   // memcpy, memset
#include <math.h>     // sqrtf
#include <stdbool.h>  // bool
#include <time.h>     // time

// ========== Platform Headers ==========
#include "platform.h"

/* 全局常量集中管理 */
#include "constants.h"

// ========== Constants ==========
#define PI 3.14159265358979323846f
#define EPSILON 1e-10f
#define NODE_FEATURE_DIM PM_NODE_FEATURE_DIM  // 向后兼容别名

// ========== Utility Functions ==========

/** 安全 strcmp：任一参数 NULL 返回非 0（视为不相等），避免野指针崩溃 */
static inline int strcmp_null(const char* a, const char* b) {
    if (!a || !b) return (a == b) ? 0 : 1;
    return strcmp(a, b);
}

/** 向量的余弦相似度，任一指针为NULL或dim<=0返回0 */
static inline float cosine_similarity(const float* a, const float* b, int dim) {
    if (!a || !b || dim <= 0) return 0.0f;
    float dot = 0, na = 0, nb = 0;
    for (int i = 0; i < dim; i++) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    float norm = sqrtf(na) * sqrtf(nb);
    return (norm < 1e-10f) ? 0.0f : dot / norm;
}

/** Hebbian 更新: 将两个向量互相拉近，任一为NULL则跳过 */
static inline void hebbian_update(float* a, float* b, int dim, float lr) {
    if (!a || !b || dim <= 0) return;
    for (int i = 0; i < dim; i++) {
        float diff = b[i] - a[i];
        a[i] += lr * diff;
        b[i] -= lr * diff;  /* 对称更新 */
    }
}

/** 上下文敏感 Hebbian 更新: 双向拉近的同时，两个向量都向上下文均值微移。
 *  这使得同一个概念在不同上下文中逐渐分化为不同的语义倾向。
 *  @param context_mean     路径上下文的特征向量均值（NULL=退化为普通 hebbian）
 *  @param context_strength 上下文引力强度，建议 0.1~0.2 */
static inline void hebbian_update_contextual(float* a, float* b, int dim,
                                              float lr,
                                              const float* context_mean,
                                              float context_strength) {
    if (!a || !b || dim <= 0) return;
    for (int i = 0; i < dim; i++) {
        float diff = b[i] - a[i];
        a[i] += lr * diff;
        b[i] -= lr * diff;
        if (context_mean) {
            a[i] += lr * context_strength * (context_mean[i] - a[i]);
            b[i] += lr * context_strength * (context_mean[i] - b[i]);
        }
    }
}

/**
 * 显式随机种子标志（唯一定义在 src/random_seed.c，全进程共享）。
 *
 * 为什么必须是「跨 TU 共享的 extern」而不是本头文件里的 static：
 *   common.h 的 inline 函数在每个翻译单元各有一份 static，而 srand() 的种子是
 *   libc 的进程级全局状态。标志若按 TU 各存一份，A 里 init_random_seed() 置了位，
 *   B 里的 init_random() 看不见，仍会 srand(time^pid) 把显式种子冲掉 —— 那样
 *   PIVOTMIND_SEED / init_random_seed() 就还是「装了开关没通电」。
 */
extern bool pm_random_seed_explicit;

/**
 * Initialize random number generator (only once)
 * 已显式设种（init_random_seed / init_random_from_env / $PIVOTMIND_SEED）时
 * **不再用 time/pid 覆盖** —— 否则固定种子会被冲掉。未显式设种时行为与旧实现
 * 逐字节一致（仍在此处 srand(time(NULL) ^ (size_t)&initialized)）。
 */
static inline void init_random() {
    static bool initialized = false;
    if (!initialized) {
        if (!pm_random_seed_explicit) {
            srand((unsigned int)(time(NULL) ^ (size_t)&initialized));
        }
        initialized = true;
    }
}

/**
 * 显式固定种子（可复现实验入口）。置位 pm_random_seed_explicit ⇒ 之后的
 * init_random() 不再覆盖它。
 */
static inline void init_random_seed(unsigned int seed) {
    srand(seed);
    pm_random_seed_explicit = true;
}

/**
 * 从环境变量 PIVOTMIND_SEED 读取固定种子（已设置且非空时生效）。
 * 置位 pm_random_seed_explicit ⇒ 之后的 init_random() 不再覆盖它。
 */
static inline void init_random_from_env(void) {
    const char* env = getenv("PIVOTMIND_SEED");
    if (env && *env) {
        srand((unsigned int)strtoul(env, NULL, 10));
        pm_random_seed_explicit = true;
    }
}

/**
 * Maximum of two floats
 */
static inline float max_f(float a, float b) {
    return a > b ? a : b;
}

/**
 * Minimum of two floats
 */
static inline float min_f(float a, float b) {
    return a < b ? a : b;
}

/**
 * Maximum of two size_t values
 */
static inline size_t max_z(size_t a, size_t b) {
    return a > b ? a : b;
}

/**
 * Clamp value to range [min_val, max_val]
 */
static inline float clamp(float value, float min_val, float max_val) {
    if (value < min_val) return min_val;
    if (value > max_val) return max_val;
    return value;
}

/**
 * Safe memcpy with NULL checks. Returns 0 on success, -1 on failure.
 */
static inline int safe_memcpy(void* dest, const void* src, size_t size) {
    if (!dest || !src || size == 0) return -1;
    memcpy(dest, src, size);
    return 0;
}

/**
 * Safe memset with NULL check. Returns 0 on success, -1 on failure.
 */
static inline int safe_memset(void* ptr, int value, size_t size) {
    if (!ptr || size == 0) return -1;
    memset(ptr, value, size);
    return 0;
}

#endif // COMMON_H
