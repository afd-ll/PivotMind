/**
 * @file web_search.c
 * @brief 自实现搜索引擎 — HTTP/HTTPS + HTML解析 + URL编码
 *
 * Windows: WinHTTP (HTTPS原生)
 * Linux:   BSD socket HTTP GET; 有OpenSSL时启用HTTPS
 * 回退: HTTPS失败 → 尝试HTTP等价URL
 */
#include "web_search.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
  #include <windows.h>
  #include <winhttp.h>
  #pragma comment(lib, "winhttp.lib")
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <errno.h>
  #ifdef HAS_OPENSSL
    #include <openssl/ssl.h>
    #include <openssl/err.h>
    static int ssl_ready = 0;
  #endif
#endif

/* ================================================================
 *  URL 编码 — 中文等非ASCII字节 → %XX
 * ================================================================ */
static int url_encode(const char* src, char* dst, int dst_sz) {
    if (!src || !dst || dst_sz <= 0) return 0;
    int pos = 0;
    for (const unsigned char* p = (const unsigned char*)src; *p && pos < dst_sz - 4; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
            *p == '.' || *p == '~' || *p == '/') {
            dst[pos++] = (char)*p;
        } else {
            pos += snprintf(dst + pos, dst_sz - pos, "%%%02X", *p);
        }
    }
    dst[pos] = '\0';
    return pos;
}

/* ================================================================
 *  URL 解析
 * ================================================================ */
static int parse_url(const char* url, char* host, int hs, int* port, char* path, int ps) {
    int is_https = (strncmp(url, "https://", 8) == 0);
    const char* p = is_https ? url+8 : (strncmp(url,"http://",7)==0 ? url+7 : url);
    *port = is_https ? 443 : 80;
    int hi=0; while(*p && *p!='/' && *p!=':' && hi<hs-1) host[hi++]=*p++;
    host[hi]=0;
    if(*p==':'){p++; *port=atoi(p); while(isdigit((unsigned char)*p))p++;}
    if(*p){int pi=0; while(*p&&pi<ps-1)path[pi++]=*p++; path[pi]=0;}
    else{path[0]='/';path[1]=0;}
    return is_https;
}

/* ================================================================
 *  Linux: BSD socket HTTP GET
 * ================================================================ */
#ifndef _WIN32
static char* bsd_http_get(const char* host, int port, const char* path,
                          int timeout_ms, int max_body, int* out_len,
                          char** out_content_type) {
    struct hostent* he = gethostbyname(host);
    if (!he) return NULL;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    int sock = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return NULL;

    struct timeval tv = { timeout_ms/1000, (timeout_ms%1000)*1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock); return NULL;
    }

    char req[2048];
    int rl = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\n"
        "User-Agent: Mozilla/5.0 (X11; Linux aarch64) PivotMind/0.2\r\n"
        "Accept: text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.8\r\n"
        "Accept-Language: zh-CN,zh;q=0.9,en;q=0.5\r\n"
        "Connection: close\r\n\r\n", path, host);

    if (send(sock, req, rl, 0) < 0) { close(sock); return NULL; }

    char* body = (char*)malloc(max_body + 1);
    if (!body) { close(sock); return NULL; }
    int total = 0;
    char buf[4096];
    while (total < max_body) {
        int n = (int)recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        if (total + n > max_body) n = max_body - total;
        memcpy(body + total, buf, n);
        total += n;
    }
    body[total] = '\0';
    *out_len = total;
    close(sock);

    /* 提取 Content-Type 响应头（在剥离 header 之前） */
    if (out_content_type) {
        *out_content_type = NULL;
        /* 找 Content-Type: 行（大小写不敏感） */
        char* ct = NULL;
        char* bs1 = strstr(body, "Content-Type:");
        char* bs2 = strstr(body, "content-type:");
        ct = bs1 ? bs1 : bs2;
        if (ct) {
            ct += 13;  /* skip "Content-Type:" */
            while (*ct == ' ' || *ct == ':') ct++;
            char* end = strstr(ct, "\r\n");
            if (!end) end = strstr(ct, "\n");
            if (end) {
                int ct_len = (int)(end - ct);
                if (ct_len > 0 && ct_len < 256) {
                    *out_content_type = (char*)malloc(ct_len + 1);
                    if (*out_content_type) {
                        memcpy(*out_content_type, ct, ct_len);
                        (*out_content_type)[ct_len] = '\0';
                    }
                }
            }
        }
    }

    /* 跳过 HTTP 头，只保留 body */
    char* bs = strstr(body, "\r\n\r\n");
    if (!bs) bs = strstr(body, "\n\n");
    if (bs) {
        int skip = (int)(bs - body) + ((bs[2]=='\n') ? 3 : 4);
        int blen = total - skip;
        if (blen > 0) memmove(body, bs + ((bs[2]=='\n')?3:4), blen);
        body[blen] = '\0';
        *out_len = blen;
    }
    return body;
}

