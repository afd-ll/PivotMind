/**
 * @file article_reader.c
 * @brief 文章阅读模式实现 — 字符级统计 + 序列正向扫描词发现
 *
 * 算法流程：
 *   1. 逐字符遍历文章，统计字符频率 + 滑动窗口共现，同时累积字符序列到 seq[]
 *   2. 每 batch_size 行或 flush 时运行词发现：
 *      a. 预计算 seq[] 相邻字符对的合并评分: score = α·PMI + β·freq_bonus + γ·conn_bonus
 *      b. 沿 seq[] 正向贪婪扫描，高评分连续段合并为一个词
 *      c. 一轮三字扩展：首尾字相同的词对合并
 *   3. 发现词 → 创建概念节点（词汇拓扑）
 *   4. 清空 seq 缓冲区，下一轮重新累积
 */

#include "article_reader.h"
#include "huarong_topology.h"
#include "topology_growth.h"
#include "common.h"
#include "chinese.h"
#include "thalamus.h"
#include "emergent_pos.h"
#include "error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <float.h>
#include <pthread.h>

// ==================== 内部常量 ====================

// 字符/词哈希表大小（2 的幂）
#define AR_CHAR_HASH_SIZE  16384
#define AR_PAIR_HASH_SIZE  262144  /* 65536→262144, 支持更多词共现对 */
#define AR_WORD_HASH_SIZE  16384

#define AR_CHAR_HASH_MASK  (AR_CHAR_HASH_SIZE - 1)
#define AR_PAIR_HASH_MASK  (AR_PAIR_HASH_SIZE - 1)
#define AR_WORD_HASH_MASK  (AR_WORD_HASH_SIZE - 1)

// 词库初始容量
#define AR_MAX_WORDS       32
#define AR_WORD_MAX_LEN    32   // 最长词（字符数）

// ==================== v0.5.9 记忆化常量 ====================
// 字符/字符对统计 = 记忆痕迹：权重(计数) + 巩固度 + 生命周期层级
// （用户拍板：'你好'几天不露面不能删——遗忘≠删除，只降权）
#define AR_MEM_CONSOLIDATE_RATE  0.02f   // 每次出现巩固增量（Hebbian 渐近）
#define AR_MEM_PROMOTE_THRESHOLD 0.5f    // 巩固度 ≥ 此值 → 晋升长期（tier=1）
#define AR_MEM_TIER_SHORT        0       // 短期（工作记忆）
#define AR_MEM_TIER_LONG         1       // 长期（巩固后，永不删除）
#define AR_MEM_SHORT_WINDOW      50000   // 短期清理窗口（记忆 tick 数）
#define AR_MEM_TABLE_MAX         2000000 // 表上限（满则清理短期+不活跃）
#define AR_MEM_INIT_CONSOLIDATED 0.05f   // 新条目起步巩固度（冷启动安全）
#define AR_MEM_CLEAN_COOLDOWN    10000   // 表满清理冷却（记忆 tick）——防每次 find 都触发

// ==================== 哈希表条目 ====================

/** 字符频率条目 — v0.5.9 记忆化 */
typedef struct {
    char   text[8];       // UTF-8 字符（含终止符）
    int    count;         // 出现次数（权重）
    float  consolidated;  // 巩固度 0~1
    int    last_seen;     // 最近出现记忆 tick
    int    tier;          // 短期/长期
    int    used;          // 占用标记
} CharEntry;

/** 字符对共现条目 — v0.5.9 记忆化 */
typedef struct {
    char   a[8];          // 第一个字符
    char   b[8];          // 第二个字符
    int    co_count;      // 共现次数（权重）
    float  consolidated;  // 巩固度 0~1
    int    last_seen;     // 最近出现记忆 tick
    int    tier;          // 短期/长期
    int    used;
    unsigned int h;       // 08-18: 键哈希值（a|b 复合 DJB2），插入时固化——
                          // 探测短路 + 扩容/清理 rehash 复用，免重建复合串
} PairEntry;

/** 词哈希表条目（声明在主结构之前） */
typedef struct {
    char   text[AR_WORD_MAX_LEN * 4 + 1];
    int    word_index;
    int    used;
} WordHashEntry;

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

    pthread_mutex_t mutex;     /* 保护本 reader 内所有哈希表 + 缓冲区 */

    // 字符频率表（开放定址哈希）
    CharEntry   char_table[AR_CHAR_HASH_SIZE];
    int         char_count;      // 不同字符数
    int         total_chars;     // 累计字符总次数

    // 字符对共现表（开放定址哈希）
    PairEntry*  pair_table;
    int         pair_hash_size;
    int         pair_count;

    // 发现的词（动态数组）
    WordEntry*  words;
    int         word_count;
    int         word_capacity;

    // 词哈希表（加速查找，创建时动态分配，支持扩容）
    WordHashEntry* word_hash;
    unsigned int   word_hash_size;   // 当前哈希表大小（槽数）
    unsigned int   word_hash_mask;
    int            word_hash_entries; // 已占用槽数（用于扩容判定）

    // 当前缓冲行
    int         lines_buffered;

    // 进度回调指针
    long*       p_added_nodes;
    long*       p_added_edges;

    // 全局字符序列缓冲区（跨行累积，由 calloc 分配，避免栈溢出）
    char        (*seq)[8];       // [seq_capacity][8] 堆分配
    int         seq_len;         // 有效长度
    int         seq_capacity;    // 当前容量

    // 当前行字符缓冲区（堆分配）
    char        (*line_chars)[8]; // [line_chars_capacity][8] 堆分配
    int         line_char_count;
    int         line_chars_capacity;

    // 缓存词汇拓扑指针（article_reader_create 时初始化）
    SubTopology* vocab_topo;

    // 上次 flush 新增词数（供外部查询）
    int         last_flush_added;

    // 丘脑信号总线（可选，NULL = 不发送反馈）
    Thalamus*   thalamus;

    // v0.5.8: 涌现词类系统（POS 池）——喂料路径喂语法拓扑
    EmergentPOS* emergent_pos;

    // v0.5.9: 记忆时钟——字符/字符对记忆化的时间源（每处理一行 +1）
    int mem_tick;
    // 上次表满清理时的记忆 tick（防每次 find 都触发全表扫描）
    int pair_clean_tick;
};

