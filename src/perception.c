/**
 * @file perception.c
 * @brief 感觉皮层实现 — 好奇心驱动自主搜索学习
 */

#include "perception.h"
#include "web_search.h"
#include "web_fetch.h"
#include "active_learner.h"
#include "autonomic_learner.h"
#include "article_reader.h"
#include "huarong_topology.h"
#include "node_hash.h"
#include "topology_growth.h"
#include "template_builder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

/* 全局感觉皮层指针 — 供 self_learner / dialog_system 等模块通过 extern 引用 */
Perception* g_perception = NULL;

/* ========== 多搜索引擎定义（SearXNG 启发） ========== */
static SearchEngine g_default_engines[] = {
    /* 搜狗微信搜索 — 微信公众号文章，信息密度高，被反爬概率低 */
    {"sogou_wx",  "https://weixin.sogou.com/weixin?type=2&query=%s",  5000, 1.0f, 5000, 10},
    /* 搜狗移动搜索 — 手机版搜狗，HTML 比 PC 版简单 */
    {"sogou_m",   "https://m.sogou.com/web/searchList.jsp?keyword=%s", 5000, 0.8f, 3000, 15},
    /* 搜狗 Web 搜索 — 原始 PC 版（保留兼容） */
    {"sogou_web", "https://www.sogou.com/sie?ie=utf-8&query=%s",       5000, 0.5f, 2000, 10},
    /* Bing CN — 需 302 重定向 */
    {"bing_cn",   "https://cn.bing.com/search?q=%s",                   8000, 0.6f, 5000, 10},
    /* 百度移动搜索 — 国内覆盖率最高 */
    {"baidu_m",   "https://m.baidu.com/s?word=%s",                     6000, 0.7f, 5000, 10},
};
#define PM_ENGINE_COUNT (int)(sizeof(g_default_engines) / sizeof(g_default_engines[0]))

/* 旧 Provider（兼容迁移） */
#define PROV_SOGOU   0
#define PROV_BING    1
#define PROV_SOGOU2  2
#define PROV_COUNT   3
#define PROVIDER_FAIL_MAX   3
#define PROVIDER_COOLDOWN_TICKS  300

/* 搜索缓存最大条目 */
#define CACHE_MAX_ENTRIES  256
/* 最大拼接文本长度 */
#define MAX_SEARCH_TEXT 65536

/* 简单本地 RNG */
static unsigned int _perception_rand(unsigned int* seed) {
    *seed = *seed * 1103515245 + 12345;
    return (*seed >> 16) & 0x7FFF;
}

/* URL 编码（仅中文和特殊字符） */
static int _url_encode(const char* src, char* dst, int dst_sz) {
    static const char hex[] = "0123456789ABCDEF";
    int j = 0;
    for (int i = 0; src[i] && j < dst_sz - 4; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[j++] = (char)c;
        } else if (c == ' ') {
            dst[j++] = '+';
        } else {
            dst[j++] = '%';
            dst[j++] = hex[c >> 4];
            dst[j++] = hex[c & 0x0F];
        }
    }
    dst[j] = '\0';
    return j;
}

/* ================================================================
 *  Unicode 转义序列解码 — 将 "\uXXXX" → UTF-8 字符
 *  修复概念名存了 "u4eca" 等未解码转义序列的问题
 * ================================================================ */
