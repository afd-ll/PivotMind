#ifndef COMMON_H
#define COMMON_H

// ========== Standard Libraries ==========
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <time.h>
#include <stdint.h>
#include <assert.h>
#include <stdarg.h>
#include <errno.h>

// ========== Platform Headers ==========
#include "platform.h"

// ========== Constants ==========
#define PI 3.14159265358979323846f
#define EPSILON 1e-10f

// ========== Utility Functions ==========

/**
 * Initialize random number generator (only once)
 * This function uses a static flag to ensure srand() is called only once,
 * preventing repeated initialization that would reduce randomness quality.
 */
static inline void init_random() {
    static bool initialized = false;
    if (!initialized) {
        srand((unsigned int)time(NULL));
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
 * Safe memcpy with NULL checks
 */
static inline void safe_memcpy(void* dest, const void* src, size_t size) {
    if (dest && src && size > 0) {
        memcpy(dest, src, size);
    }
}

/**
 * Safe memset with NULL check
 */
static inline void safe_memset(void* ptr, int value, size_t size) {
    if (ptr && size > 0) {
        memset(ptr, value, size);
    }
}

// ========== Initialization Macros ==========
#define XAVIER_INIT_SCALE 2.0f
#define HE_INIT_SCALE 2.0f
#define RAND_RANGE (2.0f)  // For uniform distribution [-1, 1]

// Xavier initialization scaling factor
static inline float xavier_scale(int fan_in, int fan_out) {
    return sqrtf(2.0f / (fan_in + fan_out));
}

// He initialization scaling factor
static inline float he_scale(int fan_in) {
    return sqrtf(2.0f / fan_in);
}

// Xavier initialization function
static inline float xavier_init(int fan_in, int fan_out) {
    float scale = xavier_scale(fan_in, fan_out);
    return ((float)rand() / RAND_MAX * RAND_RANGE - 1.0f) * scale;
}

// Glorot initialization function (same as Xavier)
static inline float glorot_init(int fan_in, int fan_out) {
    float limit = sqrtf(6.0f / (fan_in + fan_out));
    return ((float)rand() / RAND_MAX * RAND_RANGE - 1.0f) * limit;
}

#endif // COMMON_H
