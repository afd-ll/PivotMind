/* qa_crawler.c — Bing RSS 搜索 + 文章抓取训练管线
 *
 * 搜索: cn.bing.com/search?format=rss (XML, 无 JS, ~5KB/请求)
 * 内容: 抓取搜索结果中的文章 URL 获取全文
 * 训练: article_reader PMI 词发现 + autonomic_learn_from_dialog
 *
 * 安全: fork+exec curl, 父进程超时保护, 不会卡死
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>

#include "multi_topology.h"
#include "perception.h"
#include "memory_system.h"
#include "autonomic_learner.h"
#include "dream_engine.h"
#include "article_reader.h"

/* 默认搜索词 — 覆盖广泛领域的知识性问题 */
static const char* g_queries[] = {
    "人工智能入门教程","机器学习基础","深度学习原理",
    "Python编程入门","C语言教程","Linux常用命令",
    "数据结构与算法","计算机网络基础","操作系统原理",
    "数据库MySQL教程","Git版本控制","Docker入门",
    "什么是神经网络","Transformer模型","自然语言处理",
    "数学分析基础","线性代数","概率论与统计",
    "物理学常识","化学基础知识","生物进化论",
    "中国历史朝代","世界地理","经济学原理",
    "心理学入门","哲学基本问题","社会学概论",
    "如何学习编程","读书方法","时间管理技巧",
    "健康饮食","运动健身","睡眠科学",
};
#define Q_COUNT (int)(sizeof(g_queries)/sizeof(g_queries[0]))

/* URL 编码 — 仅编码中文和特殊字符 */
static void url_encode(const char* src, char* dst, int dst_sz) {
    const char* hex = "0123456789ABCDEF";
    int di = 0;
    for (const char* p = src; *p && di < dst_sz - 4; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            dst[di++] = c;
        } else {
            dst[di++] = '%';
            dst[di++] = hex[c >> 4];
            dst[di++] = hex[c & 0x0F];
        }
    }
    dst[di] = '\0';
}

/* curl 抓取 (改用 system() + 临时文件，比 fork/exec 更可靠) */
static char* curl_fetch(const char* url, int max_wait_s) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "/tmp/qa_crawl_%d.html", (int)getpid());
    /* 先用 rm 确保文件不存在（防止旧文件干扰） */
    unlink(tmp);

    char cmd[2048];
    int timeout = max_wait_s > 0 ? max_wait_s : 8;
    snprintf(cmd, sizeof(cmd),
             "curl -sL --max-time %d --connect-timeout 5 "
             "-A 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36' "
             "-o '%s' '%s' </dev/null 2>/dev/null",
             timeout, tmp, url);

    int rc = system(cmd);
    if (rc != 0) return NULL;

    FILE* f = fopen(tmp, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > 524288) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char* buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, sz, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

/* 从 RSS XML 提取 <item> 内的 <link> */
static char* rss_extract_link(const char* item_xml) {
    const char* start = strstr(item_xml, "<link>");
    if (!start) return NULL;
    start += 6;
    const char* end = strstr(start, "</link>");
    if (!end) return NULL;
    int len = end - start;
    if (len <= 0 || len > 1024) return NULL;
    char* url = malloc(len + 1);
    if (!url) return NULL;
    memcpy(url, start, len);
    url[len] = '\0';
    if (strncmp(url, "<![CDATA[", 9) == 0) {
        memmove(url, url + 9, len - 9);
        url[len - 9] = '\0';
        int clen = strlen(url);
        if (clen >= 3 && strcmp(url + clen - 3, "]]>") == 0)
            url[clen - 3] = '\0';
    }
    return url;
}

/* 从 RSS XML 提取 <title> */
static char* rss_extract_title(const char* item_xml) {
    const char* start = strstr(item_xml, "<title>");
    if (!start) return NULL;
    start += 7;
    const char* end = strstr(start, "</title>");
    if (!end) return NULL;
    int len = end - start;
    if (len <= 0 || len > 512) return NULL;
    char* title = malloc(len + 1);
    if (!title) return NULL;
    memcpy(title, start, len);
    title[len] = '\0';
    if (strncmp(title, "<![CDATA[", 9) == 0) {
        memmove(title, title + 9, len - 9);
        title[len - 9] = '\0';
        int clen = strlen(title);
        if (clen >= 3 && strcmp(title + clen - 3, "]]>") == 0)
            title[clen - 3] = '\0';
    }
    return title;
}

