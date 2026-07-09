#ifndef PLATFORM_H
#define PLATFORM_H

#include <stddef.h>

#ifdef _WIN32
    #include <windows.h>
    #include <psapi.h>
    #define OS_WINDOWS 1
    #define OS_UNIX 0
    #ifdef _MSC_VER
    #pragma comment(lib, "psapi.lib")
    #endif
#else
    #include <dlfcn.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #define OS_WINDOWS 0
    #define OS_UNIX 1
#endif

// Platform-specific library loading
#if OS_WINDOWS
    #define LOAD_LIBRARY(path) LoadLibraryA(path)
    #define GET_PROC_ADDRESS(handle, name) GetProcAddress((HMODULE)handle, name)
    #define FREE_LIBRARY(handle) FreeLibrary((HMODULE)handle)
    typedef HMODULE LibraryHandle;
#else
    #define LOAD_LIBRARY(path) dlopen(path, RTLD_LAZY)
    #define GET_PROC_ADDRESS(handle, name) dlsym(handle, name)
    #define FREE_LIBRARY(handle) dlclose(handle)
    typedef void* LibraryHandle;
#endif

/* ================================================================
 *  平台抽象 — 进程内存使用量
 *  Linux: 读取 /proc/self/status
 *  Windows: GetProcessMemoryInfo
 *  macOS: 可通过 task_info（暂不实现）
 * ================================================================ */

/**
 * 获取当前进程内存使用量
 * @param out_rss_kb  输出：RSS（物理内存），单位 KB（可为 NULL）
 * @param out_vsz_kb  输出：VSZ（虚拟内存），单位 KB（可为 NULL）
 * @return 0 成功，-1 失败
 */
static inline int pm_get_process_memory(long* out_rss_kb, long* out_vsz_kb) {
#if defined(__linux__)
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (out_rss_kb && strncmp(line, "VmRSS:", 6) == 0)
            *out_rss_kb = atol(line + 6);
        if (out_vsz_kb && strncmp(line, "VmSize:", 7) == 0)
            *out_vsz_kb = atol(line + 7);
    }
    fclose(f);
    return 0;
#elif defined(_WIN32)
    HANDLE h = GetCurrentProcess();
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) {
        if (out_rss_kb) *out_rss_kb = (long)(pmc.WorkingSetSize / 1024);
        if (out_vsz_kb) *out_vsz_kb = (long)(pmc.PagefileUsage / 1024);
    }
    return 0;
#else
    // macOS / others: placeholder
    (void)out_rss_kb;
    (void)out_vsz_kb;
    return -1;
#endif
}

/**
 * 获取 RSS（物理内存，MB）
 */
static inline float pm_get_rss_mb(void) {
    long rss_kb = 0;
    pm_get_process_memory(&rss_kb, NULL);
    return (float)rss_kb / 1024.0f;
}

/**
 * 获取系统负载（仅 Linux）
 * @param out_load  输出：1分钟平均负载
 * @return 0 成功，-1 不支持
 */
static inline int pm_get_load(float* out_load) {
#if defined(__linux__)
    FILE* f = fopen("/proc/loadavg", "r");
    if (!f) return -1;
    int matched = fscanf(f, "%f", out_load);
    fclose(f);
    return (matched == 1) ? 0 : -1;
#else
    (void)out_load;
    return -1;
#endif
}

#endif // PLATFORM_H
