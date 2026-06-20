/**
 * @file web_search.h
 * @brief 自实现搜索引擎 — 零依赖 HTTP 客户端 + HTML 解析
 *
 * 跨平台实现：
 *   - Linux: BSD sockets
 *   - Windows: Winsock2
 * 无外部库依赖，纯 C + OS socket API。
 */

#ifndef WEB_SEARCH_H
#define WEB_SEARCH_H

#ifdef __cplusplus
extern "C" {
#endif

/** 搜索结果 */
typedef struct {
    char*   url;           /* 最终 URL（可能经过重定向） */
    int     status_code;   /* HTTP 状态码 */
    char*   body;          /* 响应正文（UTF-8） */
    int     body_len;      /* 正文长度 */
    char*   title;         /* <title> 标签内容 */
    char**  keywords;      /* 提取的关键词 */
    int     keyword_count; /* 关键词数量 */
    char*   content_type;  /* Content-Type 响应头 (如 "text/html; charset=utf-8") */
} WebResult;

/**
 * 发起 HTTP GET 请求并解析 HTML
 *
 * @param url       目标 URL
 * @param timeout_ms 超时（毫秒）
 * @param max_body   最大下载字节数
 * @return          搜索结果，调用方负责 web_result_free()
 */
WebResult* web_search(const char* url, int timeout_ms, int max_body);

/** 释放搜索结果 */
void web_result_free(WebResult* r);

/**
 * 从 HTML 文本中提取可见文本（增强版：结构化分层提取）
 * @param html   HTML 字符串
 * @param out    输出缓冲区（调用方分配）
 * @param out_sz 缓冲区大小
 * @return       写入的字节数
 */
int web_extract_text(const char* html, char* out, int out_sz);

/**
 * 从 HTML 中提取 &lt;meta name="description"&gt; 内容
 * @return 写入的字节数，0=未找到
 */
int web_extract_meta_description(const char* html, char* out, int out_sz);

/**
 * 从 HTML 中提取所有 &lt;h1&gt;/&lt;h2&gt; 标题文本
 * @return 写入的字节数
 */
int web_extract_headings(const char* html, char* out, int out_sz);

/**
 * 从 HTML 中提取前 N 个 &lt;p&gt; 段落文本
 * @param max_paragraphs  最多提取段落数
 * @return 写入的字节数
 */
int web_extract_paragraphs(const char* html, char* out, int out_sz, int max_paragraphs);

#ifdef __cplusplus
}
#endif

#endif /* WEB_SEARCH_H */