#ifdef HAS_OPENSSL
static char* openssl_https_get(const char* host, int port, const char* path,
                               int timeout_ms, int max_body, int* out_len,
                               char** out_content_type) {
    if (!ssl_ready) {
        SSL_load_error_strings();
        OpenSSL_add_ssl_algorithms();
        ssl_ready = 1;
    }

    /* 1. 解析主机名 */
    struct hostent* he = gethostbyname(host);
    if (!he) return NULL;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    /* 2. 创建 socket + connect */
    int sock = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return NULL;

    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return NULL;
    }

    /* 3. SSL 上下文 — TLS 1.2+ */
    const SSL_METHOD* method = TLS_client_method();
    SSL_CTX* ctx = SSL_CTX_new(method);
    if (!ctx) { close(sock); return NULL; }

    /* 跳过证书验证（对公开网站可接受，后期可加固为 CA bundle） */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

    SSL* ssl = SSL_new(ctx);
    if (!ssl) { SSL_CTX_free(ctx); close(sock); return NULL; }

    SSL_set_fd(ssl, sock);
    if (SSL_connect(ssl) != 1) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(sock);
        return NULL;
    }

    /* 4. 构建并发送 HTTP 请求 */
    char req[2048];
    int rl = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\n"
        "User-Agent: Mozilla/5.0 (X11; Linux aarch64) PivotMind/0.2\r\n"
        "Accept: text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.8\r\n"
        "Accept-Language: zh-CN,zh;q=0.9,en;q=0.5\r\n"
        "Connection: close\r\n\r\n",
        path, host);

    if (SSL_write(ssl, req, rl) <= 0) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(sock);
        return NULL;
    }

    /* 5. 读取响应 */
    char* body = (char*)malloc(max_body + 1);
    if (!body) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(sock);
        return NULL;
    }
    int total = 0;
    while (total < max_body) {
        int n = SSL_read(ssl, body + total, max_body - total);
        if (n <= 0) break;
        total += n;
    }
    body[total] = '\0';
    *out_len = total;

    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(sock);

    /* 6. 提取 Content-Type（在剥离 header 之前） */
    if (out_content_type) {
        *out_content_type = NULL;
        char* ct = NULL;
        char* bs1 = strstr(body, "Content-Type:");
        char* bs2 = strstr(body, "content-type:");
        ct = bs1 ? bs1 : bs2;
        if (ct) {
            ct += 13;
            while (*ct == ' ' || *ct == ':') ct++;
            char* end = strstr(ct, "\r\n");
            if (!end) end = strstr(ct, "\n");
            if (end) {
                int ct_len = (int)(end - ct);
                if (ct_len > 0 && ct_len < 256) {
                    *out_content_type = (char*)malloc(ct_len + 1);
                    if (*out_content_type) {
                        memcpy(*out_content_type, ct, ct_len);
                        (*out_content_type)[ct_len] = '\0';
                    }
                }
            }
        }
    }

    /* 7. 剥离 HTTP 头，只保留 body */
    char* bs = strstr(body, "\r\n\r\n");
    if (!bs) bs = strstr(body, "\n\n");
    if (bs) {
        int skip = (int)(bs - body) + ((bs[2] == '\n') ? 3 : 4);
        int blen = total - skip;
        if (blen > 0) memmove(body, bs + ((bs[2] == '\n') ? 3 : 4), blen);
        body[blen] = '\0';
        *out_len = blen;
    }
    return body;
}
#endif
#endif /* !_WIN32 */

