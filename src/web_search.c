/**
 * @file web_search.c
 * @brief 自实现搜索引擎 — 零依赖 HTTP + HTML 解析
 *
 * HTTP/1.0 GET → 跟随重定向 → HTML→文本提取 → UTF-8关键词抽取
 */

#include "web_search.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  #define socklen_t int
  #define close_socket(s) closesocket(s)
  static int socket_init(void) {
      WSADATA wsa;
      return WSAStartup(MAKEWORD(2,2), &wsa) == 0;
  }
  static void socket_cleanup(void) { WSACleanup(); }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <errno.h>
  #define close_socket(s) close(s)
  static int socket_init(void) { return 1; }
  static void socket_cleanup(void) {}
#endif

/* ================================================================
 *  URL 解析
 * ================================================================ */

static int parse_url(const char* url, char* host, int host_sz,
                     int* port, char* path, int path_sz) {
    const char* p = url;
    /* 跳过 scheme */
    if (strncmp(p, "http://", 7) == 0) { p += 7; *port = 80; }
    else if (strncmp(p, "https://", 8) == 0) { p += 8; *port = 443; }
    else return -1;

    /* 提取 host */
    int hi = 0;
    while (*p && *p != '/' && *p != ':' && hi < host_sz - 1)
        host[hi++] = *p++;
    host[hi] = '\0';

    /* 端口 */
    if (*p == ':') {
        p++;
        *port = 0;
        while (isdigit((unsigned char)*p))
            *port = *port * 10 + (*p++ - '0');
    }

    /* 路径 */
    if (*p) {
        int pi = 0;
        while (*p && pi < path_sz - 1)
            path[pi++] = *p++;
        path[pi] = '\0';
    } else {
        path[0] = '/'; path[1] = '\0';
    }
    return 0;
}

/* ================================================================
 *  DNS 解析
 * ================================================================ */

static int resolve_host(const char* host, struct sockaddr_in* addr) {
    struct hostent* he = gethostbyname(host);
    if (!he) return -1;
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = 0;
    memcpy(&addr->sin_addr, he->h_addr_list[0], he->h_length);
    return 0;
}

/* ================================================================
 *  HTTP GET
 * ================================================================ */

#define HTTP_BUF_SIZE 4096
#define MAX_REDIRECTS 5

static char* http_get_raw(const char* host, int port, const char* path,
                          int timeout_ms, int max_body, int* out_len) {
    if (!socket_init()) return NULL;

    struct sockaddr_in addr;
    if (resolve_host(host, &addr) != 0) {
        socket_cleanup();
        return NULL;
    }
    addr.sin_port = htons((unsigned short)port);

    int sock = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { socket_cleanup(); return NULL; }

    /* 超时 */
#ifdef _WIN32
    int to = timeout_ms;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&to, sizeof(to));
#else
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close_socket(sock); socket_cleanup(); return NULL;
    }

    /* 构造 HTTP/1.0 请求 */
    char req[2048];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: PivotMind/0.1 (SelfLearner)\r\n"
        "Accept: text/html,text/plain\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host);

    if (send(sock, req, req_len, 0) < 0) {
        close_socket(sock); socket_cleanup(); return NULL;
    }

    /* 接收响应 */
    char* body = (char*)malloc(max_body + 1);
    if (!body) { close_socket(sock); socket_cleanup(); return NULL; }

    int total = 0;
    char buf[HTTP_BUF_SIZE];
    int header_done = 0;

    while (total < max_body) {
        int n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;

        if (!header_done) {
            /* 找 \r\n\r\n 分隔头部和正文 */
            for (int i = 0; i < n - 3; i++) {
                if (buf[i] == '\r' && buf[i+1] == '\n' &&
                    buf[i+2] == '\r' && buf[i+3] == '\n') {
                    int body_start = i + 4;
                    int to_copy = n - body_start;
                    if (total + to_copy > max_body) to_copy = max_body - total;
                    memcpy(body + total, buf + body_start, to_copy);
                    total += to_copy;
                    header_done = 1;
                    break;
                }
            }
            if (!header_done) continue;  /* 头部未完成，继续接收 */
        } else {
            if (total + n > max_body) n = max_body - total;
            memcpy(body + total, buf, n);
            total += n;
        }
    }

    close_socket(sock);
    socket_cleanup();

    body[total] = '\0';
    *out_len = total;
    return body;
}

/* ================================================================
 *  HTML → 文本提取
 * ================================================================ */

