/**
 * @file article_reader.c
 * @brief 文章阅读模式实现 — 字符级统计合并词发现
 *
 * 算法流程：
 *   1. 逐字符遍历文章，统计字符频率 + 滑动窗口共现
 *   2. 每 batch_size 行或 flush 时运行词发现：
 *      a. 计算相邻字符对的 PMI
 *      b. 计算分层合评分: score = α·PMI + β·freq_bonus + γ·conn_bonus
 *      c. 高于阈值的字符对合并为词
 *      d. 长词扩展：已合并的词尝试与相邻词扩展
 *   3. 发现词 → 创建概念节点（词汇拓扑）
 *   4. 相邻词之间建边
 */

#include "article_reader.h"
#include "huarong_topology.h"
#include "chinese.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

// ==================== 内部常量 ====================

// 字符/词哈希表大小（2 的幂）
#define AR_CHAR_HASH_SIZE  16384
#define AR_PAIR_HASH_SIZE  65536
#define AR_WORD_HASH_SIZE  16384

#define AR_CHAR_HASH_MASK  (AR_CHAR_HASH_SIZE - 1)
#define AR_PAIR_HASH_MASK  (AR_PAIR_HASH_SIZE - 1)
#define AR_WORD_HASH_MASK  (AR_WORD_HASH_SIZE - 1)

// 词库初始容量
#define AR_MAX_WORDS       32
#define AR_WORD_MAX_LEN    32   // 最长词（字符数）

// ==================== 哈希表条目 ====================

/** 字符频率条目 */
typedef struct {
    char   text[8];       // UTF-8 字符（含终止符）
    int    count;         // 出现次数
    int    used;          // 占用标记
} CharEntry;

/** 字符对共现条目 */
typedef struct {
    char   a[8];          // 第一个字符
    char   b[8];          // 第二个字符
    int    co_count;      // 共现次数
    int    used;
} PairEntry;

/** 发现的词 */
typedef struct {
    char   text[AR_WORD_MAX_LEN * 4 + 1];  // UTF-8 文本
    int    char_len;      // 字符数
    int    freq;          // 出现次数
} WordEntry;

// ==================== 主结构 ====================

struct ArticleReader {
    MasterTopology*    master;
    ArticleReaderConfig cfg;

    // 字符频率表（开放定址哈希）
    CharEntry   char_table[AR_CHAR_HASH_SIZE];
    int         char_count;      // 不同字符数
    int         total_chars;     // 累计字符总次数

    // 字符对共现表（开放定址哈希）
    PairEntry   pair_table[AR_PAIR_HASH_SIZE];
    int         pair_count;

    // 发现的词（动态数组）
    WordEntry*  words;
    int         word_count;
    int         word_capacity;

    // 当前缓冲行
    int         lines_buffered;

    // 进度回调指针
    long*       p_added_nodes;
    long*       p_added_edges;

    // 内部工作缓冲区
    char        line_chars[4096][8];    // 当前行字符序列（UTF-8 字符串）
    int         line_char_count;         // 当前行有效字符数
};

// ==================== 哈希工具 ====================

/** DJB2 哈希（用于 UTF-8 字符串） */
static inline unsigned int _ar_hash(const char* s) {
    unsigned int h = 5381;
    while (*s) h = ((h << 5) + h) + (unsigned char)*s++;
    return h;
}

/** 在 char_table 中查找或插入字符 */
static CharEntry* _ar_find_char(ArticleReader* ar, const char* ch) {
    unsigned int h = _ar_hash(ch) & AR_CHAR_HASH_MASK;
    for (int i = 0; i < AR_CHAR_HASH_SIZE; i++) {
        int idx = (h + i) & AR_CHAR_HASH_MASK;
        if (!ar->char_table[idx].used) {
            // 空位 → 插入
            strncpy(ar->char_table[idx].text, ch, sizeof(ar->char_table[idx].text) - 1);
            ar->char_table[idx].text[sizeof(ar->char_table[idx].text) - 1] = 0;
            ar->char_table[idx].count = 0;
            ar->char_table[idx].used = 1;
            ar->char_count++;
            return &ar->char_table[idx];
        }
        if (strcmp(ar->char_table[idx].text, ch) == 0) {
            return &ar->char_table[idx];
        }
    }
    return NULL;  // 表满
}