/* ================================================================
 *  主 fetch 函数
 * ================================================================ */

#ifdef _WIN32
static char* do_fetch(const char* url, int timeout_ms, int max_body,
                      int* out_len, int* out_status,
                      char** out_content_type) {
    char host[256], path[2048]; int port, is_https;
    (void)timeout_ms;  /* WinHTTP 自管超时 */
    is_https = parse_url(url, host, sizeof(host), &port, path, sizeof(path));

    *out_content_type = NULL;

    HINTERNET s = WinHttpOpen(L"PivotMind/0.1", 0,0,0,0);
    if (!s) return NULL;

    wchar_t wh[256], wp[2048];
    MultiByteToWideChar(CP_UTF8, 0, host, -1, wh, 256);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 2048);

    HINTERNET c = WinHttpConnect(s, wh, (unsigned short)port, 0);
    if (!c) { WinHttpCloseHandle(s); return NULL; }

    DWORD flags = is_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET r = WinHttpOpenRequest(c, L"GET", wp, 0,0,0, flags);
    if (!r) { WinHttpCloseHandle(c); WinHttpCloseHandle(s); return NULL; }

    if (is_https) {
        DWORD sf = 0x100|0x200|0x1000;
        WinHttpSetOption(r, 31, &sf, sizeof(sf));
    }

    if (!WinHttpSendRequest(r,0,0,0,0,0,0) || !WinHttpReceiveResponse(r,0)) {
        WinHttpCloseHandle(r); WinHttpCloseHandle(c); WinHttpCloseHandle(s); return NULL;
    }

    DWORD st=0,sz=4;
    WinHttpQueryHeaders(r, 19|0x20000000, 0, &st, &sz, 0);
    *out_status = (int)st;

    /* 提取 Content-Type */
    {
        DWORD ct_sz = 0;
        WinHttpQueryHeaders(r, 28, 0, 0, &ct_sz, 0);  /* 28 = WINHTTP_QUERY_CONTENT_TYPE */
        if (ct_sz > 0) {
            wchar_t* wct = (wchar_t*)malloc(ct_sz);
            if (wct && WinHttpQueryHeaders(r, 28, 0, wct, &ct_sz, 0)) {
                int ct_len = WideCharToMultiByte(CP_UTF8, 0, wct, -1, 0, 0, 0, 0);
                if (ct_len > 1) {
                    *out_content_type = (char*)malloc(ct_len);
                    if (*out_content_type)
                        WideCharToMultiByte(CP_UTF8, 0, wct, -1,
                                           *out_content_type, ct_len, 0, 0);
                }
            }
            free(wct);
        }
    }

    char* body = (char*)malloc(max_body+1);
    if (!body) { WinHttpCloseHandle(r); WinHttpCloseHandle(c); WinHttpCloseHandle(s); return NULL; }
    int total=0; DWORD avail, read;
    while (total<max_body && WinHttpQueryDataAvailable(r,&avail) && avail>0) {
        if (avail > (DWORD)(max_body-total)) avail=(DWORD)(max_body-total);
        if (!WinHttpReadData(r, body+total, avail, &read)) break;
        total+=(int)read;
    }
    body[total]=0; *out_len=total;
    WinHttpCloseHandle(r); WinHttpCloseHandle(c); WinHttpCloseHandle(s);
    return body;
}
#else
static char* do_fetch(const char* url, int timeout_ms, int max_body,
                      int* out_len, int* out_status,
                      char** out_content_type) {
    char host[256], path[2048]; int port, is_https;
    (void)timeout_ms;  /* 传入底层 socket 函数 */
    is_https = parse_url(url, host, sizeof(host), &port, path, sizeof(path));

    /* 初始化 */
    *out_content_type = NULL;

    /* HTTPS: 优先尝试 SSL */
#ifdef HAS_OPENSSL
    if (is_https) {
        char* body = openssl_https_get(host, port, path,
                                       timeout_ms, max_body, out_len,
                                       out_content_type);
        if (body) { *out_status = 200; return body; }
    }
#endif

    /* HTTP 直连（非 HTTPS，或 HTTPS+OpenSSL 未编译/失败） */
    if (!is_https) {
        char* body = bsd_http_get(host, port, path,
                                  timeout_ms, max_body, out_len,
                                  out_content_type);
        if (body) { *out_status = 200; return body; }
    }

    return NULL;
}
#endif