// ==================== 哈希工具 ====================

/** DJB2 哈希（用于 UTF-8 字符串） */
static inline unsigned int _ar_hash(const char* s) {
    unsigned int h = 5381;
    while (*s) h = ((h << 5) + h) + (unsigned char)*s++;
    return h;
}

/** 字符对键哈希（08-18）— 与旧 snprintf("%s|%s",a,b) 复合串的 DJB2 完全同值：
 * 逐字节哈希 a → '|' 分隔符 → b，等价于 _ar_hash("a|b")，但免掉每次查找的
 * snprintf 格式化 + 复合串重扫（a/b 均为单字符，键总长 ≤ 8 字节）。 */
static inline unsigned int _ar_pair_hash(const char* a, const char* b) {
    unsigned int h = 5381;
    while (*a) h = ((h << 5) + h) + (unsigned char)*a++;
    h = ((h << 5) + h) + (unsigned char)'|';
    while (*b) h = ((h << 5) + h) + (unsigned char)*b++;
    return h;
}

/** v0.5.9: 记忆触摸——出现即巩固（Hebbian 渐近），过阈晋升长期 */
static inline void _ar_mem_touch(float* consolidated, int* tier,
                                 int* last_seen, int tick) {
    *consolidated += AR_MEM_CONSOLIDATE_RATE * (1.0f - *consolidated);
    if (*consolidated >= AR_MEM_PROMOTE_THRESHOLD) *tier = AR_MEM_TIER_LONG;
    *last_seen = tick;
}

/** 在 char_table 中查找或插入字符（v0.5.9 记忆化） */
static CharEntry* _ar_find_char(ArticleReader* ar, const char* ch) {
    unsigned int h = _ar_hash(ch) & AR_CHAR_HASH_MASK;
    for (int i = 0; i < AR_CHAR_HASH_SIZE; i++) {
        int idx = (h + i) & AR_CHAR_HASH_MASK;
        if (!ar->char_table[idx].used) {
            // 空位 → 插入（记忆化：起步巩固度 + 出生时间）
            strncpy(ar->char_table[idx].text, ch, sizeof(ar->char_table[idx].text) - 1);
            ar->char_table[idx].text[sizeof(ar->char_table[idx].text) - 1] = 0;
            ar->char_table[idx].count = 0;
            ar->char_table[idx].consolidated = AR_MEM_INIT_CONSOLIDATED;
            ar->char_table[idx].last_seen = ar->mem_tick;
            ar->char_table[idx].tier = AR_MEM_TIER_SHORT;
            ar->char_table[idx].used = 1;
            ar->char_count++;
            return &ar->char_table[idx];
        }
        if (strcmp(ar->char_table[idx].text, ch) == 0) {
            // 命中 → 触摸记忆（巩固 + 更新活跃）
            _ar_mem_touch(&ar->char_table[idx].consolidated,
                          &ar->char_table[idx].tier,
                          &ar->char_table[idx].last_seen, ar->mem_tick);
            return &ar->char_table[idx];
        }
    }
    return NULL;  // 表满
}

#define AR_PAIR_LOAD_MAX  0.75f  /* 负载因子>75%时触发扩容 */
#define AR_PAIR_CLEAN_LOAD 0.85f /* 负载因子>85%时触发表满清理（P1: 防线性探测退化） */

/** 字符对哈希表扩容 — 双倍容量 + rehash */
static int _ar_expand_pair_hash(ArticleReader* ar) {
    int new_size = ar->pair_hash_size * 2;
    if (new_size > 2097152) return -1; /* 上限 2M 槽 */
    PairEntry* new_tab = (PairEntry*)calloc(new_size, sizeof(PairEntry));
    if (!new_tab) return -1;
    int new_mask = new_size - 1;

    for (int i = 0; i < ar->pair_hash_size; i++) {
        if (!ar->pair_table[i].used) continue;
        /* 08-18: 复用条目内固化的键哈希（与插入时同值），免重建复合串 + 重哈希 */
        unsigned int h = ar->pair_table[i].h & new_mask;
        for (int p = 0; p < new_size; p++) {
            int idx = (h + p) & new_mask;
            if (!new_tab[idx].used) { new_tab[idx] = ar->pair_table[i]; break; }
        }
    }
    free(ar->pair_table);
    ar->pair_table = new_tab;
    ar->pair_hash_size = new_size;
    return 0;
}