static void _decode_unicode_escapes(char* str) {
    if (!str) return;
    char* src = str;
    char* dst = str;
    while (*src) {
        /* 分支 0: uXXXX（无反斜杠，概念名存储损坏补丁）
         * 只在解码结果为非 ASCII 时才执行，避免误伤 "usage" 等英文 */
        if (src[0] == 'u' &&
            isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2]) &&
            isxdigit((unsigned char)src[3]) && isxdigit((unsigned char)src[4])) {
            unsigned int cp = 0;
            for (int i = 1; i <= 4; i++) {
                char c = src[i];
                cp = cp * 16 + (unsigned int)(c >= '0' && c <= '9' ? c - '0' :
                                              c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                                              c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
            }
            if (cp >= 0x80) {  /* 非 ASCII 才解码，避免误伤英文单词 */
                if (cp < 0x800) {
                    *dst++ = (char)(0xC0 | (cp >> 6));
                    *dst++ = (char)(0x80 | (cp & 0x3F));
                } else {
                    *dst++ = (char)(0xE0 | (cp >> 12));
                    *dst++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                    *dst++ = (char)(0x80 | (cp & 0x3F));
                }
                src += 5;
                continue;
            }
            /* cp < 0x80: 可能是巧合的英文，跳过不解码 */
        }
        /* 分支 1: \uXXXX */
        if (src[0] == '\\' && src[1] == 'u' &&
            isxdigit((unsigned char)src[2]) && isxdigit((unsigned char)src[3]) &&
            isxdigit((unsigned char)src[4]) && isxdigit((unsigned char)src[5])) {
            unsigned int cp = 0;
            for (int i = 2; i <= 5; i++) {
                char c = src[i];
                cp = cp * 16 + (unsigned int)(c >= '0' && c <= '9' ? c - '0' :
                                              c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                                              c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
            }
            if (cp < 0x80) {
                *dst++ = (char)cp;
            } else if (cp < 0x800) {
                *dst++ = (char)(0xC0 | (cp >> 6));
                *dst++ = (char)(0x80 | (cp & 0x3F));
            } else {
                *dst++ = (char)(0xE0 | (cp >> 12));
                *dst++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                *dst++ = (char)(0x80 | (cp & 0x3F));
            }
            src += 6;
        } else if (src[0] == '\\' && src[1] == 'x' &&
                   isxdigit((unsigned char)src[2]) && isxdigit((unsigned char)src[3])) {
            /* 分支 2: \xXX */
            unsigned int cp = 0;
            for (int i = 2; i <= 3; i++) {
                char c = src[i];
                cp = cp * 16 + (unsigned int)(c >= '0' && c <= '9' ? c - '0' :
                                              c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                                              c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
            }
            *dst++ = (char)cp;
            src += 4;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* ================================================================
 *  中文分句 — 将句号/感叹号/问号/分号替换为换行
 *  使 article_reader 以句子粒度处理，提升 PMI 词发现精度
 * ================================================================ */
static void _normalize_sentence_breaks(char* text) {
    if (!text) return;
    /* UTF-8 编码的中文标点 */
    static const char* terminators[] = {
        "\xE3\x80\x82",  /* 。 */
        "\xEF\xBC\x81",  /* ！ */
        "\xEF\xBC\x9F",  /* ？ */
        "\xEF\xBC\x9B",  /* ； */
        NULL
    };
    for (int t = 0; terminators[t]; t++) {
        char* pos = text;
        const char* term = terminators[t];
        int term_len = (int)strlen(term);
        while ((pos = strstr(pos, term)) != NULL) {
            *pos = '\n';
            memmove(pos + 1, pos + term_len, strlen(pos + term_len) + 1);
            pos++;
        }
    }
}

/* ================================================================ */

Perception* perception_create(MasterTopology* topology,
                               MemorySystem* memory,
                               ActiveLearner* learner,
                               PerceptionConfig* cfg) {
    if (!topology || !memory || !learner) return NULL;

    Perception* p = (Perception*)calloc(1, sizeof(Perception));
    if (!p) return NULL;

    pthread_mutex_init(&p->mutex, NULL);

    p->topology = topology;
    p->memory   = memory;
    p->learner  = learner;

    if (cfg) p->cfg = *cfg;
    else p->cfg = (PerceptionConfig)PERCEPTION_DEFAULT_CONFIG;

    /* 创建文章阅读器 — 搜索结果语义理解管线 */
    ArticleReaderConfig ar_cfg = ARTICLE_READER_DEFAULT_CONFIG;
    ar_cfg.batch_size = 50;   /* 搜索结果较短，降低 batch */
    ar_cfg.verbose    = p->cfg.verbose;
    p->ar = article_reader_create(topology, &ar_cfg);
    if (!p->ar) {
        free(p);
        return NULL;
    }

    /* 初始化缓存 */
    p->cache_head = NULL;
    p->cache_size = 0;
    p->cache_max  = CACHE_MAX_ENTRIES;

    /* 设置全局指针供其他模块引用 */
    g_perception = p;

    /* 初始化多搜索引擎 */
    p->engine_count = PM_ENGINE_COUNT;
    memcpy(p->engines, g_default_engines, sizeof(g_default_engines));
    p->day_reset_time = time(NULL);

    printf("[感觉皮层] 就绪 (%d个搜索引擎, 最大%d个/周期, 超时%dms, 缓存%ds)\n",
           PM_ENGINE_COUNT, p->cfg.max_searches_per_cycle, p->cfg.search_timeout_ms,
           p->cfg.cache_ttl_seconds);
    return p;
}

void perception_destroy(Perception* p) {
    if (!p) return;

    /* 尾部 flush：将未 flush 的搜索文本最后处理一次 */
    if (p->ar && p->article_accum_count > 0) {
        int final_words = article_flush(p->ar, NULL);
        if (p->cfg.verbose && final_words > 0)
            fprintf(stderr, "[感觉皮层] 销毁前尾部 flush: +%d 新词\n", final_words);
        p->article_accum_count = 0;
    }

    /* 清空缓存 */
    SearchCacheEntry* ce = p->cache_head;
    while (ce) {
        SearchCacheEntry* next = ce->next;
        free(ce->query);
        free(ce);
        ce = next;
    }
    /* 销毁文章阅读器 */
    if (p->ar) article_reader_destroy(p->ar);
    if (g_perception == p) g_perception = NULL;
    pthread_mutex_destroy(&p->mutex);
    free(p);
}

/* ================================================================
 *  搜索缓存
 * ================================================================ */

/** 查询缓存，返回时间戳（0=未命中） */
static time_t _cache_lookup(Perception* p, const char* query) {
    if (!p || !query) return 0;
    SearchCacheEntry* ce = p->cache_head;
    while (ce) {
        if (ce->query && strcmp(ce->query, query) == 0) {
            return ce->timestamp;
        }
        ce = ce->next;
    }
    return 0;
}

/** 插入缓存条目（LRU 淘汰） */
static void _cache_insert(Perception* p, const char* query, int result_count) {
    if (!p || !query) return;

    /* 如果已存在，更新时间戳 */
    SearchCacheEntry* ce = p->cache_head;
    while (ce) {
        if (ce->query && strcmp(ce->query, query) == 0) {
            ce->timestamp = time(NULL);
            ce->result_count = result_count;
            return;
        }
        ce = ce->next;
    }

    /* 淘汰最旧条目 */
    while (p->cache_size >= p->cache_max && p->cache_head) {
        SearchCacheEntry* prev = NULL;
        SearchCacheEntry* oldest = p->cache_head;
        SearchCacheEntry* oprev = NULL;
        ce = p->cache_head;
        while (ce) {
            if (ce->timestamp < oldest->timestamp) {
                oldest = ce;
                oprev = prev;
            }
            prev = ce;
            ce = ce->next;
        }
        if (oprev) oprev->next = oldest->next;
        else p->cache_head = oldest->next;
        free(oldest->query);
        free(oldest);
        p->cache_size--;
    }

    /* 插入到头部 */
    SearchCacheEntry* entry = (SearchCacheEntry*)calloc(1, sizeof(SearchCacheEntry));
    if (!entry) return;
    entry->query = strdup(query);
    entry->timestamp = time(NULL);
    entry->result_count = result_count;
    entry->next = p->cache_head;
    p->cache_head = entry;
    p->cache_size++;
}

/* ================================================================
 *  Provider 管理
 * ================================================================ */

/** 检查 provider 是否可用 */
static int _provider_available(Perception* p, int prov_idx) {
    if (prov_idx < 0 || prov_idx >= PROV_COUNT) return 0;
    if (p->provider_failures[prov_idx] >= PROVIDER_FAIL_MAX) {
        /* 在冷却期 */
        if (p->tick_counter < p->provider_cooldown[prov_idx]) return 0;
        /* 冷却期满，重置 */
        p->provider_failures[prov_idx] = 0;
    }
    return 1;
}

/** 记录 provider 结果 */
static void _provider_result(Perception* p, int prov_idx, int success) {
    if (prov_idx < 0 || prov_idx >= PROV_COUNT) return;
    if (success) {
        p->provider_failures[prov_idx] = 0;
    } else {
        p->provider_failures[prov_idx]++;
        if (p->provider_failures[prov_idx] >= PROVIDER_FAIL_MAX) {
            p->provider_cooldown[prov_idx] = p->tick_counter + PROVIDER_COOLDOWN_TICKS;
            if (p->cfg.verbose) {
                const char* names[] = {"搜狗搜索", "Bing搜索", "搜狗备选"};
                fprintf(stderr, "[感觉皮层] Provider '%s' 进入冷却 (%d ticks)\n",
                        names[prov_idx], PROVIDER_COOLDOWN_TICKS);
            }
        }
    }
}

/** 根据响应码触发熔断 — 对接 web_fetch 响应码分类 */
static void _provider_check_http_status(Perception* p, int prov_idx, WebResult* wr) {
    if (!wr || !p) return;

    /* 永久封禁 403/451 → 加长冷却 */
    if (web_fetch_is_permanent_block(wr->status_code)) {
        p->provider_failures[prov_idx] = PROVIDER_FAIL_MAX;
        p->provider_cooldown[prov_idx] = p->tick_counter + PROVIDER_COOLDOWN_TICKS * 5;
        if (p->cfg.verbose) {
            const char* names[] = {"搜狗搜索", "Bing搜索", "搜狗备选"};
            fprintf(stderr, "[感觉皮层] Provider '%s' HTTP %d 永久封禁，冷却延长\n",
                    names[prov_idx], wr->status_code);
        }
        return;
    }

    /* 限速 429 → 加大延迟 */
    if (web_fetch_is_rate_limit(wr->status_code)) {
        p->provider_cooldown[prov_idx] = p->tick_counter + PROVIDER_COOLDOWN_TICKS * 2;
        if (p->cfg.verbose) {
            const char* names[] = {"搜狗搜索", "Bing搜索", "搜狗备选"};
            fprintf(stderr, "[感觉皮层] Provider '%s' HTTP 429 限速，冷却延长\n",
                    names[prov_idx]);
        }
    }
}

/* ================================================================
 *  核心搜索+学习循环（重写：多源抓取 → article_reader 管线）
 * ================================================================ */

/**
 * 单次搜索：尝试某个 provider
 * @return WebResult* 成功，NULL 失败
 */
static WebResult* _try_provider(Perception* p, int prov_idx,
                                const char* encoded_query) {
    /* 映射旧 provider 索引到引擎数组 */
    static const int prov_to_eng[] = {2, 3, 2};  /* SOGOU→sogou_web[2], BING→bing_cn[3], SOGOU2→sogou_web[2] */
    if (prov_idx < 0 || prov_idx >= 3) return NULL;
    int eng_idx = prov_to_eng[prov_idx];
    if (eng_idx >= p->engine_count) return NULL;

    char url[1024];
    snprintf(url, sizeof(url), p->engines[eng_idx].url_fmt, encoded_query);

    if (p->cfg.verbose)
        fprintf(stderr, "[感觉皮层] 请求: %s\n", url);

    WebResult* wr = web_search(url, p->cfg.search_timeout_ms, 32768);
    return wr;
}

/**
 * 搜索一个概念并将结果喂入 article_reader 语义理解管线
 * @return 1=成功学到知识，0=无结果/已缓存，-1=失败
 */
static int search_and_learn(Perception* p, const char* concept, PerceptionSource source) {
    if (!concept || strlen(concept) < 2) return 0;
    if (!p->ar) return -1;

    /* 解码 Unicode 转义序列（修复概念名存了 "u4eca" 等问题） */
    char clean_concept[256];
    snprintf(clean_concept, sizeof(clean_concept), "%s", concept);
    _decode_unicode_escapes(clean_concept);

    /* 检查缓存（用解码后的概念名） */
    time_t cached = _cache_lookup(p, clean_concept);
    time_t now = time(NULL);
    if (cached > 0 && (now - cached) < p->cfg.cache_ttl_seconds) {
        if (p->cfg.verbose)
            fprintf(stderr, "[感觉皮层] '%s' 已缓存，跳过\n", clean_concept);
        return 0;
    }

    /* URL 编码 */
    char encoded[256];
    _url_encode(clean_concept, encoded, sizeof(encoded));

    /* 多源尝试拼接 */
    char* merged_text = (char*)calloc(MAX_SEARCH_TEXT, 1);
    if (!merged_text) return -1;
    int merged_len = 0;
    int sources_used = 0;

    /* Provider 优先级: 搜狗 → Bing → 搜狗备选 */
    int prov_priority[PROV_COUNT] = { PROV_SOGOU, PROV_BING, PROV_SOGOU2 };
    const char* prov_names[] = { "搜狗搜索", "Bing搜索", "搜狗备选" };

    if (p->cfg.verbose) {
        const char* src_names[] = {"好奇", "巩固", "对话", "空闲"};
        fprintf(stderr, "[感觉皮层] %s探索: '%s'\n", src_names[source], clean_concept);
    }

    for (int pi = 0; pi < PROV_COUNT && sources_used < 2; pi++) {
        int prov = prov_priority[pi];
        if (!_provider_available(p, prov)) {
            if (p->cfg.verbose)
                fprintf(stderr, "[感觉皮层]   %s 冷却中，跳过\n", prov_names[prov]);
            continue;
        }

        WebResult* wr = _try_provider(p, prov, encoded);
        if (!wr || (wr->status_code != 200 && wr->status_code != 0) || !wr->body) {
            if (wr) {
                _provider_check_http_status(p, prov, wr);
                web_result_free(wr);
            } else {
                _provider_result(p, prov, 0);
            }
            if (p->cfg.verbose)
                fprintf(stderr, "[感觉皮层]   %s 无响应\n", prov_names[prov]);
            continue;
        }

        /* 检查是否需要熔断 */
        _provider_check_http_status(p, prov, wr);

        _provider_result(p, prov, 1);

        /* 提取文本 */
        int body_len = wr->body_len;
        if (body_len > 16384) body_len = 16384;
        char* text = (char*)malloc(body_len + 1);
        if (!text) { web_result_free(wr); continue; }

        int text_len = web_extract_text(wr->body, text, body_len);
        text[text_len] = '\0';
        web_result_free(wr);

        if (text_len < 10) { free(text); continue; }

        /* 拼接：加来源标注 */
        int space_left = MAX_SEARCH_TEXT - merged_len - 128;
        if (space_left > 0) {
            int src_line = snprintf(merged_text + merged_len, space_left,
                                    "  %s\n", clean_concept);
            int copy_len = (text_len < space_left - src_line) ?
                           text_len : (space_left - src_line);
            memcpy(merged_text + merged_len + src_line, text, copy_len);
            merged_len += src_line + copy_len;
            merged_text[merged_len] = '\n';
            merged_text[merged_len + 1] = '\0';
            merged_len++;
        }
        free(text);
        sources_used++;
    }

    p->total_searches++;

    if (merged_len < 10 || sources_used == 0) {
        free(merged_text);
        _cache_insert(p, clean_concept, 0);
        return 0;
    }

    /* === 核心改动：搜索结果 → article_reader 语义理解管线 === */

    /* 概念上下文：作为标题先行注入，帮助 article_reader 建立主题锚点 */
    {
        char title_line[320];
        snprintf(title_line, sizeof(title_line), "搜索概念：%s", clean_concept);
        article_process_line(p->ar, title_line);
    }

    /* 中文分句：将 。！？； 替换为 \n，使 article_reader 获得句子级粒度 */
    _normalize_sentence_breaks(merged_text);

    /* 分行送入 article_reader */
    char* line_ctx = NULL;
#ifdef _WIN32
    char* line = strtok_s(merged_text, "\n", &line_ctx);
#else
    char* line = strtok_r(merged_text, "\n", &line_ctx);
#endif
    int lines_processed = 0;
    while (line) {
        /* 跳过空行、纯空白行、过短行 */
        int len = (int)strlen(line);
        int has_content = 0;
        for (int i = 0; i < len; i++) {
            if ((unsigned char)line[i] > 32) { has_content = 1; break; }
        }
        if (has_content && len > 2) {
            article_process_line(p->ar, line);
            lines_processed++;
        }
#ifdef _WIN32
        line = strtok_s(NULL, "\n", &line_ctx);
#else
        line = strtok_r(NULL, "\n", &line_ctx);
#endif
    }

    /* 将搜索结果文本同步喂入 auto_learn_concepts（组合节点+PMI建边） */
    extern void auto_learn_concepts(struct MasterTopology*, const char*, void*);
    auto_learn_concepts(p->topology, merged_text, NULL);

    free(merged_text);

    /* 累积一定数量后触发 flush */
    p->article_accum_count++;
    int new_words = 0;
    if (p->article_accum_count >= p->cfg.article_flush_interval) {
        new_words = article_flush(p->ar, NULL);
        p->article_accum_count = 0;

        if (p->cfg.verbose && new_words > 0)
            fprintf(stderr, "[感觉皮层] '%s' → article_flush: +%d 新词\n",
                    clean_concept, new_words);
    }

    /* 也通过海马体学习（保持双向通路） */
    if (lines_processed > 0)
        learn_from_dialog(p->learner, concept, "", "");

    p->total_concepts_learned++;
    p->total_new_connections += (new_words > 0 ? new_words : 1);

    _cache_insert(p, concept, new_words);

    return (new_words > 0) ? new_words : 1;
}

/* ================================================================
 *  三维度知识缺口检测（替代原好奇心随机采样）
 * ================================================================ */

/**
 * 维度 1：对话缺口 — 从海马体获取用户最近提到的未知概念
 */
__attribute__((unused))
static int _gap_dialog_queries(Perception* p, const char** out_queries,
                               int max_n) {
    /* 对话缺口：词汇拓扑中最近激活但低置信度的节点 */
    /* 这表明系统在对话中遇到了这个词但不理解它 */
    if (!p->topology) return 0;

    SubTopology* vocab = master_get_sub_topology_by_type(p->topology, TOPO_VOCABULARY);
    if (!vocab || !vocab->net) return 0;

    int count = 0;
    for (int i = 0; i < vocab->net->node_count && count < max_n; i++) {
        ReasoningNode* node = vocab->net->nodes[i];
        if (!node || !node->concept) continue;
        /* 最近被激活过 (activation > 0.3) + 低置信度 = 对话中遇到但没理解 */
        if (node->activation > 0.3f && node->confidence < 0.25f) {
            /* 检查是否在缓存中 */
            if (_cache_lookup(p, node->concept) == 0) {
                out_queries[count++] = node->concept;
            }
        }
    }
    return count;
}

/**
 * 维度 2：模板缺口 — POS 模式缺失对应模板
 */
__attribute__((unused))
static int _gap_template_queries(Perception* p, const char** out_queries,
                                 int max_n) {
    if (!p->topology || !p->topology->use_template_voting) return 0;

    /* 找词汇拓扑中的高频词（可能代表重要但未模板化的概念） */
    SubTopology* vocab = master_get_sub_topology_by_type(p->topology, TOPO_VOCABULARY);
    if (!vocab || !vocab->net) return 0;

    int count = 0;
    for (int i = 0; i < vocab->net->node_count && count < max_n; i++) {
        ReasoningNode* node = vocab->net->nodes[i];
        if (!node || !node->concept) continue;

        /* 高频但低边数 → 可能是重要概念但缺少上下文 */
        if (node->heat > 0.5f && node->edge_count < 5 && node->confidence < 0.4f) {
            if (_cache_lookup(p, node->concept) == 0) {
                out_queries[count++] = node->concept;
            }
        }
    }
    return count;
}

/**
 * 维度 3：拓扑缺口 — 词汇孤岛（低连接 + 低置信度）
 */
__attribute__((unused))
static int _gap_topology_queries(Perception* p, const char** out_queries,
                                 int max_n) {
    SubTopology* vocab = master_get_sub_topology_by_type(p->topology, TOPO_VOCABULARY);
    if (!vocab || !vocab->net) return 0;

    int count = 0;
    for (int i = 0; i < vocab->net->node_count && count < max_n; i++) {
        ReasoningNode* node = vocab->net->nodes[i];
        if (!node || !node->concept || node->is_cooled) continue;

        /* 孤岛判定 */
        if (node->edge_count < p->cfg.topo_gap_edge_threshold &&
            node->confidence < p->cfg.min_confidence_for_search) {
            if (_cache_lookup(p, node->concept) == 0) {
                out_queries[count++] = node->concept;
            }
        }
    }
    return count;
}

/* ================================================================
 *  tick 入口（重写：三维度知识缺口驱动搜索）
 * ================================================================ */

int perception_tick(Perception* p, float throttle) {
    (void)throttle;  /* 纯随机模式不再需要 throttle 抽签 */
    if (!p) return 0;

    pthread_mutex_lock(&p->mutex);

    p->tick_counter++;

    /* 按配置的周期（默认60秒）触发一次纯随机搜索 */
    if (p->tick_counter < p->cfg.cycle_interval_ticks) { pthread_mutex_unlock(&p->mutex); return 0; }
    p->tick_counter = 0;

    /* 从词汇拓扑中随机选节点进行搜索 */
    SubTopology* vocab = master_get_sub_topology_by_type(p->topology, TOPO_VOCABULARY);
    if (!vocab || !vocab->net || vocab->net->node_count == 0) { pthread_mutex_unlock(&p->mutex); return 0; }

    int max_searches = p->cfg.max_searches_per_cycle;
    if (max_searches < 1) max_searches = 1;
    if (max_searches > 5) max_searches = 5;

    int searched = 0;
    unsigned int rng = (unsigned int)time(NULL) ^ (unsigned int)(uintptr_t)p;
    for (int i = 0; i < max_searches && searched < max_searches; i++) {
        int idx = _perception_rand(&rng) % vocab->net->node_count;
        ReasoningNode* node = vocab->net->nodes[idx];
        if (!node || !node->concept || strlen(node->concept) < 2) continue;

        if (search_and_learn(p, node->concept, PERCEPT_CURIOSITY) >= 0) {
            searched++;
        }
    }

    pthread_mutex_unlock(&p->mutex);
    return searched;
}

/* ================================================================ */

/* ── 带锁包装的公共 API ── */

static int _perception_learn_concept_locked(Perception* p, const char* concept) {
    return search_and_learn(p, concept, PERCEPT_DIALOG);
}

static int _perception_consolidate_node_locked(Perception* p, int node_id) {
    SubTopology* vocab = master_get_sub_topology_by_type(p->topology, TOPO_VOCABULARY);
    if (!vocab || !vocab->net || node_id < 0 || node_id >= vocab->net->node_count) return -1;

    ReasoningNode* node = vocab->net->nodes[node_id];
    if (!node || !node->concept) return -1;

    return search_and_learn(p, node->concept, PERCEPT_CONSOLIDATE);
}

int perception_learn_concept(Perception* p, const char* concept) {
    if (!p || !concept) return -1;
    pthread_mutex_lock(&p->mutex);
    int r = _perception_learn_concept_locked(p, concept);
    pthread_mutex_unlock(&p->mutex);
    return r;
}

int perception_consolidate_node(Perception* p, int node_id) {
    if (!p) return -1;
    pthread_mutex_lock(&p->mutex);
    int r = _perception_consolidate_node_locked(p, node_id);
    pthread_mutex_unlock(&p->mutex);
    return r;
}

/**
 * 批量提交对话缺口查询
 */
int perception_suggest_queries(Perception* p, const char** queries) {
    if (!p || !queries) return 0;
    pthread_mutex_lock(&p->mutex);
    int learned = 0;
    for (int i = 0; queries[i] != NULL; i++) {
        if (search_and_learn(p, queries[i], PERCEPT_DIALOG) > 0)
            learned++;
    }
    pthread_mutex_unlock(&p->mutex);
    return learned;
}

/**
 * 定时新闻搜索 — 每小时搜新闻头条（走搜狗，搜索结果已含新闻）
 * 不同时段搜不同关键词，保持对现实世界的感知
 */
int perception_search_news(Perception* p) {
    if (!p || !p->ar) return 0;

    pthread_mutex_lock(&p->mutex);

    /* 根据当前小时选择新闻主题 */
    time_t now = time(NULL);
    struct tm tm_info;
#ifdef _WIN32
    localtime_s(&tm_info, &now);
#else
    localtime_r(&now, &tm_info);
#endif
    int hour = tm_info.tm_hour;

    const char* topic;
    if (hour >= 6 && hour < 10)       topic = "科技";
    else if (hour >= 10 && hour < 14) topic = "财经";
    else if (hour >= 14 && hour < 18) topic = "国际";
    else if (hour >= 18 && hour < 22) topic = "文化";
    else                              topic = "科技";  /* 夜间默认 */

    if (p->cfg.verbose)
        fprintf(stderr, "[感觉皮层] 📰 实时新闻搜索: '%s'\n", topic);

    /* 编码查询 */
    char encoded[256];
    _url_encode(topic, encoded, sizeof(encoded));

    /* 走搜狗搜索（搜索结果已包含新闻，无需独立新闻入口） */
    if (!_provider_available(p, PROV_SOGOU)) {
        if (p->cfg.verbose)
            fprintf(stderr, "[感觉皮层] 搜狗搜索 冷却中，跳过新闻\n");
        pthread_mutex_unlock(&p->mutex);
        return 0;
    }

    WebResult* wr = _try_provider(p, PROV_SOGOU, encoded);
    if (!wr || !wr->body) {
        if (wr) {
            _provider_check_http_status(p, PROV_SOGOU, wr);
            web_result_free(wr);
        } else {
            _provider_result(p, PROV_SOGOU, 0);
        }
        pthread_mutex_unlock(&p->mutex);
        return 0;
    }

    _provider_check_http_status(p, PROV_SOGOU, wr);
    _provider_result(p, PROV_SOGOU, 1);

    /* 提取文本 */
    int body_len = wr->body_len;
    if (body_len > 16384) body_len = 16384;
    char* text = (char*)malloc(body_len + 1);
    if (!text) { web_result_free(wr); pthread_mutex_unlock(&p->mutex); return 0; }

    int text_len = web_extract_text(wr->body, text, body_len);
    text[text_len] = '\0';
    web_result_free(wr);

    if (text_len < 20) { free(text); return 0; }

    /* 概念上下文 */
    char title_line[320];
    snprintf(title_line, sizeof(title_line), "新闻：%s", topic);
    article_process_line(p->ar, title_line);

    /* 分句处理 */
    _normalize_sentence_breaks(text);

    char* line_ctx = NULL;
    int lines_processed = 0;
#ifdef _WIN32
    char* line = strtok_s(text, "\n", &line_ctx);
#else
    char* line = strtok_r(text, "\n", &line_ctx);
#endif
    while (line) {
        int len = (int)strlen(line);
        int has_content = 0;
        for (int i = 0; i < len; i++) {
            if ((unsigned char)line[i] > 32) { has_content = 1; break; }
        }
        if (has_content && len > 2) {
            article_process_line(p->ar, line);
            lines_processed++;
        }
#ifdef _WIN32
        line = strtok_s(NULL, "\n", &line_ctx);
#else
        line = strtok_r(NULL, "\n", &line_ctx);
#endif
    }
    free(text);

    /* 累积并可能 flush */
    p->article_accum_count++;
    int new_words = 0;
    if (p->article_accum_count >= p->cfg.article_flush_interval) {
        new_words = article_flush(p->ar, NULL);
        p->article_accum_count = 0;
    }

    p->total_searches++;
    p->total_concepts_learned++;
    if (lines_processed > 0)
        learn_from_dialog(p->learner, topic, "", "");

    if (p->cfg.verbose)
        fprintf(stderr, "[感觉皮层]  新闻 '%s' 处理 %d 行, +%d 新词\n",
                topic, lines_processed, new_words);

    pthread_mutex_unlock(&p->mutex);
    return (new_words > 0) ? new_words : 1;
}

void perception_stats(Perception* p, long* searches, long* learned, long* new_conns) {
    if (!p) return;
    pthread_mutex_lock(&p->mutex);
    if (searches) *searches = p->total_searches;
    if (learned)  *learned  = p->total_concepts_learned;
    if (new_conns) *new_conns = p->total_new_connections;
    pthread_mutex_unlock(&p->mutex);
}

/* ================================================================
 *  用户驱动的联网搜索 API — 并行多引擎查询
 * ================================================================ */

/* 前向声明：引擎 HTML 解析器 */
static int _parse_sogou_weixin(const char* html, SearchSnippet* out, int max);
static int _parse_sogou_mobile(const char* html, SearchSnippet* out, int max);
static int _parse_baidu_mobile(const char* html, SearchSnippet* out, int max);
static int _parse_bing_html(const char* html, SearchSnippet* out, int max);
static int _parse_sogou_web(const char* html, SearchSnippet* out, int max);
static int _html_extract_text(const char* html, char* out, int max);
static int _dedup_snippets(SearchSnippet* snippets, int count);

/* 引擎健康检查：是否可用 */
static int _engine_available(SearchEngine* eng, int tick) {
    if (!eng || !eng->url_fmt) return 0;
    if (eng->cooldown_until_tick > tick) return 0;

    /* 每日重置 */
    time_t now_t = time(NULL);
    struct tm tm_now;
    localtime_r(&now_t, &tm_now);
    int today = tm_now.tm_yday;
    struct tm tm_last;
    localtime_r(&eng->last_request_time, &tm_last);
    if (today != tm_last.tm_yday) eng->requests_today = 0;

    if (eng->requests_today >= eng->max_requests_per_hour * 24) return 0;
    return 1;
}

/* 引擎查询：构造 URL → web_fetch → 解析 HTML → 提取片段 */
static int _engine_query(SearchEngine* eng, const char* query_encoded,
                         SearchSnippet* out, int max_out) {
    char url[1024];
    snprintf(url, sizeof(url), eng->url_fmt, query_encoded);

    WebResult* wr = web_search(url, eng->timeout_ms, 65536);
    if (!wr || !wr->body) return 0;

    int count = 0;

    /* 根据引擎名选择解析器 */
    if (strstr(eng->name, "sogou_wx") || strstr(eng->name, "weixin")) {
        count = _parse_sogou_weixin(wr->body, out, max_out);
    } else if (strstr(eng->name, "sogou_m") || strstr(eng->name, "m.sogou")) {
        count = _parse_sogou_mobile(wr->body, out, max_out);
    } else if (strstr(eng->name, "baidu_m") || strstr(eng->name, "m.baidu")) {
        count = _parse_baidu_mobile(wr->body, out, max_out);
    } else if (strstr(eng->name, "bing")) {
        count = _parse_bing_html(wr->body, out, max_out);
    } else {
        count = _parse_sogou_web(wr->body, out, max_out);
    }

    /* 记录来源 */
    for (int i = 0; i < count; i++) {
        snprintf(out[i].source, sizeof(out[i].source), "%s", eng->name);
        out[i].score *= eng->quality_weight;
    }

    web_result_free(wr);
    return count;
}

/* 通用 HTML 文本提取 — 从任意 HTML 中抽纯文本 */
static int _html_extract_text(const char* html, char* out, int max) {
    if (!html || !out) return 0;
    int pos = 0;
    int in_tag = 0, in_script = 0, in_style = 0;
    const char* p = html;
    while (*p && pos < max - 1) {
        if (*p == '<') {
            in_tag = 1;
            if (!in_script && strncasecmp(p, "<script", 7) == 0) in_script = 1;
            if (!in_style && strncasecmp(p, "<style", 6) == 0) in_style = 1;
        }
        if (!in_tag && !in_script && !in_style) {
            /* 输出非标签、非脚本、非样式文本 */
            if (*p == '&') {
                /* 简单 HTML 实体解码 */
                if (strncmp(p, "&nbsp;", 6) == 0) { out[pos++] = ' '; p += 5; }
                else if (strncmp(p, "&lt;", 4) == 0) { out[pos++] = '<'; p += 3; }
                else if (strncmp(p, "&gt;", 4) == 0) { out[pos++] = '>'; p += 3; }
                else if (strncmp(p, "&amp;", 5) == 0) { out[pos++] = '&'; p += 4; }
                else if (strncmp(p, "&quot;", 6) == 0) { out[pos++] = '"'; p += 5; }
                else { out[pos++] = *p; }
            } else if (*p == '\n' || *p == '\r' || *p == '\t') {
                if (pos > 0 && out[pos-1] != ' ') out[pos++] = ' ';
            } else if ((unsigned char)*p >= 0x20 || *p == '\n') {
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
    return pos;
}

/* ── 各引擎 HTML 解析器 ── */

/* 搜狗微信搜索解析器 */
static int _parse_sogou_weixin(const char* html, SearchSnippet* out, int max) {
    if (!html || !out) return 0;
    /* 微信公众号搜索结果结构: <h3><a>标题</a></h3> + <p class="txt-info">摘要</p> */
    int count = 0;
    const char* pos = html;
    while (count < max && (pos = strstr(pos, "<h3>"))) {
        const char* a_start = strstr(pos, "<a ");
        if (!a_start) { pos += 4; continue; }
        const char* href = strstr(a_start, "href=\"");
        const char* title_start = strstr(a_start, ">");
        if (!title_start) { pos += 4; continue; }
        title_start++;
        const char* title_end = strstr(title_start, "</a>");
        if (!title_end) { pos += 4; continue; }

        size_t tlen = title_end - title_start;
        if (tlen >= sizeof(out[count].title)) tlen = sizeof(out[count].title) - 1;
        memcpy(out[count].title, title_start, tlen);
        out[count].title[tlen] = '\0';

        /* URL（可选） */
        if (href) {
            href += 6;
            const char* h_end = strchr(href, '"');
            if (h_end) {
                size_t ulen = h_end - href;
                if (ulen >= sizeof(out[count].url)) ulen = sizeof(out[count].url) - 1;
                memcpy(out[count].url, href, ulen);
                out[count].url[ulen] = '\0';
            }
        }

        /* 摘要 */
        const char* snippet = strstr(title_end, "<p class=\"txt-info\">");
        if (snippet) {
            snprintf(out[count].snippet, sizeof(out[count].snippet), "%.800s", snippet + 20);
        } else {
            out[count].snippet[0] = '\0';
        }

        out[count].score = 1.0f;
        count++;
        pos = title_end + 5;
    }
    return count;
}

/* 搜狗移动搜索解析器 */
static int _parse_sogou_mobile(const char* html, SearchSnippet* out, int max) {
    if (!html || !out) return 0;
    /* 简单 HTML 文本提取 + 分词 */
    char text[32768];
    int tlen = _html_extract_text(html, text, sizeof(text));
    if (tlen < 50) return 0;

    /* 按句号/换行切分，取前几条作为摘要 */
    int count = 0;
    char* tok = strtok(text, "\n");
    while (tok && count < max) {
        while (*tok == ' ') tok++;
        if (strlen(tok) > 20) {
            snprintf(out[count].title, sizeof(out[count].title), "%.200s", tok);
            snprintf(out[count].snippet, sizeof(out[count].snippet), "%.800s",
                     tok + (strlen(tok) > 200 ? 200 : 0));
            out[count].score = 0.7f;
            count++;
        }
        tok = strtok(NULL, "\n");
    }
    return count;
}

/* 百度移动搜索解析器 */
static int _parse_baidu_mobile(const char* html, SearchSnippet* out, int max) {
    return _parse_sogou_mobile(html, out, max);  /* 同策略：纯文本提取 */
}

/* Bing 搜索解析器 */
static int _parse_bing_html(const char* html, SearchSnippet* out, int max) {
    if (!html || !out) return 0;
    int count = 0;
    /* 找搜索结果区域 */
    const char* pos = strstr(html, "b_algo");
    if (!pos) pos = strstr(html, "b_results");
    if (!pos) return _parse_sogou_mobile(html, out, max);  /* 回退文本提取 */

    while (count < max && (pos = strstr(pos, "<h2"))) {
        const char* title_start = strstr(pos, ">");
        if (!title_start) { pos += 3; continue; }
        title_start++;
        const char* title_end = strstr(title_start, "</h2>") ? strstr(title_start, "</h2>") : strstr(title_start, "</a>");
        if (!title_end) { pos += 3; continue; }
        size_t tlen = title_end - title_start;
        if (tlen >= sizeof(out[count].title)) tlen = sizeof(out[count].title) - 1;
        memcpy(out[count].title, title_start, tlen);
        out[count].title[tlen] = '\0';

        /* 去除 HTML 标签 */
        for (int c = 0; out[count].title[c]; c++) {
            if (out[count].title[c] == '<') {
                char* endb = strchr(out[count].title + c, '>');
                if (endb) memmove(out[count].title + c, endb + 1, strlen(endb + 1) + 1);
            }
        }

        const char* snippet_start = strstr(title_end, "<p");
        if (snippet_start) {
            snippet_start = strstr(snippet_start, ">") + 1;
            const char* snippet_end = strstr(snippet_start, "</p>");
            if (snippet_end) {
                size_t slen = snippet_end - snippet_start;
                if (slen >= sizeof(out[count].snippet)) slen = sizeof(out[count].snippet) - 1;
                memcpy(out[count].snippet, snippet_start, slen);
                out[count].snippet[slen] = '\0';
            }
        }
        out[count].score = 0.8f;
        count++;
        pos = title_end;
    }
    return count;
}

/* 搜狗 Web 搜索解析器（保留原有逻辑） */
static int _parse_sogou_web(const char* html, SearchSnippet* out, int max) {
    return _parse_sogou_mobile(html, out, max);  /* 同文本提取策略 */
}

/* 搜索结果去重（按标题） */
static int _dedup_snippets(SearchSnippet* snippets, int count) {
    for (int i = 0; i < count; i++) {
        if (!snippets[i].title[0]) continue;
        for (int j = i + 1; j < count; j++) {
            if (!snippets[j].title[0]) continue;
            if (strcmp(snippets[i].title, snippets[j].title) == 0) {
                snippets[j].title[0] = '\0';  /* 标记为删除 */
            }
        }
    }
    int w = 0;
    for (int i = 0; i < count; i++) {
        if (snippets[i].title[0]) {
            if (w != i) snippets[w] = snippets[i];
            w++;
        }
    }
    return w;
}

/**
 * 用户驱动的联网搜索 — 查询多个引擎，返回格式化结果
 */
char* perception_search_for_user(Perception* p, const char* query, int max_len) {
    if (!p || !query || !query[0]) return NULL;
    if (max_len <= 0) max_len = 4096;

    /* URL 编码查询词 */
    char encoded[512];
    _url_encode(query, encoded, sizeof(encoded));

    SearchSnippet all_snips[64];
    int total = 0;
    int now_tick = p->tick_counter;

    pthread_mutex_lock(&p->mutex);

    /* 检查缓存 */
    time_t cached = _cache_lookup(p, query);
    time_t now = time(NULL);
    if (cached > 0 && (now - cached) < p->cfg.cache_ttl_seconds) {
        pthread_mutex_unlock(&p->mutex);
        char* result = malloc(256);
        if (result) snprintf(result, 256, "（该问题最近已搜索过，请稍后再试）");
        return result;
    }

    /* 遍历引擎查询 */
    for (int ei = 0; ei < p->engine_count && total < 50; ei++) {
        SearchEngine* eng = &p->engines[ei];
        if (!_engine_available(eng, now_tick)) continue;

        /* 引擎间隔节流 */
        time_t now_s = time(NULL);
        if (eng->last_request_time > 0 &&
            (now_s - eng->last_request_time) * 1000 < eng->min_request_interval_ms)
            continue;

        int snip_count = _engine_query(eng, encoded,
                                       all_snips + total, 64 - total);
        if (snip_count > 0) {
            eng->failures = 0;
            eng->requests_today++;
            eng->last_request_time = now_s;
            total += snip_count;
        } else {
            eng->failures++;
            if (eng->failures >= 3) {
                eng->cooldown_until_tick = now_tick + 300;
                if (p->cfg.verbose)
                    fprintf(stderr, "[感觉皮层] %s 熔断 %d tick\n", eng->name, 300);
            }
        }

        /* 搜到足够结果就停 */
        if (total >= 10) break;
    }

    /* 更新缓存 */
    if (total > 0) _cache_insert(p, query, total);

    pthread_mutex_unlock(&p->mutex);

    if (total == 0) {
        char* result = malloc(128);
        if (result) snprintf(result, 128, "（暂时无法搜索到相关信息）");
        return result;
    }

    /* 去重 */
    total = _dedup_snippets(all_snips, total);

    /* 格式化为可读文本 */
    char* result = (char*)calloc(max_len, 1);
    if (!result) return NULL;

    int written = snprintf(result, max_len, "搜索「%s」找到以下信息：\n\n", query);
    for (int i = 0; i < total && i < 8; i++) {
        char line[1024];
        int n = snprintf(line, sizeof(line),
                        "[%d] %s\n    %s\n    (来源: %s)\n\n",
                        i + 1, all_snips[i].title,
                        all_snips[i].snippet[0] ? all_snips[i].snippet : "(无摘要)",
                        all_snips[i].source);
        if (written + n < max_len - 1) {
            memcpy(result + written, line, n);
            written += n;
        }
    }
    result[written] = '\0';
    return result;
}

/**
 * 查询扩展：从拓扑中获取关联词
 */
int perception_expand_query(Perception* p, const char* concept,
                            char expanded[][128], int max) {
    if (!p || !concept || !expanded || max <= 0) return 0;

    SubTopology* vocab = master_get_sub_topology_by_type(p->topology, TOPO_VOCABULARY);
    if (!vocab || !vocab->net) return 0;

    /* 找到概念节点 */
    ReasoningNode* source = node_hash_find(vocab->node_hash, concept);
    if (!source) return 0;

    /* 选共现边强度最高的前 max 个词 */
    typedef struct { char word[128]; float strength; } Related;
    Related related[32];
    int rc = 0;

    for (int i = 0; i < source->edge_count && rc < 32; i++) {
        ReasoningNode* tgt = source->edges[i].target;
        if (!tgt || !tgt->concept || strcmp(tgt->concept, concept) == 0) continue;
        snprintf(related[rc].word, sizeof(related[rc].word), "%s", tgt->concept);
        related[rc].strength = source->edges[i].weight;
        rc++;
    }

    /* 简单排序取 top */
    for (int i = 0; i < rc - 1; i++)
        for (int j = i + 1; j < rc; j++)
            if (related[j].strength > related[i].strength) {
                Related t = related[i]; related[i] = related[j]; related[j] = t;
            }

    int count = 0;
    for (int i = 0; i < rc && count < max; i++) {
        snprintf(expanded[count], 128, "%s", related[i].word);
        count++;
    }
    return count;
}

/* ================================================================
 *  QA 对提取：从纯文本中检测"提问→回答"结构
 * ================================================================ */

#define PM_QA_MAX_QUESTIONS 64
#define PM_QA_MAX_Q_LEN     512
#define PM_QA_MAX_A_LEN    2048

/**
 * 从原始文本中提取 QA 对
 * 策略：检测问号结尾的行作为问题，紧跟的非空行作为回答
 */
int perception_extract_qa_pairs(const char* text,
                                 char questions[][PM_QA_MAX_Q_LEN],
                                 char answers[][PM_QA_MAX_A_LEN],
                                 int max_pairs) {
    if (!text || !questions || !answers || max_pairs <= 0) return 0;

    char clean[65536];
    int clen = 0;
    int in_tag = 0;
    for (const char* p = text; *p && clen < (int)sizeof(clean) - 1; p++) {
        if (*p == '<') in_tag = 1;
        else if (*p == '>') { in_tag = 0; clean[clen++] = ' '; }
        else if (!in_tag) {
            if (*p == '\r') continue;
            clean[clen++] = *p;
        }
    }
    clean[clen] = '\0';

    /* 按行分割 */
    char* lines[2048];
    int line_count = 0;
    char* saveptr;
    char* tok = strtok_r(clean, "\n", &saveptr);
    while (tok && line_count < 2048) {
        while (*tok == ' ' || *tok == '\t') tok++;
        size_t len = strlen(tok);
        while (len > 0 && (tok[len-1] == ' ' || tok[len-1] == '\r')) tok[--len] = '\0';
        if (len > 0) lines[line_count++] = tok;
        tok = strtok_r(NULL, "\n", &saveptr);
    }

    /* 检测 QA 对 */
    int count = 0;
    for (int i = 0; i < line_count - 1 && count < max_pairs; i++) {
        size_t llen = strlen(lines[i]);
        if (llen < 4 || llen > 500) continue;

        /* 检测问句：以？结尾，或包含疑问词 */
        int is_question = 0;
        if (lines[i][llen-1] == '？' || lines[i][llen-1] == '?') {
            is_question = 1;
        } else if (strstr(lines[i], "怎么") || strstr(lines[i], "如何") ||
                   strstr(lines[i], "为什么") || strstr(lines[i], "什么是") ||
                   strstr(lines[i], "哪个") || strstr(lines[i], "能否")) {
            is_question = 1;
        }

        if (!is_question) continue;

        /* 找回答 */
        size_t alen = strlen(lines[i+1]);
        if (alen < 6 || alen > 2000) continue;
        /* 排除连续问句 */
        if (lines[i+1][alen-1] == '？' || lines[i+1][alen-1] == '?') continue;

        snprintf(questions[count], PM_QA_MAX_Q_LEN, "%s", lines[i]);
        snprintf(answers[count], PM_QA_MAX_A_LEN, "%s", lines[i+1]);
        count++;
        i++;  /* 跳过回答行 */
    }

    return count;
}

/**
 * 搜索 + 提取 QA 对 + 喂入自主学习器
 * @return 成功学习的 QA 对数
 */
int perception_search_and_learn_qa(Perception* p, const char* query, int engine_limit) {
    if (!p || !query || !query[0]) return 0;
    if (!p->learner) return 0;
    if (engine_limit <= 0) engine_limit = 3;

    int total_learned = 0;
    int tick = p->tick_counter;

    /* 搜索 */
    char encoded[512];
    _url_encode(query, encoded, sizeof(encoded));

    for (int ei = 0; ei < p->engine_count && ei < engine_limit; ei++) {
        SearchEngine* eng = &p->engines[ei];
        if (!_engine_available(eng, tick)) continue;

        char url[1024];
        snprintf(url, sizeof(url), eng->url_fmt, encoded);

    /* 硬超时保护 — 用 alarm 防止 libcurl 卡死 */
    #ifndef _WIN32
    alarm(eng->timeout_ms / 1000 + 2);
    #endif
    WebResult* wr = web_search(url, eng->timeout_ms > 0 ? eng->timeout_ms : 5000, 131072);
    #ifndef _WIN32
    alarm(0);
    #endif
    if (!wr || !wr->body) { eng->failures++; web_result_free(wr); eng->cooldown_until_tick = tick + 600; continue; }

        eng->failures = 0;

        /* 提取 QA 对 */
        char questions[PM_QA_MAX_QUESTIONS][PM_QA_MAX_Q_LEN];
        char answers[PM_QA_MAX_QUESTIONS][PM_QA_MAX_A_LEN];
        int pair_count = perception_extract_qa_pairs(wr->body, questions, answers, PM_QA_MAX_QUESTIONS);

        /* 喂入自主学习器 */
        AutonomicState astate;
        memset(&astate, 0, sizeof(astate));
        autonomic_state_init(&astate);

        for (int i = 0; i < pair_count; i++) {
            autonomic_learn_from_dialog(p->topology,
                                        questions[i], answers[i],
                                        &astate, NULL, p->memory);
            total_learned++;
        }

        autonomic_state_destroy(&astate);
        web_result_free(wr);

        if (p->cfg.verbose)
            fprintf(stderr, "[QA训练] '%s' %s: %d对\n", query, eng->name, pair_count);
    }

    return total_learned;
}

int perception_feed_learn_text(Perception* p, const char* text) {
    if (!p || !text || !text[0]) return 0;
    if (!p->ar || !p->topology) return 0;

    /* 中文分句：将句号、问号、感叹号、分号替换为换行 */
    char* buf = strdup(text);
    if (!buf) return 0;
    for (char* c = buf; *c; c++)
        if (*c == '。' || *c == '！' || *c == '？' || *c == '；') *c = '\n';

    int fed = 0;
    char* line = strtok(buf, "\n");
    while (line) {
        while (*line == ' ' || *line == '\t') line++;
        if (*line) {
            article_process_line(p->ar, line);
            fed++;
        }
        line = strtok(NULL, "\n");
    }
    free(buf);

    /* 每 5 句 flush 一次，强制写入拓扑 */
    static int call_count = 0;
    if (++call_count % 5 == 0) {
        SubTopology* vocab = NULL;
        for (int t = 0; t < p->topology->sub_topo_count; t++)
            if (p->topology->sub_topologies[t] && p->topology->sub_topologies[t]->type == TOPO_VOCABULARY)
                { vocab = p->topology->sub_topologies[t]; break; }
        article_flush(p->ar, vocab);
    }

    return fed;
}