/* ================================================================
 *  HTML → 文本（增强版：结构化分层提取）
 * ================================================================ */

/**
 * 提取指定标签内的纯文本（非递归，简单版）
 */
static int _extract_tag_content(const char* html, const char* tag,
                                char* out, int out_sz) {
    if (!html || !tag || !out || out_sz <= 0) return 0;
    int tag_len = (int)strlen(tag);
    char open[64], close[64];
    snprintf(open,  sizeof(open),  "<%s",  tag);
    snprintf(close, sizeof(close), "</%s>", tag);

    int out_pos = 0;
    const char* p = html;
    while (*p && out_pos < out_sz - 1) {
        /* 找开始标签 */
        const char* start = strstr(p, open);
        if (!start) break;
        /* 确保是完整标签（以 > 或空格结尾） */
        const char* after_tag = start + tag_len + 1;
        while (*after_tag == ' ' || (*after_tag >= 'a' && *after_tag <= 'z') ||
               (*after_tag >= 'A' && *after_tag <= 'Z') || *after_tag == '=' ||
               *after_tag == '"' || *after_tag == '\'')
            after_tag++;
        if (*after_tag != '>') { p = start + 1; continue; }

        const char* body_start = after_tag + 1;
        const char* end = strstr(body_start, close);
        if (!end) { p = body_start; continue; }

        /* 提取 body_start 到 end 之间的纯文本（剥离内嵌标签） */
        int in_tag2 = 0;
        for (const char* bp = body_start; bp < end && out_pos < out_sz - 1; bp++) {
            if (*bp == '<') { in_tag2 = 1; continue; }
            if (*bp == '>') { in_tag2 = 0; continue; }
            if (in_tag2) continue;
            if (*bp == '&') {
                if (strncmp(bp, "&amp;", 5) == 0)  { out[out_pos++] = '&'; bp += 4; continue; }
                if (strncmp(bp, "&lt;", 4) == 0)   { out[out_pos++] = '<'; bp += 3; continue; }
                if (strncmp(bp, "&gt;", 4) == 0)   { out[out_pos++] = '>'; bp += 3; continue; }
                if (strncmp(bp, "&quot;", 6) == 0) { out[out_pos++] = '"'; bp += 5; continue; }
                if (strncmp(bp, "&nbsp;", 6) == 0) { out[out_pos++] = ' '; bp += 5; continue; }
                if (bp[1] == '#') { while (*bp && *bp != ';') bp++; continue; }
            }
            out[out_pos++] = *bp;
        }
        if (out_pos > 0 && out[out_pos - 1] != '\n') {
            out[out_pos++] = '\n';
        }
        p = end + strlen(close);
    }
    out[out_pos] = '\0';
    return out_pos;
}

/**
 * 提取 <meta name="description" content="..."> 中的描述文本
 */