/** v0.5.9: 表满清理——只清「未巩固 + 长期不活跃」的短期对，长期对永不删。
 * 用重建方式删除（开放寻址表直接置 used=0 会断裂探测链导致条目丢失）。
 * 冷启动安全：新对 last_seen 都新鲜 → 一条不动。 */
static void _ar_cleanup_stale_pairs(ArticleReader* ar) {
    PairEntry* new_tab = (PairEntry*)calloc((size_t)ar->pair_hash_size,
                                            sizeof(PairEntry));
    if (!new_tab) return;
    int mask = ar->pair_hash_size - 1;
    int kept = 0;
    for (int i = 0; i < ar->pair_hash_size; i++) {
        PairEntry* pe = &ar->pair_table[i];
        if (!pe->used) continue;
        /* 淘汰条件：短期（未巩固）+ 长期不活跃（超过窗口没再见） */
        if (pe->tier == AR_MEM_TIER_SHORT &&
            ar->mem_tick - pe->last_seen > AR_MEM_SHORT_WINDOW) {
            continue;
        }
        /* 保留（长期对 或 近期活跃的短期对）：rehash 到新表 */
        /* 08-18: 复用固化键哈希，免重建复合串 + 重哈希 */
        unsigned int h = pe->h & mask;
        for (int p = 0; p < ar->pair_hash_size; p++) {
            int idx = (h + p) & mask;
            if (!new_tab[idx].used) { new_tab[idx] = *pe; kept++; break; }
        }
    }
    int removed = ar->pair_count - kept;
    free(ar->pair_table);
    ar->pair_table = new_tab;
    ar->pair_count = kept;
    if (removed > 0) {
        LOG_INFO("[文章阅读] 字符对记忆清理: 移除 %d 条短期不活跃对 (剩余 %d)",
                 removed, kept);
    }
}

/** 在 pair_table 中查找或插入字符对（v0.5.9 记忆化）
 * 08-18: 开放定址哈希 + 线性探测不变（75% 扩容 / 85% 清理兜底均摊 O(1)），
 * 但每槽比较从 2×strcmp 短路为 1 次 int 哈希比较——仅哈希命中才 strcmp 精确
 * 校验（UTF-8 单字符对碰撞率极低）。键哈希直接对 a、'|'、b 逐字节算，免
 * snprintf 复合串。 */
static PairEntry* _ar_find_pair(ArticleReader* ar, const char* a, const char* b) {
    /* 负载因子检查：超过75%触发扩容 */
    if ((float)ar->pair_count / ar->pair_hash_size > AR_PAIR_LOAD_MAX)
        _ar_expand_pair_hash(ar);

    /* P1（08-16 修复）：清理触发从「绝对表满 AR_MEM_TABLE_MAX(2M)」改为
     * 「负载因子 > AR_PAIR_CLEAN_LOAD(85%)」。原判据要等 pair_count 撞 2M
     * 封顶才清，此时负载已 ~95%，线性探测退化到每次查找数百次 strcmp
     * （08-15 19:44 / 01:22 两次 STUCK 抓栈实锤）。改为 85% 提前清理，
     * 避免探测链退化成 O(n) 扫描。
     * ⚠️ 冷却防抖保留：pair_clean_tick + COOLDOWN < mem_tick 才清——
     * 早期实现 pair_clean_tick < mem_tick 恒真（mem_tick 每行 +1），
     * 表满后每次 find 都触发 88MB calloc + 2M rehash（锁内！）
     * → 学习线程全部卡死（08-07 14:xx 实锤，STUCK 53 分钟） */
    if ((float)ar->pair_count / ar->pair_hash_size > AR_PAIR_CLEAN_LOAD &&
        ar->pair_clean_tick + AR_MEM_CLEAN_COOLDOWN < ar->mem_tick) {
        _ar_cleanup_stale_pairs(ar);
        ar->pair_clean_tick = ar->mem_tick;
    }

    int mask = ar->pair_hash_size - 1;
    unsigned int h = _ar_pair_hash(a, b) & mask;
    for (int i = 0; i < ar->pair_hash_size; i++) {
        int idx = (h + i) & mask;
        PairEntry* pe = &ar->pair_table[idx];
        if (!pe->used) {
            strncpy(pe->a, a, sizeof(pe->a) - 1);
            pe->a[sizeof(pe->a) - 1] = 0;
            strncpy(pe->b, b, sizeof(pe->b) - 1);
            pe->b[sizeof(pe->b) - 1] = 0;
            pe->co_count = 0;
            pe->consolidated = AR_MEM_INIT_CONSOLIDATED;
            pe->last_seen = ar->mem_tick;
            pe->tier = AR_MEM_TIER_SHORT;
            pe->used = 1;
            pe->h = h;  /* 08-18: 固化键哈希（扩容/清理 rehash 直接复用） */
            ar->pair_count++;
            return pe;
        }
        /* 08-18: 哈希短路——先比 int，命中才做精确 strcmp */
        if (pe->h == h &&
            strcmp(pe->a, a) == 0 &&
            strcmp(pe->b, b) == 0) {
            _ar_mem_touch(&pe->consolidated,
                          &pe->tier,
                          &pe->last_seen, ar->mem_tick);
            return pe;
        }
    }
    return NULL;
}