/* HTML → 纯文本 */
static char* html_to_text(const char* html, int max_sz) {
    if (!html) return NULL;
    int limit = max_sz > 0 ? max_sz : 65536;
    char* out = malloc(limit);
    if (!out) return NULL;
    int pos = 0, in_tag = 0, in_script = 0, in_style = 0;
    const char* p = html;
    while (*p && pos < limit - 1) {
        if (*p == '<') {
            in_tag = 1;
            if (!in_script && strncasecmp(p, "<script", 7) == 0) in_script = 1;
            if (!in_style && strncasecmp(p, "<style", 6) == 0) in_style = 1;
        }
        if (!in_tag && !in_script && !in_style) {
            if (*p == '&') {
                if (strncmp(p, "&nbsp;", 6) == 0)      { out[pos++] = ' '; p += 5; }
                else if (strncmp(p, "&lt;", 4) == 0)   { out[pos++] = '<'; p += 3; }
                else if (strncmp(p, "&gt;", 4) == 0)   { out[pos++] = '>'; p += 3; }
                else if (strncmp(p, "&amp;", 5) == 0)  { out[pos++] = '&'; p += 4; }
                else if (strncmp(p, "&quot;", 6) == 0) { out[pos++] = '"'; p += 5; }
                else { out[pos++] = *p; }
            } else if (*p == '\n' || *p == '\r' || *p == '\t') {
                if (pos > 0 && out[pos-1] != ' ') out[pos++] = ' ';
            } else if ((unsigned char)*p >= 0x20) {
                out[pos++] = *p;
            }
        }
        if (in_tag && *p == '>') {
            in_tag = 0;
            if (in_script && strncasecmp(p-7, "/script", 7) == 0) in_script = 0;
            if (in_style && strncasecmp(p-6, "/style", 6) == 0) in_style = 0;
            if (!in_script && !in_style) out[pos++] = ' ';
        }
        p++;
    }
    out[pos] = '\0';
    return out;
}

/* 已知返回静态 HTML 的站点（白名单，避免知乎等 JS 站浪费超时等待） */
static int url_is_ok(const char* url) {
    const char* whitelist[] = {
        "runoob.com", "csdn.net", "cnblogs.com", "blog.csdn.net",
        "w3cschool.cn", "c.biancheng.net", "liaoxuefeng.com",
        "zhuanlan.zhihu.com", "jianshu.com", "juejin.cn",
        "developer.aliyun.com", "cloud.tencent.com/developer",
        NULL
    };
    /* 排除纯 JS 站点和明确反爬的 */
    const char* blacklist[] = {
        "zhihu.com/question", "www.zhihu.com/question",
        "bilibili.com", "youtube.com", "douyin.com",
        NULL
    };
    for (int i = 0; blacklist[i]; i++)
        if (strstr(url, blacklist[i])) return 0;
    for (int i = 0; whitelist[i]; i++)
        if (strstr(url, whitelist[i])) return 1;
    /* 不在黑名单的也允许（未知站点默认允许，超时保护即可） */
    return 1;
}
static int url_seen(char** seen, int sc, const char* url) {
    for (int i = 0; i < sc; i++)
        if (strcmp(seen[i], url) == 0) return 1;
    return 0;
}