int web_extract_meta_description(const char* html, char* out, int out_sz) {
    if (!html || !out || out_sz <= 0) return 0;

    /* 大小写不敏感搜索 */
    const char* p = html;
    while (*p) {
        const char* m = strstr(p, "<meta");
        if (!m) break;
        /* 检查是否包含 name="description" 或 name=description */
        const char* end_tag = strchr(m, '>');
        if (!end_tag) { p = m + 1; continue; }
        if ((strstr(m, "name=\"description\"") || strstr(m, "name='description'") ||
             strstr(m, "name=description")) &&
            strstr(m, "content=")) {
            /* 提取 content="..." 或 content='...' 的值 */
            const char* c = strstr(m, "content=\"");
            if (!c) c = strstr(m, "content='");
            if (!c) c = strstr(m, "content=");
            if (c) {
                c += 8;  /* skip "content=" */
                if (*c == '"' || *c == '\'') c++;
                const char* cend = c;
                while (*cend && *cend != '"' && *cend != '\'' && *cend != '>' && *cend != '/')
                    cend++;
                int len = (int)(cend - c);
                if (len > 0 && len < out_sz) {
                    memcpy(out, c, len);
                    out[len] = '\0';

                    /* HTML 实体解码 */
                    char* amp;
                    while ((amp = strstr(out, "&amp;"))) {
                        int rest = (int)strlen(amp + 5);
                        memmove(amp + 1, amp + 5, rest + 1);
                        *amp = '&';
                    }
                    while ((amp = strstr(out, "&nbsp;"))) {
                        int rest2 = (int)strlen(amp + 6);
                        memmove(amp + 1, amp + 6, rest2 + 1);
                        *amp = ' ';
                    }
                    while ((amp = strstr(out, "&#39;"))) {
                        int rest3 = (int)strlen(amp + 5);
                        memmove(amp + 1, amp + 5, rest3 + 1);
                        *amp = '\'';
                    }
                    while ((amp = strstr(out, "&quot;"))) {
                        int rest4 = (int)strlen(amp + 6);
                        memmove(amp + 1, amp + 6, rest4 + 1);
                        *amp = '"';
                    }

                    return (int)strlen(out);
                }
            }
        }
        p = end_tag + 1;
    }
    return 0;
}

/** 提取所有 h1/h2 标题 */
int web_extract_headings(const char* html, char* out, int out_sz) {
    int total = 0;
    int n1 = _extract_tag_content(html, "h1", out, out_sz);
    total += n1;
    if (total < out_sz - 1) {
        int n2 = _extract_tag_content(html, "h2", out + total, out_sz - total);
        total += n2;
    }
    return total;
}

/** 提取前 N 个段落 */
int web_extract_paragraphs(const char* html, char* out, int out_sz,
                           int max_paragraphs) {
    if (!html || !out || out_sz <= 0) return 0;
    int total = 0;
    const char* p = html;
    int para_count = 0;
    while (*p && para_count < max_paragraphs && total < out_sz - 1) {
        const char* start_tag = strstr(p, "<p");
        if (!start_tag) break;
        const char* after_tag = start_tag + 2;
        while (*after_tag && *after_tag != '>') after_tag++;
        if (*after_tag != '>') { p = start_tag + 2; continue; }

        const char* body = after_tag + 1;
        const char* end_tag = strstr(body, "</p>");
        if (!end_tag) { p = body; continue; }

        /* 提取段落纯文本 */
        int in_tag = 0;
        for (const char* bp = body; bp < end_tag && total < out_sz - 1; bp++) {
            if (*bp == '<') { in_tag = 1; continue; }
            if (*bp == '>') { in_tag = 0; continue; }
            if (in_tag) continue;
            if (*bp == '&') {
                if (strncmp(bp, "&amp;", 5) == 0)  { out[total++] = '&'; bp += 4; continue; }
                if (strncmp(bp, "&lt;", 4) == 0)   { out[total++] = '<'; bp += 3; continue; }
                if (strncmp(bp, "&gt;", 4) == 0)   { out[total++] = '>'; bp += 3; continue; }
                if (strncmp(bp, "&quot;", 6) == 0) { out[total++] = '"'; bp += 5; continue; }
                if (strncmp(bp, "&nbsp;", 6) == 0) { out[total++] = ' '; bp += 5; continue; }
                if (bp[1] == '#') { while (*bp && *bp != ';') bp++; continue; }
            }
            out[total++] = *bp;
        }
        out[total++] = '\n';
        para_count++;
        p = end_tag + 4;
    }
    out[total] = '\0';
    return total;
}