/** 词哈希表扩容 — 双倍容量 + rehash */
static int _ar_expand_word_hash(ArticleReader* ar) {
    unsigned int new_size = ar->word_hash_size * 2;
    if (new_size > 131072) return -1; // 上限保护
    WordHashEntry* new_tab = (WordHashEntry*)calloc(new_size, sizeof(WordHashEntry));
    if (!new_tab) return -1;
    unsigned int new_mask = new_size - 1;

    for (unsigned int i = 0; i < ar->word_hash_size; i++) {
        if (!ar->word_hash[i].used) continue;
        unsigned int h = _ar_hash(ar->word_hash[i].text) & new_mask;
        for (unsigned int p = 0; p < new_size; p++) {
            int idx = (h + p) & new_mask;
            if (!new_tab[idx].used) {
                new_tab[idx] = ar->word_hash[i];
                break;
            }
        }
    }

    free(ar->word_hash);
    ar->word_hash = new_tab;
    ar->word_hash_size = new_size;
    ar->word_hash_mask = new_mask;
    ar->word_hash_entries = 0; // 后面重新计数
    for (unsigned int i = 0; i < new_size; i++)
        if (ar->word_hash[i].used) ar->word_hash_entries++;
    return 0;
}

/** 查找或添加词到词表（哈希加速 O(1)），哈希满时自动扩容 */
static WordEntry* _ar_find_or_add_word(ArticleReader* ar, const char* text) {
    if (!ar->word_hash) return NULL;
    unsigned int mask = ar->word_hash_mask;
    unsigned int h = _ar_hash(text) & mask;
    for (int i = 0; i < (int)(mask + 1); i++) {
        int idx = (h + i) & mask;
        if (!ar->word_hash[idx].used) {
            // 新增
            if (ar->word_count >= ar->word_capacity) {
                int new_cap = ar->word_capacity ? ar->word_capacity * 2 : AR_MAX_WORDS;
                WordEntry* tmp = (WordEntry*)realloc(ar->words, new_cap * sizeof(WordEntry));
                if (!tmp) return NULL;
                ar->words = tmp;
                ar->word_capacity = new_cap;
            }
            WordEntry* we = &ar->words[ar->word_count];
            snprintf(we->text, sizeof(we->text), "%s", text);
            we->char_len = (int)utf8_strlen(text);
            we->freq = 0;
            // 写哈希表
            snprintf(ar->word_hash[idx].text, sizeof(ar->word_hash[idx].text), "%s", text);
            ar->word_hash[idx].word_index = ar->word_count;
            ar->word_hash[idx].used = 1;
            ar->word_hash_entries++;
            ar->word_count++;

            // 哈希表负载 > 70% 时自动扩容
            if (ar->word_hash_entries * 10 > (int)(ar->word_hash_size * 7)) {
                _ar_expand_word_hash(ar);
            }
            return we;
        }
        if (strcmp(ar->word_hash[idx].text, text) == 0) {
            return &ar->words[ar->word_hash[idx].word_index];
        }
    }
    // 理论上不会到这里（扩容机制确保了空闲槽）
    LOG_WARNING("article_reader: word hash completely full after rehash, discarding '%s'", text);
    return NULL;
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
        int bytes = get_char_bytes(p);
        if (bytes <= 0 || bytes > 4) { p++; continue; }
        // 跳过空白
        if (bytes == 1 && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            p++; continue;
        }
        // 跳过不可打印字符
        if (bytes == 1 && (*p < 32 || *p > 126)) { p++; continue; }

        // 跳过纯数字
        if (bytes == 1 && *p >= '0' && *p <= '9') { p++; continue; }

        // 跳过ASCII标点（不参与PMI词发现，减少噪声计算）
        if (bytes == 1 && is_punctuation(p)) { p++; continue; }

        char* dest = ar->line_chars[count];
        size_t cp_len = (size_t)(bytes < 7 ? bytes : 7);
        memcpy(dest, p, cp_len);
        dest[cp_len] = 0;
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

    // 扫描词表，每个词尝试创建节点（仅建节点，边缘由后续训练自动形成）
    for (int i = 0; i < ar->word_count; i++) {
        WordEntry* we = &ar->words[i];
        if (!we->text[0]) continue;

        int nid = huarong_net_find_concept(net, we->text);
        if (nid < 0) {
            // 拒绝含有标点的脏词（如 "A@yuan" 不会作为词节点）
            int has_punct = 0;
            for (const char* cp = we->text; *cp && !has_punct; ) {
                if (is_punctuation(cp)) has_punct = 1;
                int b = get_char_bytes(cp);
                if (b <= 0) b = 1;
                cp += b;
            }
            if (has_punct) continue;
            if (strstr(we->text, "//")) continue;  /* URL/路径残留 */

            /* v0.5.7: 英文垃圾词过滤（请求参数/cookie 名/乱码——实测
             * 抽查 67 个新词 ~90% 是 _G/SID/wXMLH 这类） */
            if ((unsigned char)we->text[0] < 0x80) {  /* ASCII 词 */
                const char* w = we->text;
                size_t wl = strlen(w);
                int has_vowel = 0, has_underscore = 0, has_digit = 0, has_lower = 0;
                for (const char* cp = w; *cp; cp++) {
                    char ch = *cp;
                    if (ch == '_' || ch == '-') has_underscore = 1;
                    if (ch >= '0' && ch <= '9') has_digit = 1;
                    if (islower((unsigned char)ch)) has_lower = 1;
                    if (ch=='a'||ch=='e'||ch=='i'||ch=='o'||ch=='u'||
                        ch=='A'||ch=='E'||ch=='I'||ch=='O'||ch=='U') has_vowel = 1;
                }
                if (wl < 2 || has_underscore || has_digit ||
                    (!has_vowel && wl < 6) || (!has_lower && wl >= 4)) continue;
            }

            // insert_node_dynamic：具备自动扩容 + 全局统计
            nid = insert_node_dynamic(ar->master, topo->topo_id,
                                       we->text, NULL, 0);
            if (nid >= 0 && nid < topo->net->node_count && topo->net->nodes[nid]) {
                created++;
                if (ar->p_added_nodes) (*ar->p_added_nodes)++;
                /* v0.5.7: 抽查——打印实际进拓扑的新词（污染比例分析） */
                LOG_INFO("[文章阅读] 新词抽查: %s", we->text);

                // 基于词文本的确定性特征向量初始化
                ReasoningNode* node = topo->net->nodes[nid];
                if (!node->features) {
                    node->features = (float*)malloc(NODE_FEATURE_DIM * sizeof(float));
                    node->feature_dim = NODE_FEATURE_DIM;
                }
                if (node->features) {
                    unsigned int seed = _ar_hash(we->text);
                    for (int d = 0; d < NODE_FEATURE_DIM; d++) {
                        int val = (seed * (d + 1) * 7 + 13) % 20001;
                        node->features[d] = ((float)val / 100000.0f) - 0.1f;
                    }
                }
                node->activation = we->freq > 5 ? 0.5f : 0.2f;
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

    /* 字符对哈希表动态分配 */
    ar->pair_table = (PairEntry*)calloc(AR_PAIR_HASH_SIZE, sizeof(PairEntry));
    if (!ar->pair_table) { free(ar); return NULL; }
    ar->pair_hash_size = AR_PAIR_HASH_SIZE;

    pthread_mutex_init(&ar->mutex, NULL);

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

    // 初始化词哈希表（动态扩容起始大小）
    ar->word_hash_size = AR_WORD_HASH_SIZE;
    ar->word_hash = (WordHashEntry*)calloc(ar->word_hash_size, sizeof(WordHashEntry));
    ar->word_hash_mask = ar->word_hash_size - 1;
    ar->word_hash_entries = 0;

    // 堆分配字符序列缓冲区（避免栈上 1MB + 32KB）
    ar->seq_capacity = 131072;
    ar->seq = (char(*)[8])calloc(ar->seq_capacity, 8);
    ar->seq_len = 0;

    ar->line_chars_capacity = 4096;
    ar->line_chars = (char(*)[8])calloc(ar->line_chars_capacity, 8);
    ar->line_char_count = 0;
    ar->last_flush_added = 0;
    ar->thalamus = NULL;

    // 缓存词汇拓扑指针（避免每次 flush 遍历查找）
    ar->vocab_topo = NULL;
    for (int t = 0; t < ar->master->sub_topo_count; t++) {
        if (ar->master->sub_topologies[t] &&
            ar->master->sub_topologies[t]->type == TOPO_VOCABULARY) {
            ar->vocab_topo = ar->master->sub_topologies[t];
            break;
        }
    }

    if (ar->cfg.verbose) {
        LOG_INFO("[文章阅读] 初始化: 窗口=%d PMI阈值=%.2f α=%.2f β=%.2f γ=%.2f batch=%d",
               ar->cfg.window_size, ar->cfg.pmi_threshold,
               ar->cfg.alpha, ar->cfg.beta, ar->cfg.gamma,
               ar->cfg.batch_size);
    }

    return ar;
}

void article_reader_destroy(ArticleReader* ar) {
    if (!ar) return;
    pthread_mutex_destroy(&ar->mutex);
    free(ar->words);
    free(ar->word_hash);
    free(ar->pair_table);
    free(ar->seq);
    free(ar->line_chars); // 堆分配的行缓冲区
    free(ar);
}

int article_process_line(ArticleReader* ar, const char* line) {
    if (!ar || !line) return -1;

    pthread_mutex_lock(&ar->mutex);

    /* v0.5.9: 记忆时钟 +1（字符/字符对记忆化的时间源） */
    ar->mem_tick++;

    // 提取有效字符序列
    int n = _ar_extract_chars(ar, line);
    if (n < 2) { pthread_mutex_unlock(&ar->mutex); return 0; }

    // 追加到全局序列缓冲区（供 flush 时正向扫描）
    int remain = ar->seq_capacity - ar->seq_len;
    if (remain > n) remain = n;
    for (int i = 0; i < remain; i++) {
        memcpy(ar->seq[ar->seq_len + i], ar->line_chars[i], 8);
    }
    ar->seq_len += remain;

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

    // 达到 batch 大小则触发词发现（先解锁避免 article_flush 内部加锁时死锁）
    if (ar->lines_buffered >= ar->cfg.batch_size) {
        pthread_mutex_unlock(&ar->mutex);
        int found = article_flush(ar, NULL);
        pthread_mutex_lock(&ar->mutex);
        ar->lines_buffered = 0;
        ar->last_flush_added = found > 0 ? found : 0;
        pthread_mutex_unlock(&ar->mutex);
        return found > 0 ? found : 0;
    }

    pthread_mutex_unlock(&ar->mutex);
    return 0;
}

int _article_flush_locked(ArticleReader* ar, SubTopology* topo,
                          char* pos_out[64], int* pos_out_count) {
    int _rc = -1;
    if (pos_out_count) *pos_out_count = 0;
    if (!ar) goto _flush_out;

    // 使用缓存词汇拓扑（避免每次遍历 master）
    SubTopology* vocab = topo ? topo : ar->vocab_topo;
    if (!vocab || !vocab->net) goto _flush_out;

    // 数据不足时不处理
    if (ar->total_chars < 10 || ar->seq_len < 2) {
        ar->seq_len = 0;
        _rc = 0;
        goto _flush_out;
    }

    int old_word_count = ar->word_count;

    // ============ 正向扫描：沿 seq[] 序列顺序，贪婪合并高评分相邻字符对 ============

    // 预计算相邻对的合并评分
    // pair_scores[i] = score(seq[i], seq[i+1]), 0 = 阈值以下/无效
    float* pair_scores = (float*)malloc((ar->seq_len - 1) * sizeof(float));
    if (!pair_scores) goto _flush_out;

    for (int i = 0; i < ar->seq_len - 1; i++) {
        pair_scores[i] = 0;

        PairEntry* pe = _ar_find_pair(ar, ar->seq[i], ar->seq[i + 1]);
        if (!pe || pe->co_count < ar->cfg.min_freq) continue;

        CharEntry* ce_a = _ar_find_char(ar, ar->seq[i]);
        CharEntry* ce_b = _ar_find_char(ar, ar->seq[i + 1]);
        if (!ce_a || !ce_b || ce_a->count == 0 || ce_b->count == 0) continue;

        // PMI
        float p_a  = (float)ce_a->count / ar->total_chars;
        float p_b  = (float)ce_b->count / ar->total_chars;
        float p_ab = (float)pe->co_count / (ar->total_chars - 1);
        float pmi = 0.0f;
        if (p_a > 0 && p_b > 0 && p_ab > 0) {
            pmi = logf(p_ab / (p_a * p_b));
            if (pmi < 0) pmi = 0;
        }

        // 频次奖励
        float freq_bonus = (float)pe->co_count / (pe->co_count + 5.0f);

        // 计算综合评分
        float score = ar->cfg.alpha * pmi + ar->cfg.beta * freq_bonus;

        // 连接奖励：如果附近也有高评分对（该字符对处于更长的上下文中）
        // 向前看一步：检查 seq[i+1] 与 seq[i+2] 是否也有有效 pair
        float conn_bonus = 0.0f;
        if (i + 2 < ar->seq_len) {
            PairEntry* next_pe = _ar_find_pair(ar, ar->seq[i + 1], ar->seq[i + 2]);
            if (next_pe && next_pe->co_count >= ar->cfg.min_freq) {
                conn_bonus = 1.0f;
            }
        }
        // 向后看一步：检查 seq[i-1] 与 seq[i] 是否也有有效 pair
        if (conn_bonus < 1.0f && i > 0) {
            PairEntry* prev_pe = _ar_find_pair(ar, ar->seq[i - 1], ar->seq[i]);
            if (prev_pe && prev_pe->co_count >= ar->cfg.min_freq) {
                conn_bonus = 1.0f;
            }
        }

        score += ar->cfg.gamma * conn_bonus;

        if (score > ar->cfg.pmi_threshold) {
            pair_scores[i] = score;
        }
    }

    // 正向贪婪合并
    int i = 0;
    while (i < ar->seq_len - 1) {
        if (pair_scores[i] <= 0) {
            i++;
            continue;
        }

        // 贪婪扩展至最长连续高评分段
        int end = i;
        while (end + 1 < ar->seq_len - 1 && pair_scores[end + 1] > 0) {
            end++;
        }

        // 从 seq[i] 构建词到 seq[end+1]（end 是最后一个高评分对的起始位置）
        char word_buf[AR_WORD_MAX_LEN * 4 + 1] = {0};
        int total_bytes = 0;
        for (int k = i; k <= end + 1 && k < ar->seq_len; k++) {
            size_t blen = strlen(ar->seq[k]);
            if (blen == 0 || total_bytes + (int)blen >= (int)sizeof(word_buf) - 1) break;
            memcpy(word_buf + total_bytes, ar->seq[k], blen);
            total_bytes += (int)blen;
        }
        word_buf[total_bytes] = 0;

        if ((int)utf8_strlen(word_buf) >= 2) {
            // 使用首个 pair 的共现次数作为词频基准
            PairEntry* first_pe = _ar_find_pair(ar, ar->seq[i], ar->seq[i + 1]);
            WordEntry* we = _ar_find_or_add_word(ar, word_buf);
            if (we && first_pe) {
                we->freq += first_pe->co_count;
            }
        }

        i = end + 1;
    }

    free(pair_scores);

    // ============ 一轮三字扩展（首字符哈希索引 O(n)） ============
    // 构建首字符→词索引列表（存索引而非指针，防御下方 _ar_find_or_add_word 内部 realloc）
    // P2（08-16 修复）：索引 key 从「首字节」升级为「UTF-8 前 2 字节」。
    //   中文是 3 字节编码，首字节几乎全落在 0xE4~0xE9 六个桶 → 全部单字词
    //   挤在这 6 个桶里，内层仍要遍历大量无关词（真根因 O(n²) 迭代）。
    //   取前 2 字节后有效桶 ≈ 6×64 ≈ 384，内层遍历量降约 64 倍。
    //   同时只收录单字词（char_len==1）：多字词作 w2 经 PairEntry.b[8]
    //   截断永不命中，是纯污染源（见 P0），收录无意义。
    typedef struct { int* list; int count; int cap; } CharWordList;
    // 堆分配 [256][256]（≈1MB，避免线程栈吃紧；栈上限 8MB）
    CharWordList (*first_char_map)[256] =
        (CharWordList(*)[256])calloc(256 * 256, sizeof(CharWordList));
    if (!first_char_map) goto _flush_out;  /* 分配失败 → 本轮放弃三字扩展 */
    for (int wi = 0; wi < ar->word_count; wi++) {
        WordEntry* w = &ar->words[wi];
        if (w->char_len != 1) continue;  /* P2: 只收单字词 */
        unsigned char b0 = (unsigned char)w->text[0];
        unsigned char b1 = (unsigned char)w->text[1];  /* 单字词：ASCII 时 text[1]==0 */
        CharWordList* cwl = &first_char_map[b0][b1];
        if (cwl->count >= cwl->cap) {
            int new_cap = cwl->cap ? cwl->cap * 2 : 8;
            int* tmp = (int*)realloc(cwl->list, new_cap * sizeof(int));
            if (!tmp) continue;
            cwl->list = tmp;
            cwl->cap = new_cap;
        }
        cwl->list[cwl->count++] = wi;  /* 存索引，不存指针 */
    }

    for (int wi = 0; wi < ar->word_count; wi++) {
        WordEntry* w1 = &ar->words[wi];
        if (w1->char_len < 1) continue;

        // 提取 w1 的最后一字
        const char* p = w1->text;
        for (int cp = 1; cp < w1->char_len; cp++) {
            int blen = get_char_bytes(p);
            if (blen <= 0) break;
            p += blen;
        }
        char last_c[8] = {0};
        int blen = get_char_bytes(p);
        if (blen > 0 && blen <= 4) {
            memcpy(last_c, p, blen);
            last_c[blen] = 0;
        }
        if (!last_c[0]) continue;

        // 用首字符（UTF-8 前 2 字节）哈希快速查找
        unsigned char b0 = (unsigned char)last_c[0];
        unsigned char b1 = (unsigned char)last_c[1];  /* 单字 last_c：ASCII 时 text[1]==0 */
        CharWordList* cwl = &first_char_map[b0][b1];
        for (int wi2 = 0; wi2 < cwl->count; wi2++) {
            int widx2 = cwl->list[wi2];
            if (wi == widx2) continue;  /* 用索引去重，防御 realloc */
            WordEntry* w2 = &ar->words[widx2];  /* 每次从最新 ar->words 取，防御 realloc */
            if (w2->char_len < 1) continue;
            /* P0（08-16 修复）：只处理单字词。多字词作 b 经 PairEntry.b[8]
             * （仅 7 字节）strncpy 截断 → 下次 strcmp 整词 vs 截断串永不相等
             * → 每次 flush 重复插入 char|word 垃圾条目 → pair_count 无界增长。 */
            if (w2->char_len != 1) continue;

            // 检查 pair_table 中该字对是否高频
            PairEntry* pe = _ar_find_pair(ar, last_c, w2->text);
            if (!pe || pe->co_count < ar->cfg.min_freq) continue;

            // 合并 — w1 也每次从索引读，防御内层循环中 _ar_find_or_add_word 的 realloc
            size_t w1len = strlen(ar->words[wi].text);
            size_t w2len = strlen(w2->text);
            if (w1len + w2len >= AR_WORD_MAX_LEN * 4 + 1) continue;

            char combined[AR_WORD_MAX_LEN * 4 + 1] = {0};
            memcpy(combined, ar->words[wi].text, w1len);
            memcpy(combined + w1len, w2->text, w2len);
            combined[w1len + w2len] = 0;

            if ((int)utf8_strlen(combined) <= AR_WORD_MAX_LEN) {
                WordEntry* we = _ar_find_or_add_word(ar, combined);
                if (we) we->freq += pe->co_count;
            }
        }
    }

    // 释放首字符索引（每桶动态 list + 2D 数组本体）
    for (int i = 0; i < 256; i++) {
        for (int j = 0; j < 256; j++) {
            free(first_char_map[i][j].list);
        }
    }
    free(first_char_map);

    // 清空序列缓冲区（下一轮重新累积）
    ar->seq_len = 0;

    int new_words = ar->word_count - old_word_count;

    // 建图
    int built = _ar_build_topo(ar, vocab);
    if (built > 0) {
        if (ar->cfg.verbose) {
            LOG_INFO("[文章阅读] 刷新: %d 新词, 共 %d 词, 累计 %d 不同字符, %d 字符对",
                   new_words, ar->word_count,
                   ar->char_count, ar->pair_count);

        }

        // === v0.5.8: 新词 → POS 池（锁内只收集概念名，tag_soft 锁外执行）===
        // 此前 POS 池只吃生成/对话路径（Broca/cognitive_controller），
        // feed/learn 完全绕开 → 玄枢不会说话 → POS 池空 → 聚类永不触发
        // （额外词类 08-04 至今为 0）。tag_soft 含 O(n²) 聚类，若在锁内
        // 执行会阻塞学习线程（08-07 32 线程堆积实锤：每 500 次调用
        // 聚类 256² 矩阵 2-5 秒）→ 改锁内收集 / article_flush 锁外喂养。
        if (new_words > 0 && ar->emergent_pos && pos_out && pos_out_count) {
            HuarongTopologyNet* vnet = vocab->net;
            for (int wi = 0; wi < ar->word_count && *pos_out_count < 64; wi++) {
                WordEntry* we = &ar->words[wi];
                if (!we->text[0]) continue;
                if (huarong_net_find_concept(vnet, we->text) < 0) continue;
                pos_out[*pos_out_count] = strdup(we->text);
                if (pos_out[*pos_out_count]) (*pos_out_count)++;
            }
        }
        // === POS 池收集结束 ===

        // === 新词 → 模板反馈：为新词建立与已有模板的跨拓扑连接 ===
        if (new_words > 0 && ar->master && ar->master->use_template_voting) {
            HuarongTopologyNet* vnet = vocab->net;
            int tpl_matched = 0;
            for (int wi = 0; wi < ar->word_count; wi++) {
                WordEntry* we = &ar->words[wi];
                if (!we->text[0]) continue;
                int nid_cur = huarong_net_find_concept(vnet, we->text);
                if (nid_cur < 0) continue;
                /* 与相邻前一个词配对匹配模板 */
                if (wi > 0) {
                    WordEntry* prev = &ar->words[wi - 1];
                    if (prev->text[0]) {
                        int nid_prev = huarong_net_find_concept(vnet, prev->text);
                        if (nid_prev >= 0) {
                            int tpl_id = master_find_template_for_pair(
                                ar->master, vocab->topo_id, nid_prev, nid_cur);
                            if (tpl_id >= 0) {
                                master_add_cross_link(ar->master,
                                    vocab->topo_id, nid_prev,
                                    TOPO_TEMPLATE, tpl_id, 0.5f, "article_anchor_a");
                                master_add_cross_link(ar->master,
                                    vocab->topo_id, nid_cur,
                                    TOPO_TEMPLATE, tpl_id, 0.4f, "article_anchor_b");
                                tpl_matched++;
                            }
                        }
                    }
                }
            }
            if (ar->cfg.verbose && tpl_matched > 0) {
                LOG_INFO("[文章阅读] 模板匹配: %d 个新词关联到已有模板", tpl_matched);
            }
        }
        // === 模板反馈结束 ===

        // === 通过丘脑信号总线报告工作量（让调度系统感知认知负载） ===
        if (ar->thalamus) {
            thalamus_send_feedback(ar->thalamus, THAL_HIPPOCAMPUS,
                                   built, 0, 0);
        }
        // === 丘脑反馈结束 ===
    }

    _rc = new_words;
_flush_out:
    /* v0.5.8: 统一出口——调用方（article_flush 834-836）持有 ar->mutex，
     * 任何提前 return 都会跳过调用方的 unlock → ar->mutex 永久持有
     * → 学习线程堆积死锁（08-07 274 线程雪崩实锤） */
    return _rc;
}

int article_flush(ArticleReader* ar, SubTopology* topo) {
    if (!ar) return -1;
    char* pos_words[64];
    int   pos_n = 0;
    pthread_mutex_lock(&ar->mutex);
    int result = _article_flush_locked(ar, topo, pos_words, &pos_n);
    pthread_mutex_unlock(&ar->mutex);

    /* v0.5.8: 锁外 POS 池喂养——emergent_pos_tag_soft 含 O(n²) 聚类
     * （每 500 次调用聚类 256² 相似度矩阵 2-5 秒），若在 ar->mutex
     * 锁内执行会阻塞学习线程 → 线程堆积（08-07 32 线程实锤）。
     * 锁内只收集概念名（strdup），此处解锁后统一查询词性进池。 */
    if (ar->emergent_pos) {
        for (int i = 0; i < pos_n; i++) {
            SoftClassResult soft;
            memset(&soft, 0, sizeof(soft));
            emergent_pos_tag_soft(ar->emergent_pos, ar->master,
                                  pos_words[i], &soft);
            free(pos_words[i]);
        }
        if (ar->cfg.verbose && pos_n > 0) {
            LOG_INFO("[文章阅读] POS 池喂养: %d 个新词查询词性", pos_n);
        }
    }
    return result;
}

void article_set_progress_ptr(ArticleReader* ar,
                              long* total_added_nodes_ptr,
                              long* total_added_edges_ptr) {
    if (!ar) return;
    ar->p_added_nodes = total_added_nodes_ptr;
    ar->p_added_edges = total_added_edges_ptr;
}

void article_reader_set_thalamus(ArticleReader* ar, Thalamus* th) {
    if (ar) ar->thalamus = th;
}

void article_reader_set_emergent_pos(ArticleReader* ar, EmergentPOS* ep) {
    if (ar) ar->emergent_pos = ep;
}

void article_get_stats(ArticleReader* ar,
                       int* out_chars, int* out_pairs, int* out_words) {
    if (!ar) return;
    pthread_mutex_lock(&ar->mutex);
    if (out_chars) *out_chars = ar->char_count;
    if (out_pairs) *out_pairs = ar->pair_count;
    if (out_words) *out_words = ar->word_count;
    pthread_mutex_unlock(&ar->mutex);
}

int article_get_last_flush_added(ArticleReader* ar) {
    if (!ar) return -1;
    pthread_mutex_lock(&ar->mutex);
    int val = ar->last_flush_added;
    pthread_mutex_unlock(&ar->mutex);
    return val;
}
