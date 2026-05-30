/*
 * heap_check.h - 堆完整性检查宏
 * 在关键位置插入 HEAP_CHECK() 调用，堆损坏时立即报告
 */
#ifndef HEAP_CHECK_H
#define HEAP_CHECK_H

#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

#ifdef DEBUG_HEAP
#define HEAP_CHECK(label) do { \
    int _hc = _heapchk(); \
    if (_hc != _HEAPOK && _hc != _HEAPEMPTY) { \
        fprintf(stderr, "\n[HEAP CORRUPTION] at %s (code=%d)\n", label, _hc); \
        fprintf(stderr, "  File: %s:%d\n", __FILE__, __LINE__); \
        fflush(stderr); \
        abort(); \
    } \
} while(0)
#else
#define HEAP_CHECK(label) ((void)0)
#endif

#endif
