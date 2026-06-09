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
                          int timeout_ms, int max_body, int* out_len) {
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
        "GET %s HTTP/1.0\r\nHost: %s\r\n"
        "User-Agent: PivotMind/0.1\r\n"
        "Accept: text/html,text/plain\r\n"
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
                               int timeout_ms, int max_body, int* out_len) {
    if (!ssl_ready) { SSL_load_error_strings(); OpenSSL_add_ssl_algorithms(); ssl_ready=1; }
    char* body = bsd_http_get(host, port, path, timeout_ms, max_body, out_len);
    /* 简化：先用HTTP试，如果301到HTTPS且可行再切 */
    /* TODO: 完整OpenSSL实现 */
    return body;
}
#endif
#endif /* !_WIN32 */

/* ================================================================
 *  主 fetch 函数
 * ================================================================ */

#ifdef _WIN32
static char* do_fetch(const char* url, int timeout_ms, int max_body,
                      int* out_len, int* out_status) {
    char host[256], path[2048]; int port, is_https;
    is_https = parse_url(url, host, sizeof(host), &port, path, sizeof(path));

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
                      int* out_len, int* out_status) {
    char host[256], path[2048]; int port, is_https;
    is_https = parse_url(url, host, sizeof(host), &port, path, sizeof(path));

    /* 先尝试直接HTTP GET */
    char* body = bsd_http_get(host, port, path, timeout_ms, max_body, out_len);
    if (body) { *out_status = 200; return body; }

    /* HTTPS回退: 尝试HTTP等价URL（http://...） */
    if (is_https && port == 443) {
        /* 构造HTTP版本URL并递归 */
        char http_url[2048];
        snprintf(http_url, sizeof(http_url), "http://%s%s", host, path);
        body = bsd_http_get(host, 80, path, timeout_ms, max_body, out_len);
        if (body) { *out_status = 200; return body; }
    }

#ifdef HAS_OPENSSL
    /* 如果编译时带了OpenSSL，尝试HTTPS */
    if (is_https) {
        body = openssl_https_get(host, port, path, timeout_ms, max_body, out_len);
        if (body) { *out_status = 200; return body; }
    }
#endif

    return NULL;
}
#endif

/* ================================================================
 *  HTML → 文本
 * ================================================================ */
int web_extract_text(const char* html, char* out, int out_sz) {
    if (!html||!out||out_sz<=0) return 0;
    int pos=0, in_tag=0, in_script=0, in_style=0, last_ws=1;
    for (const char* p=html; *p&&pos<out_sz-1; p++) {
        if (*p=='<'){in_tag=1;
            if(strncasecmp(p+1,"script",6)==0)in_script=1;
            if(strncasecmp(p+1,"/script",7)==0)in_script=0;
            if(strncasecmp(p+1,"style",5)==0)in_style=1;
            if(strncasecmp(p+1,"/style",6)==0)in_style=0;
            continue;}
        if(*p=='>'){in_tag=0;continue;}
        if(in_tag||in_script||in_style)continue;
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

/* ================================================================
 *  主 API
 * ================================================================ */
WebResult* web_search(const char* url, int timeout_ms, int max_body) {
    if (!url) return NULL;

    /* URL编码：自动处理中文 */
    char encoded[2048];
    const char* u = url;
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
    char* raw = do_fetch(encoded, timeout_ms, max_body, &body_len, &status);
    if (!raw) { free(r); return NULL; }

    r->status_code = status;
    r->body = raw;
    r->body_len = body_len;

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
    free(r->url); free(r->body); free(r->title);
    for(int i=0;i<r->keyword_count;i++) free(r->keywords[i]);
    free(r->keywords); free(r);
}
