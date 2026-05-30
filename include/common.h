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

/** 向量的余弦相似度 */
static inline float cosine_similarity(const float* a, const float* b, int dim) {
    float dot = 0, na = 0, nb = 0;
    for (int i = 0; i < dim; i++) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    float norm = sqrtf(na) * sqrtf(nb);
    return (norm < 1e-10f) ? 0.0f : dot / norm;
}

/** Hebbian 更新: 将两个向量互相拉近 */
static inline void hebbian_update(float* a, float* b, int dim, float lr) {
    for (int i = 0; i < dim; i++) {
        float diff = b[i] - a[i];
        a[i] += lr * diff;
        b[i] -= lr * diff;  // 对称更新
    }
}

/**
 * Initialize random number generator (only once)
 * This function uses a static flag to ensure srand() is called only once,
 * preventing repeated initialization that would reduce randomness quality.
 */
static inline void init_random() {
    static bool initialized = false;
    if (!initialized) {
        srand((unsigned int)(time(NULL) ^ (size_t)&initialized));
        initialized = true;
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
