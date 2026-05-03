#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include "../include/error.h"

// 当前日志级别(默认为INFO)
static LogLevel g_log_level = LOG_INFO;

// 设置日志级别
void log_set_level(LogLevel level) {
    g_log_level = level;
}

// 日志输出函数
void log_message(LogLevel level, const char* file, int line, const char* fmt, ...) {
    // 检查是否应该输出此级别的日志
    if (level < g_log_level) {
        return;
    }

    // 获取当前时间
    time_t now;
    time(&now);
    struct tm* tm_info = localtime(&now);
    char time_buffer[26];
    strftime(time_buffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

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
    const char* filename = file;
    const char* last_slash = file;
    while (*file) {
        if (*file == '/' || *file == '\\') {
            last_slash = file + 1;
        }
        file++;
    }
    filename = last_slash;

    // 输出日志头
    fprintf(stderr, "[%s] [%s] %s:%d: ", time_buffer, level_str, filename, line);

    // 输出格式化消息
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    // 换行
    fprintf(stderr, "\n");

    // 如果是致命错误,刷新缓冲区
    if (level == LOG_FATAL) {
        fflush(stderr);
    }
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
        default:                             return "Unknown error";
    }
}
