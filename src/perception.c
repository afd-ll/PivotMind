/**
 * @file perception.c
 * @brief 感觉皮层实现 — 好奇心驱动自主搜索学习
 */

#include "perception.h"
#include "web_search.h"
#include "active_learner.h"
#include "huarong_topology.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 百度百科搜索 URL */
#define BAIDU_BK "https://baike.baidu.com/item/"

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

/* ================================================================ */

Perception* perception_create(MasterTopology* topology,
                               MemorySystem* memory,
                               ActiveLearner* learner,
                               PerceptionConfig* cfg) {
    if (!topology || !memory || !learner) return NULL;

    Perception* p = (Perception*)calloc(1, sizeof(Perception));
    if (!p) return NULL;

    p->topology = topology;
    p->memory   = memory;
    p->learner  = learner;

    if (cfg) p->cfg = *cfg;
    else p->cfg = (PerceptionConfig)PERCEPTION_DEFAULT_CONFIG;

    printf("[感觉皮层] 就绪 (每次最多搜%d个概念, 超时%dms)\n",
           p->cfg.max_searches_per_cycle, p->cfg.search_timeout_ms);
    return p;
}

void perception_destroy(Perception* p) {
    if (!p) return;
    free(p);
}

/* ================================================================
 *  核心搜索+学习循环
 * ================================================================ */

/**
 * 搜索一个概念并喂给海马体学习
 * @return 成功学习的连接数，0=无结果，-1=失败
 */
static int search_and_learn(Perception* p, const char* concept, PerceptionSource source) {
    if (!concept || strlen(concept) < 2) return 0;

    /* 构造搜索 URL */
    char encoded[256];
    _url_encode(concept, encoded, sizeof(encoded));

    char url[512];
    snprintf(url, sizeof(url), "%s%s", BAIDU_BK, encoded);

    /* 联网搜索 */
    if (p->cfg.verbose) {
        const char* src_names[] = {"好奇", "巩固", "对话", "空闲"};
        fprintf(stderr, "[感觉皮层] %s探索: '%s' → %s\n",
                src_names[source], concept, url);
    }

    WebResult* wr = web_search(url, p->cfg.search_timeout_ms, 32768);
    if (!wr || wr->status_code != 200 || !wr->body) {
        if (wr) web_result_free(wr);
        return 0;
    }

    /* 提取文本 */
    int body_len = wr->body_len;
    if (body_len > 8192) body_len = 8192;
    char* text = (char*)malloc(body_len + 1);
    if (!text) { web_result_free(wr); return -1; }

    int text_len = web_extract_text(wr->body, text, body_len);
    text[text_len] = '\0';
    web_result_free(wr);

    if (text_len < 10) { free(text); return 0; }

    /* 提取关键词（用搜索结果前几个词作为关联概念） */
    /* 喂给海马体学习 */
    // learn_from_dialog 需要: (learner, message, user_input, expected_response)
    // 我们将搜索结果作为"对话消息"喂入
    learn_from_dialog(p->learner, concept, text, "");

    int conns_added = 0;

    /* 再提取关键词建立关联 */
    if (wr && wr->keywords) {
        for (int i = 0; i < wr->keyword_count && i < 10; i++) {
            if (wr->keywords[i] && strlen(wr->keywords[i]) > 1) {
                learn_from_dialog(p->learner, wr->keywords[i], "", "");
                conns_added++;
            }
        }
    }

    free(text);
    p->total_searches++;
    p->total_concepts_learned++;
    p->total_new_connections += conns_added;

    if (p->cfg.verbose && conns_added > 0) {
        fprintf(stderr, "[感觉皮层] '%s' → 学习了 %d 个关联概念\n", concept, conns_added);
    }

    return conns_added;
}

/* ================================================================
 *  好奇心采样 — 选低置信度/低连接的节点
 * ================================================================ */

static int curiosity_score_node(ReasoningNode* node) {
    if (!node || !node->concept || node->is_cooled) return -1;
    /* 越低置信度 + 越少连接 → 越需要搜索 */
    if (node->connection_count == 0) return 100;  /* 孤立节点最高优先级 */
    if (node->confidence < 0.1f) return 80;
    if (node->connection_count < 5) return 60;
    if (node->confidence < 0.3f) return 40;
    return 0;
}