/**
 * 回退策略：原始 tag 剥离（保留作为最终 fallback）
 */
static int _legacy_extract_text(const char* html, char* out, int out_sz) {
    if (!html||!out||out_sz<=0) return 0;
    int pos=0, in_tag=0, last_ws=1;
    for (const char* p=html; *p&&pos<out_sz-1; p++) {
        if (*p=='<'){in_tag=1;continue;}
        if(*p=='>'){in_tag=0;continue;}
        if(in_tag)continue;
        if(*p=='&'){
            if(strncmp(p,"&amp;",5)==0){out[pos++]='&';p+=4;continue;}
            if(strncmp(p,"&lt;",4)==0){out[pos++]='<';p+=3;continue;}
            if(strncmp(p,"&gt;",4)==0){out[pos++]='>';p+=3;continue;}
            if(strncmp(p,"&quot;",6)==0){out[pos++]='"';p+=5;continue;}
            if(strncmp(p,"&nbsp;",6)==0){out[pos++]=' ';p+=5;continue;}
            if(p[1]=='#'){while(*p&&*p!=';')p++;continue;}}
        if(isspace((unsigned char)*p)){if(!last_ws){out[pos++]=' ';last_ws=1;}continue;}
        out[pos++]=*p;last_ws=0;
    }
    out[pos]=0; return pos;
}

/**
 * 主入口：分层提取 HTML 文本
 *
 * 策略 1: <meta name="description"> — 最高质量摘要（百度百科/维基标准）
 * 策略 2: <h1>/<h2> 标题 + 前 3 段 <p> — 结构化正文
 * 策略 3: 回退到原始 tag 剥离
 */
int web_extract_text(const char* html, char* out, int out_sz) {
    if (!html || !out || out_sz <= 0) return 0;

    /* 策略 1: meta description */
    int len = web_extract_meta_description(html, out, out_sz);
    if (len > 80) return len;  /* 有效摘要 */

    /* 策略 2: 标题 + 段落 */
    int total = web_extract_headings(html, out, out_sz);
    if (total < out_sz - 1) {
        int n = web_extract_paragraphs(html, out + total, out_sz - total, 3);
        total += n;
    }
    if (total > 50) return total;

    /* 策略 3: 回退到原有逻辑 */
    return _legacy_extract_text(html, out, out_sz);
}

/* ================================================================
 *  主 API
 * ================================================================ */
