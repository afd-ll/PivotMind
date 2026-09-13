#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include "error.h"

// 当前日志级别(默认为INFO)
static LogLevel g_log_level = LOG_INFO;

// P2-4: 日志输出目标，默认 stderr；log_set_output 可重定向到文件
static FILE* g_log_stream = NULL;   // NULL 表示 stderr

// P2-4: 多线程互斥，保证整行日志不被并发写交错
#ifndef _WIN32
#include <pthread.h>
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
#define LOG_LOCK()   pthread_mutex_lock(&g_log_mutex)
#define LOG_UNLOCK() pthread_mutex_unlock(&g_log_mutex)
#else
#define LOG_LOCK()
#define LOG_UNLOCK()
#endif

// 设置日志级别
void log_set_level(LogLevel level) {
    g_log_level = level;
}

// P2-4: 设置日志输出流；stream 为 NULL 时恢复默认 stderr
void log_set_output(FILE* stream) {
    LOG_LOCK();
    g_log_stream = stream;
    LOG_UNLOCK();
}

// 日志输出函数
void log_message(LogLevel level, const char* file, int line, const char* fmt, ...) {
    // 检查是否应该输出此级别的日志
    if (level < g_log_level) {
        return;
    }

    FILE* out = g_log_stream ? g_log_stream : stderr;

    LOG_LOCK();

    // 获取当前时间（localtime_r 线程安全）
    time_t now;
    time(&now);
    struct tm tm_info;
#ifdef _WIN32
    localtime_s(&tm_info, &now);
#else
    localtime_r(&now, &tm_info);
#endif
    char time_buffer[26];
    strftime(time_buffer, 26, "%Y-%m-%d %H:%M:%S", &tm_info);

    // 日志级别字符串
    const char* level_str;
    switch (level) {
        case LOG_DEBUG:    level_str = "DEBUG"; break;
        case LOG_INFO:     level_str = "INFO"; break;
        case LOG_WARNING:  level_str = "WARN"; break;
        case LOG_ERROR:    level_str = "ERROR"; break;
        case LOG_FATAL:    level_str = "FATAL"; break;
        default:           level_str = "UNKNOWN"; break;
    }

    // 提取文件名(去掉路径)
    const char* last_slash = file;
    while (*file) {
        if (*file == '/' || *file == '\\') {
            last_slash = file + 1;
        }
        file++;
    }

    // 输出日志头
    fprintf(out, "[%s] [%s] %s:%d: ", time_buffer, level_str, last_slash, line);

    // 输出格式化消息
    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    // 换行
    fprintf(out, "\n");

    // 关键级别立即刷新，避免进程崩溃时日志滞留缓冲区
    if (level >= LOG_WARNING) {
        fflush(out);
    }

    LOG_UNLOCK();
}

// 获取错误信息字符串
const char* error_string(ErrorCode code) {
    switch (code) {
        case ERR_SUCCESS:                    return "Success";
        case ERR_NULL_POINTER:               return "Null pointer error";
        case ERR_INVALID_ARGUMENT:           return "Invalid argument";
        case ERR_OUT_OF_MEMORY:              return "Out of memory";
        case ERR_INVALID_SHAPE:              return "Invalid tensor shape";
        case ERR_TYPE_MISMATCH:              return "Data type mismatch";
        case ERR_NOT_IMPLEMENTED:            return "Not implemented";
        case ERR_TENSOR_BROADCAST_FAILED:    return "Tensor broadcast failed";
        case ERR_FILE_NOT_FOUND:             return "File not found";
        case ERR_DB_QUERY_FAILED:            return "Database query failed";
        case ERR_IO_ERROR:                   return "I/O error";
        case ERR_TIMEOUT:                    return "Operation timed out";
        case ERR_BUSY:                       return "Resource busy";
        case ERR_UNAUTHORIZED:               return "Unauthorized";
        case ERR_BAD_REQUEST:                return "Bad request";
        case ERR_PARSE_FAILED:               return "Parse failed";
        case ERR_INVALID_STATE:              return "Invalid state";
        case ERR_CHECKSUM_MISMATCH:          return "Checksum mismatch";
        default:                             return "Unknown error";
    }
}