int main(int argc, char* argv[]) {
    const char* qf = NULL;
    int rounds = 1, save_every = 3, delay_s = 3;
    int fetch_articles = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--queries") && i+1 < argc) qf = argv[++i];
        else if (!strcmp(argv[i], "--rounds") && i+1 < argc) rounds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--save-every") && i+1 < argc) save_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--delay") && i+1 < argc) delay_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fetch-articles")) fetch_articles = 1;
    }

    const char** qs = g_queries;
    int qc = Q_COUNT;
    char* custom[512]; int cc = 0;
    if (qf) {
        FILE* f = fopen(qf, "r"); if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f) && cc < 512) {
                line[strcspn(line, "\r\n")] = 0;
                if (line[0]) custom[cc++] = strdup(line);
            } fclose(f);
            qs = (const char**)custom; qc = cc;
        }
    }

    printf("=== QA Crawler v2 === Bing RSS + article fetch ===\n");
    printf("    %d queries x %d rounds, delay %ds\n", qc, rounds, delay_s);
    printf("    Article fetch: %s\n\n", fetch_articles ? "ON" : "OFF (RSS snippets only)");

    /* 确保语料目录存在 */
    system("mkdir -p /tmp/pm_corpus 2>/dev/null");

    MasterTopology* topo = master_topology_create(16);
    master_add_sub_topology(topo, TOPO_VOCABULARY,"词汇拓扑",50000,10);
    master_add_sub_topology(topo, TOPO_SEMANTIC, "语义拓扑", 8000, 9);
    master_add_sub_topology(topo, TOPO_EMOTION,  "情绪拓扑", 2000, 8);
    master_add_sub_topology(topo, TOPO_SYNTAX,   "语法拓扑", 2000, 7);
    master_add_sub_topology(topo, TOPO_CONTEXT,  "上下文拓扑",2000,6);
    master_add_sub_topology(topo, TOPO_DOMAIN,   "领域拓扑", 2000, 5);
    master_add_sub_topology(topo, TOPO_PRAGMA,   "语用拓扑", 2000, 4);
    master_add_sub_topology(topo, TOPO_CULTURE,  "文化拓扑", 2000, 3);
    master_add_sub_topology(topo, TOPO_CONCEPT,  "概念拓扑", 12000,9);
    master_add_sub_topology(topo, TOPO_MASTER,   "主拓扑",   100,  0);

    MemorySystem* mem = memory_system_create(128, 512, 2048);
    AutonomicState astate;
    memset(&astate, 0, sizeof(astate));
    autonomic_state_init(&astate);

    char* seen_urls[2048];
    int seen_count = 0;

    long total_lines = 0, total_articles = 0, total_pairs = 0;

    /* 减少每 query 抓取文章数，优先质量好的站点 */
    #define MAX_ARTICLES_PER_QUERY 3

    for (int rd = 0; rd < rounds; rd++) {
        printf("\n--- Round %d/%d ---\n", rd+1, rounds);
        for (int qi = 0; qi < qc; qi++) {
            const char* q = qs[qi];
            printf("[%ld] '%s'\n", qi + 1L, q);

            /* Bing RSS 搜索 */
            char encoded[512];
            url_encode(q, encoded, sizeof(encoded));
            char rss_url[1024];
            snprintf(rss_url, sizeof(rss_url),
                     "https://cn.bing.com/search?q=%s&format=rss&count=10", encoded);

            char* rss_xml = curl_fetch(rss_url, 12);
            if (!rss_xml) { printf("  RSS fetch failed\n"); sleep(delay_s); continue; }

            /* 解析 RSS items */
            int rss_count = 0;
            const char* ptr = rss_xml;
            while ((ptr = strstr(ptr, "<item>")) != NULL && rss_count < 10) {
                ptr += 6;
                const char* item_end = strstr(ptr, "</item>");
                if (!item_end) break;
                int item_len = item_end - ptr;
                char item_xml[4096];
                int copy_len = item_len < 4095 ? item_len : 4095;
                memcpy(item_xml, ptr, copy_len);
                item_xml[copy_len] = '\0';

                char* title = rss_extract_title(item_xml);
                char* link = rss_extract_link(item_xml);

                if (title && link) {
                    rss_count++;
                    printf("  [%d] %s\n", rss_count, title);

                    if (fetch_articles && !url_seen(seen_urls, seen_count, link)
                        && url_is_ok(link)
                        && total_articles < (rd * qc + qi + 1) * MAX_ARTICLES_PER_QUERY / 2 + MAX_ARTICLES_PER_QUERY) {
                        if (seen_count < 2048)
                            seen_urls[seen_count++] = strdup(link);

                        printf("    fetching(%s) ...", link); fflush(stdout);
                        time_t ft0 = time(NULL);
                        char* article_html = curl_fetch(link, 8);
                        time_t ft1 = time(NULL);
                        printf(" done(%lds)\n", (long)(ft1-ft0)); fflush(stdout);
                        if (article_html) {
                            printf("    text_extract..."); fflush(stdout);
                            char* text = html_to_text(article_html, 65536);
                            printf(" done\n"); fflush(stdout);
                            free(article_html);
                            if (text) {
                                int tlen = strlen(text);
                                if (tlen > 100) {
                                    /* 保存纯文本到语料目录 */
                                    char txtpath[512];
                                    snprintf(txtpath, sizeof(txtpath),
                                             "/tmp/pm_corpus/%05ld_%s.txt",
                                             total_articles, title ? title : "article");
                                    FILE* tf = fopen(txtpath, "w");
                                    if (tf) { fputs(text, tf); fclose(tf); }
                                    /* 通过 article_reader PMI 管线做词发现 */
                                    printf("    ar_create..."); fflush(stdout);
                                    ArticleReaderConfig ar_cfg = {
                                        .window_size = 2, .pmi_threshold = 1.5f,
                                        .min_freq = 2, .alpha = 0.4f, .beta = 0.4f,
                                        .gamma = 0.2f, .batch_size = 50, .verbose = 0
                                    };
                                    ArticleReader* ar = article_reader_create(topo, &ar_cfg);
                                    if (ar) {
                                        printf(" lines..."); fflush(stdout);
                                        char* line = strtok(text, "\n");
                                        while (line) {
                                            article_process_line(ar, line);
                                            line = strtok(NULL, "\n");
                                        }
                                        printf(" flush..."); fflush(stdout);
                                        article_flush(ar, NULL);
                                        printf(" destroy..."); fflush(stdout);
                                        article_reader_destroy(ar);
                                    }
                                    printf(" qa..."); fflush(stdout);
                                    /* QA 提取：只提取真实 QA 对，不把文章标题当伪 QA */
                                    char qs_buf[8][512], as_buf[8][2048];
                                    int pc = perception_extract_qa_pairs(text, qs_buf, as_buf, 8);
                                    for (int pi = 0; pi < pc; pi++) {
                                        printf(" learn[%d]...", pi); fflush(stdout);
                                        autonomic_learn_from_dialog(topo, qs_buf[pi],
                                            as_buf[pi], &astate, NULL, mem);
                                        dream_enqueue_qa(qs_buf[pi], as_buf[pi]);
                                    }
                                    total_pairs += pc;
                                    total_lines += tlen;
                                    total_articles++;
                                    printf(" done\n"); fflush(stdout);
                                    printf(" -> %d chars, %d learns (%lds)\n", tlen, pc, (long)(ft1-ft0));
                                }
                                free(text);
                            }
                        }
                    }
                }

                free(title);
                free(link);
                ptr = item_end + 7;
            }
            free(rss_xml);
            printf("  RSS: %d results | total: %ld articles, %ld chars, %ld pairs\n",
                   rss_count, total_articles, total_lines, total_pairs);

            if ((qi + 1) % save_every == 0) {
                int nodes = master_save_state(topo, "pivotmind_state.dat");
                printf("  [saved %d nodes]\n", nodes);
            }

            sleep(delay_s);
        }
    }

    autonomic_state_destroy(&astate);
    int nodes = master_save_state(topo, "pivotmind_state.dat");
    printf("\n=== Done: %ld articles, %ld chars, %ld QA pairs, %d nodes ===\n",
           total_articles, total_lines, total_pairs, nodes);

    memory_system_destroy(mem);
    master_topology_destroy(topo);
    for (int i = 0; i < cc; i++) free(custom[i]);
    for (int i = 0; i < seen_count; i++) free(seen_urls[i]);
    return 0;
}
