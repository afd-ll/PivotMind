/**
 * @file web_search.c
 * @brief 网页搜索 — 基于 web_fetch 框架的 HTML 抓取 + 解析
 *
 * 传输层：委托给 web_fetch.c（libcurl 引擎 + 策略管控）
 * 解析层：HTML → 纯文本提取（多策略分层）
 *
 * 跨平台：web_fetch 内部处理 Windows/Linux 差异，本文件无需平台条件编译
 */

#include "web_search.h"
#include "web_fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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
 * 搜索结果页文本提取策略 — 适配 Bing/Google 搜索结果页
 * 提取搜索摘要片段和标题链接文本
 */
static int _extract_search_results(const char* html, char* out, int out_sz) {
    if (!html || !out || out_sz <= 0) return 0;

    int total = 0;

    /* 提取所有 <h2> / <h3> 标题（搜索结果通常以标题链接呈现） */
    int h2 = _extract_tag_content(html, "h2", out + total, out_sz - total);
    total += h2;
    if (total < out_sz - 1) {
        int h3 = _extract_tag_content(html, "h3", out + total, out_sz - total);
        total += h3;
    }

    /* 提取 <cite> 标签（URL/来源标注） */
    if (total < out_sz - 1) {
        total += _extract_tag_content(html, "cite", out + total, out_sz - total);
    }

    /* 回退：用普通段落提取兜底 */
    if (total < 50 && total < out_sz - 1) {
        total += web_extract_paragraphs(html, out + total, out_sz - total, 5);
    }

    return total;
}

/**
 * 主入口：分层提取 HTML 文本
 *
 * 策略 0: 检测是否为搜索结果页 → 搜索摘要提取
 * 策略 1: <meta name="description"> — 最高质量摘要（百度百科/维基标准）
 * 策略 2: <h1>/<h2> 标题 + 前 3 段 <p> — 结构化正文
 * 策略 3: 回退到原始 tag 剥离
 */
