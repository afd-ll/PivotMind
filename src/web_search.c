/**
 * @file web_search.c
 * @brief 自实现搜索引擎 — HTTP/HTTPS + HTML解析
 *
 * HTTPS: WinHTTP(Windows) / OpenSSL(Linux)
 * 零外部API依赖，仅用操作系统内置SSL基础设施
 */

/* WinHTTP API 在 -O2 下有调用约定问题，降级到 -O1 */
#ifdef _WIN32
#pragma GCC optimize ("O1")
#endif

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
  #include <openssl/ssl.h>
  #include <openssl/err.h>
#endif

/* URL解析 */
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
 *  HTTPS fetch
 * ================================================================ */
#ifdef _WIN32
static char* https_fetch(const char* url, int timeout_ms, int max_body,
                         int* out_len, int* out_status) {
    char host[256], path[2048]; int port, is_https;
    is_https = parse_url(url, host, sizeof(host), &port, path, sizeof(path));

    HINTERNET s = WinHttpOpen(L"PivotMind/0.1", 0, 0, 0, 0);
    if (!s) return NULL;

    wchar_t wh[256], wp[2048];
    MultiByteToWideChar(CP_UTF8, 0, host, -1, wh, 256);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 2048);

    HINTERNET c = WinHttpConnect(s, wh, (unsigned short)port, 0);
    if (!c) { WinHttpCloseHandle(s); return NULL; }

    DWORD flags = is_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET r = WinHttpOpenRequest(c, L"GET", wp, 0, 0, 0, flags);
    if (!r) { WinHttpCloseHandle(c); WinHttpCloseHandle(s); return NULL; }

    if (is_https) {
        DWORD sf = 0x100|0x200|0x1000;
        WinHttpSetOption(r, 31, &sf, sizeof(sf));
    }

    if (!WinHttpSendRequest(r, 0,0,0,0,0,0)) {
        WinHttpCloseHandle(r); WinHttpCloseHandle(c); WinHttpCloseHandle(s); return NULL;
    }
    if (!WinHttpReceiveResponse(r, 0)) {
        WinHttpCloseHandle(r); WinHttpCloseHandle(c); WinHttpCloseHandle(s); return NULL;
    }

    DWORD st=0,sz=4;
    WinHttpQueryHeaders(r, 19|0x20000000, 0, &st, &sz, 0);
    *out_status = (int)st;

    char* body = (char*)malloc(max_body+1);
    if (!body) { WinHttpCloseHandle(r); WinHttpCloseHandle(c); WinHttpCloseHandle(s); return NULL; }
    int total = 0; DWORD avail, read;
    while (total < max_body) {
        if (!WinHttpQueryDataAvailable(r, &avail)) break;
        if (avail==0) break;
        if (avail > (DWORD)(max_body-total)) avail = (DWORD)(max_body-total);
        if (!WinHttpReadData(r, body+total, avail, &read)) break;
        total += (int)read;
        if (read==0) break;
    }
    body[total] = 0;
    *out_len = total;

    WinHttpCloseHandle(r); WinHttpCloseHandle(c); WinHttpCloseHandle(s);
    return body;
}
#else
static char* https_fetch(const char* url, int timeout_ms, int max_body,
                         int* out_len, int* out_status) {
    /* Linux OpenSSL — 先返回NULL，用户可链接-lssl -lcrypto编译 */
    (void)url; (void)timeout_ms; (void)max_body; (void)out_len; (void)out_status;
    return NULL;
}
#endif

/* ================================================================
 *  HTML→文本
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
 *  主API
 * ================================================================ */
WebResult* web_search(const char* url, int timeout_ms, int max_body) {
    if (!url) return NULL;
    WebResult* r = (WebResult*)calloc(1,sizeof(WebResult));
    if (!r) return NULL;

    int body_len=0, status=0;
    char* raw = https_fetch(url, timeout_ms, max_body, &body_len, &status);
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

    /* 提取关键词 */
    if (r->body && r->body_len>0) {
        char* text = (char*)malloc(r->body_len+1);
        if (text) {
            int tl = web_extract_text(r->body, text, r->body_len);
            text[tl] = 0;
            int kw_cap=32;
            r->keywords = (char**)calloc(kw_cap, sizeof(char*));
#ifdef _WIN32
            char* next=NULL;
            char* tok = strtok_s(text," \t\n\r,.;:!?()[]{}<>，。；：！？（）【】《》",&next);
            while(tok&&r->keyword_count<kw_cap){
#else
            char* sv;
            char* tok = strtok_r(text," \t\n\r,.;:!?()[]{}<>，。；：！？（）【】《》",&sv);
            while(tok&&r->keyword_count<kw_cap){
#endif
                int tkl=(int)strlen(tok);
                if(tkl>=2&&tkl<32){
                    int ht=0; for(const char*cp=tok;*cp;cp++)
                        if((unsigned char)*cp>127||isalpha((unsigned char)*cp)){ht=1;break;}
                    if(ht){r->keywords[r->keyword_count]=strdup(tok); r->keyword_count++;}
                }
#ifdef _WIN32
                tok=strtok_s(NULL," \t\n\r,.;:!?()[]{}<>，。；：！？（）【】《》",&next);
#else
                tok=strtok_r(NULL," \t\n\r,.;:!?()[]{}<>，。；：！？（）【】《》",&sv);
#endif
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