/* ================================================================
 *  tick 入口
 * ================================================================ */

void perception_tick(Perception* p, float throttle) {
    if (!p) return;

    p->tick_counter++;
    p->fallback_counter++;

    /* 保底触发：无论多忙，到达间隔必须搜一次 */
    int force_search = (p->fallback_counter >= p->cfg.fallback_interval_ticks);

    if (!force_search && p->tick_counter < p->cfg.cycle_interval_ticks) return;
    p->tick_counter = 0;

    if (force_search) {
        p->fallback_counter = 0;
        if (p->cfg.verbose) fprintf(stderr, "[感觉皮层] 保底触发 (超过%d秒无搜索)\n",
                                    p->cfg.fallback_interval_ticks);
        throttle = 1.0f;  /* 强制全速搜索 */
    } else {
        float roll = (float)_perception_rand(&(unsigned int){0}) / 32767.0f;
        if (roll > throttle) return;
    }

    /* 采样：从词汇拓扑中选好奇心得分最高的节点 */
    SubTopology* vocab = NULL;
    for (int t = 0; t < p->topology->sub_topo_count; t++) {
        SubTopology* sub = p->topology->sub_topologies[t];
        if (sub && sub->type == TOPO_VOCABULARY) { vocab = sub; break; }
    }
    if (!vocab || !vocab->net) return;

    /* 选 top N 候选 */
    #define MAX_CANDIDATES 32
    typedef struct { int idx; int score; const char* concept; } Candidate;
    Candidate candidates[MAX_CANDIDATES];
    int cand_n = 0;

    unsigned int rng = (unsigned int)time(NULL);
    for (int i = 0; i < 200 && cand_n < MAX_CANDIDATES; i++) {
        int idx = _perception_rand(&rng) % vocab->net->node_count;
        ReasoningNode* node = vocab->net->nodes[idx];
        if (!node || !node->concept) continue;
        int score = curiosity_score_node(node);
        if (score > 0) {
            candidates[cand_n].idx     = idx;
            candidates[cand_n].score   = score;
            candidates[cand_n].concept = node->concept;
            cand_n++;
        }
    }
    #undef MAX_CANDIDATES

    /* 按得分排序（简单冒泡，候选数少） */
    for (int i = 0; i < cand_n - 1; i++) {
        for (int j = i + 1; j < cand_n; j++) {
            if (candidates[j].score > candidates[i].score) {
                Candidate tmp = candidates[i];
                candidates[i] = candidates[j];
                candidates[j] = tmp;
            }
        }
    }

    /* 搜索 top N */
    int searched = 0;
    for (int i = 0; i < cand_n && searched < p->cfg.max_searches_per_cycle; i++) {
        if (search_and_learn(p, candidates[i].concept, PERCEPT_CURIOSITY) >= 0) {
            searched++;
        }
    }
}

/* ================================================================ */

int perception_learn_concept(Perception* p, const char* concept) {
    if (!p || !concept) return -1;
    return search_and_learn(p, concept, PERCEPT_DIALOG);
}

int perception_consolidate_node(Perception* p, int node_id) {
    if (!p) return -1;

    /* 找词汇拓扑中的对应节点 */
    SubTopology* vocab = NULL;
    for (int t = 0; t < p->topology->sub_topo_count; t++) {
        SubTopology* sub = p->topology->sub_topologies[t];
        if (sub && sub->type == TOPO_VOCABULARY) { vocab = sub; break; }
    }
    if (!vocab || !vocab->net || node_id < 0 || node_id >= vocab->net->node_count) return -1;

    ReasoningNode* node = vocab->net->nodes[node_id];
    if (!node || !node->concept) return -1;

    return search_and_learn(p, node->concept, PERCEPT_CONSOLIDATE);
}

void perception_stats(Perception* p, long* searches, long* learned, long* new_conns) {
    if (!p) return;
    if (searches) *searches = p->total_searches;
    if (learned)  *learned  = p->total_concepts_learned;
    if (new_conns) *new_conns = p->total_new_connections;
}