int web_extract_text(const char* html, char* out, int out_sz) {
    if (!html || !out || out_sz <= 0) return 0;

    /* 策略 0: 快速检测是否为搜索结果页（含 /search 或 "result" 等特征） */
    {
        const char* check = html;
        int search_hints = 0;
        if (strstr(check, "/search")) search_hints++;
        if (strstr(check, "result-stats") || strstr(check, "resultStats")) search_hints++;
        if (strstr(check, "search-results") || strstr(check, "searchResults")) search_hints++;
        if (strstr(check, "b_results") || strstr(check, "b_algo")) search_hints++; /* Bing */
        if (strstr(check, "\"estimatedResultCount\"") || strstr(check, "\"webPages\"")) search_hints++;

        if (search_hints >= 2) {
            int len = _extract_search_results(html, out, out_sz);
            if (len > 30) return len;
        }
    }

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
 *  主 API — 委托给 web_fetch 框架
 * ================================================================ */

WebResult* web_search(const char* url, int timeout_ms, int max_body) {
    (void)timeout_ms;  /* 超时由 web_fetch 的 CrawlPolicy 统一管理 */
    if (!url) return NULL;

    /* 使用 web_fetch 替代裸 socket */
    FetchResult* fr = web_fetch(url);
    if (!fr) return NULL;

    /* 永久封禁或网络错误 → 直接返回 NULL */
    if (fr->fetch_class == FETCH_PERM_BLOCK ||
        fr->fetch_class == FETCH_NETWORK_ERR) {
        web_fetch_result_free(fr);
        return NULL;
    }

    /* 构造 WebResult（保持现有 API 兼容） */
    WebResult* r = (WebResult*)calloc(1, sizeof(WebResult));
    if (!r) { web_fetch_result_free(fr); return NULL; }

    r->url          = strdup(fr->final_url ? fr->final_url : url);
    r->status_code  = fr->status_code;
    r->body         = fr->body;          /* 转移所有权 */
    r->body_len     = fr->body_len;
    r->content_type = fr->content_type;  /* 转移所有权 */
    fr->body = NULL;
    fr->content_type = NULL;
    web_fetch_result_free(fr);

    /* 截断 body 到 max_body */
    if (r->body && r->body_len > max_body) {
        r->body_len = max_body;
        r->body[r->body_len] = '\0';
    }

    /* 提取 <title> */
    if (r->body) {
        const char* ts = strstr(r->body, "<title>");
        const char* te = ts ? strstr(ts, "</title>") : NULL;
        if (ts && te) {
            ts += 7;
            int tl = (int)(te - ts);
            if (tl > 0 && tl < 256) {
                r->title = (char*)malloc(tl + 1);
                if (r->title) { memcpy(r->title, ts, tl); r->title[tl] = 0; }
            }
        }
    }

    /* 提取关键词（含中文 n-gram） */
    if (r->body && r->body_len > 0) {
        char* text = (char*)malloc(r->body_len + 1);
        if (text) {
            int tl = web_extract_text(r->body, text, r->body_len);
            text[tl] = 0;
            int kw_cap = 32;
            r->keywords = (char**)calloc(kw_cap, sizeof(char*));

            /* 双策略：strtok分词 + 中文2-gram补充 */
            char* delim = " \t\n\r,.;:!?()[]{}<>，。；：！？（）【】《》";
#ifdef _WIN32
            char* next = NULL;
            char* tok = strtok_s(text, delim, &next);
            while (tok && r->keyword_count < kw_cap - 10) {
#else
            char* sv;
            char* tok = strtok_r(text, delim, &sv);
            while (tok && r->keyword_count < kw_cap - 10) {
#endif
                int tkl = (int)strlen(tok);
                if (tkl >= 2 && tkl < 32) {
                    int ht = 0;
                    for (const char* cp = tok; *cp; cp++)
                        if ((unsigned char)*cp > 127 || isalpha((unsigned char)*cp)) { ht = 1; break; }
                    if (ht) { r->keywords[r->keyword_count] = strdup(tok); r->keyword_count++; }
                }
#ifdef _WIN32
                tok = strtok_s(NULL, delim, &next);
#else
                tok = strtok_r(NULL, delim, &sv);
#endif
            }

            /* 中文2-gram补充 */
            int kg_start = r->keyword_count;
            for (int i = 0; i < tl - 2 && r->keyword_count < kw_cap; i++) {
                unsigned char c1 = (unsigned char)text[i];
                unsigned char c2 = (unsigned char)text[i + 1];
                unsigned char c3 = (unsigned char)text[i + 2];
                unsigned char c4 = (unsigned char)text[i + 3];
                if (c1 >= 0xE0 && c2 >= 0x80 && (c3 >= 0xE0 || c3 < 0x80)) {
                    if (c3 >= 0xE0 && c4 >= 0x80) {
                        char bigram[7];
                        bigram[0] = text[i]; bigram[1] = text[i+1]; bigram[2] = text[i+2];
                        bigram[3] = text[i+3]; bigram[4] = text[i+4]; bigram[5] = text[i+5];
                        bigram[6] = 0;
                        int dup = 0;
                        for (int d = kg_start; d < r->keyword_count; d++)
                            if (strcmp(r->keywords[d], bigram) == 0) { dup = 1; break; }
                        if (!dup && r->keyword_count < kw_cap) {
                            r->keywords[r->keyword_count] = strdup(bigram);
                            r->keyword_count++;
                        }
                        i += 2;
                    }
                }
            }
            free(text);
        }
    }

    if (!r->url) r->url = strdup(url);
    return r;
}

void web_result_free(WebResult* r) {
    if (!r) return;
    free(r->url);
    free(r->body);
    free(r->title);
    free(r->content_type);
    for (int i = 0; i < r->keyword_count; i++) free(r->keywords[i]);
    free(r->keywords);
    free(r);
}