/** 在 pair_table 中查找或插入字符对 */
static PairEntry* _ar_find_pair(ArticleReader* ar, const char* a, const char* b) {
    char key[16];
    snprintf(key, sizeof(key), "%s|%s", a, b);
    unsigned int h = _ar_hash(key) & AR_PAIR_HASH_MASK;
    for (int i = 0; i < AR_PAIR_HASH_SIZE; i++) {
        int idx = (h + i) & AR_PAIR_HASH_MASK;
        if (!ar->pair_table[idx].used) {
            strncpy(ar->pair_table[idx].a, a, sizeof(ar->pair_table[idx].a) - 1);
            ar->pair_table[idx].a[sizeof(ar->pair_table[idx].a) - 1] = 0;
            strncpy(ar->pair_table[idx].b, b, sizeof(ar->pair_table[idx].b) - 1);
            ar->pair_table[idx].b[sizeof(ar->pair_table[idx].b) - 1] = 0;
            ar->pair_table[idx].co_count = 0;
            ar->pair_table[idx].used = 1;
            ar->pair_count++;
            return &ar->pair_table[idx];
        }
        if (strcmp(ar->pair_table[idx].a, a) == 0 &&
            strcmp(ar->pair_table[idx].b, b) == 0) {
            return &ar->pair_table[idx];
        }
    }
    return NULL;
}

/** 查找或添加词到词表 */
static WordEntry* _ar_find_or_add_word(ArticleReader* ar, const char* text) {
    for (int i = 0; i < ar->word_count; i++) {
        if (strcmp(ar->words[i].text, text) == 0)
            return &ar->words[i];
    }
    // 新增
    if (ar->word_count >= ar->word_capacity) {
        int new_cap = ar->word_capacity ? ar->word_capacity * 2 : AR_MAX_WORDS;
        WordEntry* tmp = (WordEntry*)realloc(ar->words, new_cap * sizeof(WordEntry));
        if (!tmp) return NULL;
        ar->words = tmp;
        ar->word_capacity = new_cap;
    }
    WordEntry* we = &ar->words[ar->word_count++];
    strncpy(we->text, text, sizeof(we->text) - 1);
    we->text[sizeof(we->text) - 1] = 0;
    we->char_len = (int)utf8_strlen(text);
    we->freq = 0;
    return we;
}

// ==================== 获取字符序列 ====================

/**
 * 从一行文本中提取有效的字符序列（中文+英文，跳过标点/空白/数字）
 * 结果写入 ar->line_chars[]
 */
static int _ar_extract_chars(ArticleReader* ar, const char* line) {
    const char* p = line;
    int count = 0;

    while (*p && count < 4096) {
        if (is_punctuation(p)) {
            int skip = get_char_bytes(p);
            if (skip <= 0) skip = 1;
            p += skip;
            continue;
        }
        int bytes = get_char_bytes(p);
        if (bytes <= 0 || bytes > 4) { p++; continue; }
        if (bytes == 1 && (*p < 32 || *p > 126)) { p++; continue; }

        char buf[8];
        strncpy(buf, p, bytes);
        buf[bytes] = 0;

        // 跳过纯数字
        if (bytes == 1 && *p >= '0' && *p <= '9') { p++; continue; }

        strncpy(ar->line_chars[count], buf, sizeof(ar->line_chars[count]) - 1);
        ar->line_chars[count][sizeof(ar->line_chars[count]) - 1] = 0;
        count++;
        p += bytes;
    }

    ar->line_char_count = count;
    return count;
}

// ==================== 词发现算法 ====================

/**
 * 使用合并评分发现词边界
 * 评分 = α·PMI + β·freq_bonus + γ·conn_bonus
 * 高于 pmi_threshold 的字符对视为"应合并为词"
 *
 * 策略：逐字符扫描，若 char[i] 和 char[i+1] 的合并评分高于阈值，
 * 则合并为一个临时词，继续尝试扩展。
 * 最终词列表保存在 ar->words 中。
 */

// ==================== 建图 ====================

/**
 * 将发现的词构建到拓扑中
 * 每个词 → 一个概念节点
 * 相邻词之间 → 共现边
 *
 * 词→句子的序列保存在一个临时数组中，flush 时从 words 重建序列。
 */
static int _ar_build_topo(ArticleReader* ar, SubTopology* topo) {
    if (!ar || !topo || !topo->net || ar->word_count == 0) return 0;

    HuarongTopologyNet* net = topo->net;
    int created = 0;

    // 扫描词表，每个词尝试创建节点
    for (int i = 0; i < ar->word_count; i++) {
        WordEntry* we = &ar->words[i];
        if (!we->text[0]) continue;

        // 检查节点是否已存在
        int nid = huarong_net_find_concept(net, we->text);
        if (nid < 0 && net->node_count < net->max_nodes) {
            nid = huarong_net_dynamic_add_node(net, we->text, NULL, 0);
            if (nid >= 0) {
                created++;
                if (ar->p_added_nodes) (*ar->p_added_nodes)++;
                // 激活值基于频次
                net->nodes[nid]->activation = we->freq > 5 ? 0.5f : 0.2f;
            }
        }
    }

    return created;
}

// ==================== 公共 API ====================