WebResult* web_search(const char* url, int timeout_ms, int max_body) {
    if (!url) return NULL;

    /* URL编码：自动处理中文 */
    char encoded[2048];
    if (strchr(url, '%') == NULL) {
        /* 未编码的URL → 编码path部分 */
        int is_https = (strncmp(url, "https://", 8) == 0);
        const char* p = is_https ? url+8 : (strncmp(url,"http://",7)==0 ? url+7 : url);
        /* 找到path起始 */
        const char* path_start = strchr(p, '/');
        if (!path_start) path_start = p + strlen(p);
        int prefix_len = (int)(path_start - url);
        /* 编码path */
        char encoded_path[1536];
        url_encode(path_start, encoded_path, sizeof(encoded_path));
        snprintf(encoded, sizeof(encoded), "%.*s%s", prefix_len, url, encoded_path);
    } else {
        snprintf(encoded, sizeof(encoded), "%s", url);
    }

    WebResult* r = (WebResult*)calloc(1,sizeof(WebResult));
    if (!r) return NULL;

    int body_len=0, status=0;
    char* content_type = NULL;
    char* raw = do_fetch(encoded, timeout_ms, max_body, &body_len, &status, &content_type);
    if (!raw) { free(r); free(content_type); return NULL; }

    r->status_code = status;
    r->body = raw;
    r->body_len = body_len;
    r->content_type = content_type;  /* 可能为 NULL */

    /* 提取 <title> */
    if (r->body) {
        const char* ts = strstr(r->body, "<title>");
        const char* te = ts?strstr(ts,"</title>"):NULL;
        if (ts&&te) {
            ts+=7; int tl=(int)(te-ts);
            if(tl>0&&tl<256){r->title=(char*)malloc(tl+1);
                if(r->title){memcpy(r->title,ts,tl);r->title[tl]=0;}}
        }
    }

    /* 提取关键词（含中文n-gram） */
    if (r->body && r->body_len>0) {
        char* text = (char*)malloc(r->body_len+1);
        if (text) {
            int tl = web_extract_text(r->body, text, r->body_len);
            text[tl] = 0;
            int kw_cap = 32;
            r->keywords = (char**)calloc(kw_cap, sizeof(char*));

            /* 双策略：strtok分词 + 中文2-gram补充 */
            char* delim = " \t\n\r,.;:!?()[]{}<>，。；：！？（）【】《》";
#ifdef _WIN32
            char* next=NULL;
            char* tok = strtok_s(text, delim, &next);
            while(tok&&r->keyword_count<kw_cap-10){
#else
            char* sv;
            char* tok = strtok_r(text, delim, &sv);
            while(tok&&r->keyword_count<kw_cap-10){
#endif
                int tkl=(int)strlen(tok);
                if(tkl>=2&&tkl<32){
                    int ht=0; for(const char*cp=tok;*cp;cp++)
                        if((unsigned char)*cp>127||isalpha((unsigned char)*cp)){ht=1;break;}
                    if(ht){r->keywords[r->keyword_count]=strdup(tok); r->keyword_count++;}
                }
#ifdef _WIN32
                tok=strtok_s(NULL,delim,&next);
#else
                tok=strtok_r(NULL,delim,&sv);
#endif
            }

            /* 中文2-gram补充：从文本中提取连续的CJK字符对 */
            int kg_start = r->keyword_count;
            for (int i=0; i<tl-2 && r->keyword_count<kw_cap; i++) {
                unsigned char c1=(unsigned char)text[i];
                unsigned char c2=(unsigned char)text[i+1];
                unsigned char c3=(unsigned char)text[i+2];
                unsigned char c4=(unsigned char)text[i+3];
                /* 2个CJK字符（3字节*2=6字节UTF-8）→ 中文词组 */
                if (c1>=0xE0 && c2>=0x80 && (c3>=0xE0||c3<0x80)) {
                    if (c3>=0xE0 && c4>=0x80) {
                        char bigram[7];
                        bigram[0]=text[i]; bigram[1]=text[i+1]; bigram[2]=text[i+2];
                        bigram[3]=text[i+3]; bigram[4]=text[i+4]; bigram[5]=text[i+5];
                        bigram[6]=0;
                        int dup=0;
                        for(int d=kg_start; d<r->keyword_count; d++)
                            if(strcmp(r->keywords[d],bigram)==0){dup=1;break;}
                        if(!dup&&r->keyword_count<kw_cap) {
                            r->keywords[r->keyword_count]=strdup(bigram);
                            r->keyword_count++;
                        }
                        i+=2; /* skip next UTF-8 char */
                    }
                }
            }
            free(text);
        }
    }

    r->url = strdup(url);
    return r;
}

void web_result_free(WebResult* r) {
    if (!r) return;
    free(r->url); free(r->body); free(r->title); free(r->content_type);
    for(int i=0;i<r->keyword_count;i++) free(r->keywords[i]);
    free(r->keywords); free(r);
}
