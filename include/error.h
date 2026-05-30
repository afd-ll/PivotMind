#ifndef ERROR_H
#define ERROR_H

#include <stdio.h>
#include <stdbool.h>

// 错误级别
typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR,
    LOG_FATAL
} LogLevel;

// 错误代码
typedef enum {
    ERR_SUCCESS = 0,
    ERR_NULL_POINTER,
    ERR_INVALID_ARGUMENT,
    ERR_OUT_OF_MEMORY,
    ERR_INVALID_SHAPE,
    ERR_TYPE_MISMATCH,
    ERR_NOT_IMPLEMENTED,
    ERR_TENSOR_BROADCAST_FAILED,
    ERR_FILE_NOT_FOUND,
    ERR_DB_QUERY_FAILED
} ErrorCode;

// 设置日志级别
void log_set_level(LogLevel level);

// 日志输出函数
void log_message(LogLevel level, const char* file, int line, const char* fmt, ...);

// 错误检查宏
#define CHECK_NULL(ptr) \
    do { \
        if (!(ptr)) { \
            log_message(LOG_ERROR, __FILE__, __LINE__, "Null pointer: %s", #ptr); \
            return NULL; \
        } \
    } while(0)

#define CHECK_NULL_RETURN(ptr, ret) \
    do { \
        if (!(ptr)) { \
            log_message(LOG_ERROR, __FILE__, __LINE__, "Null pointer: %s", #ptr); \
            return ret; \
        } \
    } while(0)

#define CHECK_BOOL(expr) \
    do { \
        if (!(expr)) { \
            log_message(LOG_ERROR, __FILE__, __LINE__, "Condition failed: %s", #expr); \
            return false; \
        } \
    } while(0)

#define CHECK_BOOL_RETURN(expr, ret) \
    do { \
        if (!(expr)) { \
            log_message(LOG_ERROR, __FILE__, __LINE__, "Condition failed: %s", #expr); \
            return ret; \
        } \
    } while(0)

// 便捷日志宏
#define LOG_DEBUG(fmt, ...) log_message(LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) log_message(LOG_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARNING(fmt, ...) log_message(LOG_WARNING, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_message(LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...) log_message(LOG_FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

// 获取错误信息字符串
const char* error_string(ErrorCode code);

#endif // ERROR_H