int web_extract_text(const char* html, char* out, int out_sz) {
    if (!html || !out || out_sz <= 0) return 0;

    int pos = 0;
    int in_tag = 0;
    int in_script = 0;
    int in_style = 0;
    int last_was_space = 1;

    for (const char* p = html; *p && pos < out_sz - 1; p++) {
        if (*p == '<') {
            in_tag = 1;
            if (strncasecmp(p+1, "script", 6) == 0) in_script = 1;
            if (strncasecmp(p+1, "/script", 7) == 0) in_script = 0;
            if (strncasecmp(p+1, "style", 5) == 0) in_style = 1;
            if (strncasecmp(p+1, "/style", 6) == 0) in_style = 0;
            continue;
        }
        if (*p == '>') { in_tag = 0; continue; }
        if (in_tag || in_script || in_style) continue;

        /* HTML 实体解码 */
        if (*p == '&') {
            if (strncmp(p, "&amp;", 5) == 0)  { out[pos++] = '&';  p += 4; continue; }
            if (strncmp(p, "&lt;", 4) == 0)   { out[pos++] = '<';  p += 3; continue; }
            if (strncmp(p, "&gt;", 4) == 0)   { out[pos++] = '>';  p += 3; continue; }
            if (strncmp(p, "&quot;", 6) == 0) { out[pos++] = '"';  p += 5; continue; }
            if (strncmp(p, "&nbsp;", 6) == 0) { out[pos++] = ' ';  p += 5; continue; }
            /* &#xXXXX; 数字实体 → 跳过 */
            if (p[1] == '#') { while (*p && *p != ';') p++; continue; }
        }

        /* 空白折叠 */
        if (isspace((unsigned char)*p)) {
            if (!last_was_space) { out[pos++] = ' '; last_was_space = 1; }
            continue;
        }

        out[pos++] = *p;
        last_was_space = 0;
    }

    out[pos] = '\0';
    return pos;
}

/* ================================================================
 *  主搜索 API
 * ================================================================ */

WebResult* web_search(const char* url, int timeout_ms, int max_body) {
    if (!url) return NULL;

    WebResult* r = (WebResult*)calloc(1, sizeof(WebResult));
    if (!r) return NULL;

    /* URL 解析 + 重定向循环 */
    char host[256], path[1024];
    int port;
    int redirects = 0;

    const char* cur_url = url;
    while (redirects < MAX_REDIRECTS) {
        if (parse_url(cur_url, host, sizeof(host), &port, path, sizeof(path)) != 0)
            { web_result_free(r); return NULL; }

        int body_len = 0;
        char* raw = http_get_raw(host, port, path, timeout_ms, max_body, &body_len);
        if (!raw) { web_result_free(r); return NULL; }

        /* 检查是否重定向（简单解析状态行） */
        /* HTTP/1.x 3xx Location: xxx */
        int code = 0;
        if (strncmp(raw, "HTTP/", 5) == 0) {
            const char* sp = raw + 9; /* 跳过 "HTTP/1.x " */
            while (*sp == ' ') sp++;
            code = atoi(sp);
        }

        if (code >= 301 && code <= 303) {
            /* 找 Location: 头 */
            const char* loc = strstr(raw, "\nLocation:");
            if (!loc) loc = strstr(raw, "\nlocation:");
            if (loc) {
                loc += 10;
                while (*loc == ' ') loc++;
                char new_url[1024];
                int li = 0;
                while (*loc && *loc != '\r' && *loc != '\n' && li < 1023)
                    new_url[li++] = *loc++;
                new_url[li] = '\0';
                free(raw);
                cur_url = strdup(new_url);  /* 注意：这会泄漏，实际应管理好 */
                redirects++;
                continue;
            }
        }

        /* 正常响应 */
        r->status_code = code > 0 ? code : 200;

        /* 找到 body（跳过 HTTP 头） */
        char* body = strstr(raw, "\r\n\r\n");
        if (body) {
            body += 4;
            int blen = (int)strlen(body);
            r->body = (char*)malloc(blen + 1);
            if (r->body) { memcpy(r->body, body, blen + 1); r->body_len = blen; }
        } else {
            r->body = raw;
            r->body_len = body_len;
            raw = NULL;  /* 不 free，已转移所有权 */
        }
        if (raw) free(raw);

        /* 提取 <title> */
        if (r->body) {
            const char* ts = strstr(r->body, "<title>");
            const char* te = ts ? strstr(ts, "</title>") : NULL;
            if (ts && te) {
                ts += 7;
                int tl = (int)(te - ts);
                if (tl > 0) {
                    r->title = (char*)malloc(tl + 1);
                    if (r->title) {
                        memcpy(r->title, ts, tl);
                        r->title[tl] = '\0';
                    }
                }
            }
        }

        /* 提取文本 */
        if (r->body) {
            char* text = (char*)malloc(r->body_len + 1);
            if (text) {
                int tlen = web_extract_text(r->body, text, r->body_len);
                text[tlen] = '\0';

                /* 简单关键词提取：按空格/标点分割 */
                int kw_cap = 32;
                r->keywords = (char**)calloc(kw_cap, sizeof(char*));
                r->keyword_count = 0;

                char* saveptr;
                char* tok = strtok_r(text, " \t\n\r,.;:!?()[]{}，。；：！？（）【】", &saveptr);
                while (tok && r->keyword_count < kw_cap) {
                    int tkl = (int)strlen(tok);
                    if (tkl >= 2 && tkl < 32) {  /* 2-31 字节的词才有意义 */
                        r->keywords[r->keyword_count] = strdup(tok);
                        r->keyword_count++;
                    }
                    tok = strtok_r(NULL, " \t\n\r,.;:!?()[]{}，。；：！？（）【】", &saveptr);
                }
                free(text);
            }
        }

        r->url = strdup(cur_url);
        break;
    }

    return r;
}

void web_result_free(WebResult* r) {
    if (!r) return;
    free(r->url);
    free(r->body);
    free(r->title);
    for (int i = 0; i < r->keyword_count; i++) free(r->keywords[i]);
    free(r->keywords);
    free(r);
}