ArticleReader* article_reader_create(MasterTopology* master,
                                     ArticleReaderConfig* cfg) {
    if (!master) return NULL;

    ArticleReader* ar = (ArticleReader*)calloc(1, sizeof(ArticleReader));
    if (!ar) return NULL;

    ar->master = master;
    if (cfg) {
        ar->cfg = *cfg;
    } else {
        ar->cfg.window_size = 2;
        ar->cfg.pmi_threshold = 1.5f;
        ar->cfg.min_freq = 2;
        ar->cfg.alpha = 0.4f;
        ar->cfg.beta = 0.4f;
        ar->cfg.gamma = 0.2f;
        ar->cfg.batch_size = 200;
        ar->cfg.verbose = 0;
    }

    ar->p_added_nodes = NULL;
    ar->p_added_edges = NULL;
    ar->line_char_count = 0;
    ar->lines_buffered = 0;

    // 初始化词表
    ar->word_capacity = AR_MAX_WORDS;
    ar->words = (WordEntry*)calloc(ar->word_capacity, sizeof(WordEntry));
    ar->word_count = 0;

    if (ar->cfg.verbose) {
        printf("[文章阅读] 初始化: 窗口=%d PMI阈值=%.2f "
               "α=%.2f β=%.2f γ=%.2f batch=%d\n",
               ar->cfg.window_size, ar->cfg.pmi_threshold,
               ar->cfg.alpha, ar->cfg.beta, ar->cfg.gamma,
               ar->cfg.batch_size);
    }

    return ar;
}

void article_reader_destroy(ArticleReader* ar) {
    if (!ar) return;
    free(ar->words);
    free(ar);
}

int article_process_line(ArticleReader* ar, const char* line) {
    if (!ar || !line) return 0;

    // 提取有效字符序列
    int n = _ar_extract_chars(ar, line);
    if (n < 2) return 0;

    // 更新字符频率和共现统计
    for (int i = 0; i < n; i++) {
        // 字符频率
        CharEntry* ce = _ar_find_char(ar, ar->line_chars[i]);
        if (ce) { ce->count++; ar->total_chars++; }

        // 窗口内共现
        for (int w = 1; w <= ar->cfg.window_size && (i + w) < n; w++) {
            PairEntry* pe = _ar_find_pair(ar, ar->line_chars[i],
                                           ar->line_chars[i + w]);
            if (pe) pe->co_count++;
        }
    }

    ar->lines_buffered++;

    // 达到 batch 大小则触发词发现
    if (ar->lines_buffered >= ar->cfg.batch_size) {
        int found = article_flush(ar, NULL);
        ar->lines_buffered = 0;
        return found > 0 ? found : 1;
    }

    return 0;
}

