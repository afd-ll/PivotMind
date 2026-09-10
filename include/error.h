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
// 注意：新增枚举必须追加在末尾，禁止在中间插入——已持久化/对外接口按数值
// 引用旧码（含种子文件、HTTP 响应等），重排会破坏兼容。
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
    ERR_DB_QUERY_FAILED,
    /* P2-3: 子系统通用错误码（v0.5.25 追加，值稳定，勿重排） */
    ERR_IO_ERROR,             /* 文件/网络读写失败 */
    ERR_TIMEOUT,              /* 等待/请求超时 */
    ERR_BUSY,                 /* 资源忙/并发上限 */
    ERR_UNAUTHORIZED,         /* 鉴权失败 */
    ERR_BAD_REQUEST,          /* 请求/参数格式非法 */
    ERR_PARSE_FAILED,         /* 解析失败（JSON/HTTP/报文） */
    ERR_INVALID_STATE,        /* 状态机/生命周期非法 */
    ERR_CHECKSUM_MISMATCH     /* 完整性校验失败 */
} ErrorCode;

// 设置日志级别
void log_set_level(LogLevel level);

// 设置日志输出流（NULL 恢复默认 stderr）；P2-4 新增
void log_set_output(FILE* stream);

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
