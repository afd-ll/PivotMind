/**
 * @file perception.c
 * @brief 感觉皮层实现 — 好奇心驱动自主搜索学习
 */

#include "perception.h"
#include "web_search.h"
#include "web_fetch.h"
#include "active_learner.h"
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

/* 全局感觉皮层指针 — 供 self_learner / dialog_system 等模块通过 extern 引用 */
Perception* g_perception = NULL;

/* 搜狗搜索（已验证可用，HTTP 200, 528KB） */
#define SOGOU_WEB   "https://www.sogou.com/sie?ie=utf-8&query="
/* 搜狗备选（独立熔断状态，同一 URL 不同 UA 重试） */
#define SOGOU_WEB2  "https://www.sogou.com/sie?ie=utf-8&query="
/* Bing 搜索（cn.bing.com，需 302 重定向，web_fetch 已支持） */
#define BING_WEB    "https://cn.bing.com/search?q="

/* 搜索 Provider 索引 */
#define PROV_SOGOU    0
#define PROV_BING     1
#define PROV_SOGOU2   2   /* 备选：搜狗独立熔断入口 */
#define PROV_COUNT    3

/* Provider 熔断：连续失败此数后冷却 */
#define PROVIDER_FAIL_MAX   3
/* Provider 冷却时长（tick 数） */
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

    printf("[感觉皮层] 就绪 (最大%d个/周期, 超时%dms, 缓存%ds, article_reader 已绑定)\n",
           p->cfg.max_searches_per_cycle, p->cfg.search_timeout_ms,
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
    char url[512];
    switch (prov_idx) {
    case PROV_SOGOU:
        snprintf(url, sizeof(url), "%s%s", SOGOU_WEB, encoded_query);
        break;
    case PROV_BING:
        snprintf(url, sizeof(url), "%s%s", BING_WEB, encoded_query);
        break;
    case PROV_SOGOU2:
        snprintf(url, sizeof(url), "%s%s", SOGOU_WEB2, encoded_query);
        break;
    default:
        return NULL;
    }

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