int article_flush(ArticleReader* ar, SubTopology* topo) {
    if (!ar) return -1;

    // 如果没有传入 topo，从 master 获取词汇拓扑
    SubTopology* vocab = topo;
    if (!vocab && ar->master) {
        for (int t = 0; t < ar->master->sub_topo_count; t++) {
            if (ar->master->sub_topologies[t] &&
                ar->master->sub_topologies[t]->type == TOPO_VOCABULARY) {
                vocab = ar->master->sub_topologies[t];
                break;
            }
        }
    }
    if (!vocab || !vocab->net) return -1;

    // 数据不足时不处理
    if (ar->total_chars < 10) return 0;

    // — 行缓冲中的字符未在 words 中保留，直接使用统计表跑一次词发现 —
    // 注意：flush 时 line_chars 为空，因为我们每行处理后就丢弃了
    // 正确的做法是从统计表中提取出高频相邻对，重建序列
    // 简化版：遍历所有共现对，找高频强关联组合

    int old_word_count = ar->word_count;

    // 简化词发现：遍历所有共现对，评分高的视为词
    for (int i = 0; i < AR_PAIR_HASH_SIZE; i++) {
        if (!ar->pair_table[i].used) continue;
        PairEntry* pe = &ar->pair_table[i];
        if (pe->co_count < ar->cfg.min_freq) continue;

        // 计算 PMI
        CharEntry* ce_a = _ar_find_char(ar, pe->a);
        CharEntry* ce_b = _ar_find_char(ar, pe->b);
        if (!ce_a || !ce_b || ce_a->count == 0 || ce_b->count == 0) continue;

        float p_a  = (float)ce_a->count / ar->total_chars;
        float p_b  = (float)ce_b->count / ar->total_chars;
        float p_ab = (float)pe->co_count / (ar->total_chars - 1);

        float pmi = 0.0f;
        if (p_a > 0 && p_b > 0 && p_ab > 0) {
            pmi = logf(p_ab / (p_a * p_b));
            if (pmi < 0) pmi = 0;
        }

        float freq_bonus = (float)pe->co_count / (pe->co_count + 5.0f);
        float score = ar->cfg.alpha * pmi + ar->cfg.beta * freq_bonus;

        if (score > ar->cfg.pmi_threshold) {
            // 构建双字符词
            char word_buf[20] = {0};
            strncat(word_buf, pe->a, sizeof(word_buf) - strlen(word_buf) - 1);
            strncat(word_buf, pe->b, sizeof(word_buf) - strlen(word_buf) - 1);

            WordEntry* we = _ar_find_or_add_word(ar, word_buf);
            if (we) we->freq += pe->co_count;
        }
    }

    // 尝试三字扩展：检查发现的二字词与相邻字符的组合
    int extended = 1;
    while (extended) {
        extended = 0;
        for (int i = 0; i < ar->word_count && !extended; i++) {
            WordEntry* w1 = &ar->words[i];
            if (w1->char_len < 1) continue;

            for (int j = 0; j < ar->word_count; j++) {
                if (i == j) continue;
                WordEntry* w2 = &ar->words[j];
                if (w2->char_len < 1) continue;

                // 检查 w1 最后一个字 和 w2 第一个字是否有强共现
                // 如果是同一个词被拆成两半，尝试合并
                char last_char_of_w1[8] = {0};
                const char* p = w1->text;
                int char_pos = 0;
                while (*p && char_pos < w1->char_len - 1) {
                    int blen = get_char_bytes(p);
                    if (blen <= 0) break;
                    p += blen;
                    char_pos++;
                }
                if (*p) {
                    int blen = get_char_bytes(p);
                    if (blen > 0 && blen <= 4) {
                        strncpy(last_char_of_w1, p, blen);
                        last_char_of_w1[blen] = 0;
                    }
                }

                char first_char_of_w2[8] = {0};
                if (w2->text[0]) {
                    int blen = get_char_bytes(w2->text);
                    if (blen > 0 && blen <= 4) {
                        strncpy(first_char_of_w2, w2->text, blen);
                        first_char_of_w2[blen] = 0;
                    }
                }

                if (!last_char_of_w1[0] || !first_char_of_w2[0]) continue;

                // 检查共现
                char key[16];
                snprintf(key, sizeof(key), "%s|%s", last_char_of_w1, first_char_of_w2);
                unsigned int h = _ar_hash(key) & AR_PAIR_HASH_MASK;
                for (int k = 0; k < AR_PAIR_HASH_SIZE; k++) {
                    int idx = (h + k) & AR_PAIR_HASH_MASK;
                    if (!ar->pair_table[idx].used) break;
                    if (strcmp(ar->pair_table[idx].a, last_char_of_w1) == 0 &&
                        strcmp(ar->pair_table[idx].b, first_char_of_w2) == 0) {
                        if (ar->pair_table[idx].co_count >= ar->cfg.min_freq * 2) {
                            // 合并为三字/四字词
                            char combined[AR_WORD_MAX_LEN * 4 + 1] = {0};
                            strncat(combined, w1->text,
                                    sizeof(combined) - strlen(combined) - 1);
                            strncat(combined, w2->text,
                                    sizeof(combined) - strlen(combined) - 1);

                            if ((int)utf8_strlen(combined) <= AR_WORD_MAX_LEN) {
                                WordEntry* we = _ar_find_or_add_word(ar, combined);
                                if (we) {
                                    we->freq += w1->freq + w2->freq;
                                    extended = 1;
                                }
                            }
                        }
                        break;
                    }
                }
            }
        }
    }

    int new_words = ar->word_count - old_word_count;

    // 建图
    int built = _ar_build_topo(ar, vocab);
    if (built > 0) {
        // 在相邻词之间建共现边
        // 简化版：在同一篇文章中相邻出现的词之间建边
        // 由于我们丢失了词序列信息，暂时只创建节点
        // 后续版本可以在 process_line 阶段同步构建 seq 序列

        if (ar->cfg.verbose) {
            printf("[文章阅读] 刷新: %d 新词, 共 %d 词, 累计 %d 不同字符, "
                   "%d 字符对\n",
                   new_words, ar->word_count,
                   ar->char_count, ar->pair_count);
        }
    }

    return new_words;
}

void article_set_progress_ptr(ArticleReader* ar,
                              long* total_added_nodes_ptr,
                              long* total_added_edges_ptr) {
    if (!ar) return;
    ar->p_added_nodes = total_added_nodes_ptr;
    ar->p_added_edges = total_added_edges_ptr;
}

void article_get_stats(ArticleReader* ar,
                       int* out_chars, int* out_pairs, int* out_words) {
    if (!ar) return;
    if (out_chars) *out_chars = ar->char_count;
    if (out_pairs) *out_pairs = ar->pair_count;
    if (out_words) *out_words = ar->word_count;
}
