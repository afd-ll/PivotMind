/**
 * @file charpair.c
 * @brief 字对计数表实现 —— 见 include/charpair.h（键=概念字符串对，非 node_id）
 *
 * 结构：开放寻址哈希（键槽内直接存两个字的字节，碰撞时逐字节比较），
 * 线性探测，空槽 = count==0。整表一把互斥锁，观测与存盘并发安全。
 */

#include "charpair.h"
#include "pivotmind_paths.h"
#include "error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define CP_MAGIC      0x504D4350u   /* "PMCP" */
#define CP_VERSION    3u            /* v3：键上限 8→32，中英双轨 token（旧表自动作废重建） */
#define CPT_INIT_CAP  1024
#define CP_MAXB       32            /* 单个 token 的字节上限（中文单字 3；英文词 ≤31） */
#define CP_TOKEN_MAX  512

typedef struct {
    char     a[CP_MAXB];
    char     b[CP_MAXB];
    uint32_t count;
} CpEntry;

struct CharPairTable {
    CpEntry* slots;
    int      capacity;   /* 2 的幂 */
    int      size;
    pthread_mutex_t lock;
};

static uint64_t cpt_hash(const char* a, const char* b) {
    uint64_t h = 1469598103934665603ULL;   /* FNV-1a 64 */
    for (const char* p = a; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
    h ^= 0x5F; h *= 1099511628211ULL;      /* 分隔符，避免 ("ab","c") 与 ("a","bc") 撞 */
    for (const char* p = b; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;
    return h;
}

/* 返回槽位下标；found 置 1 表示命中。调用方持锁。 */
static int cpt_find(const CharPairTable* t, const char* a, const char* b, int* found) {
    uint64_t mask = (uint64_t)(t->capacity - 1);
    int idx = (int)(cpt_hash(a, b) & mask);
    for (int probe = 0; probe < t->capacity; probe++) {
        int i = (int)(((uint64_t)idx + (uint64_t)probe) & mask);
        if (t->slots[i].count == 0) { *found = 0; return i; }
        if (strcmp(t->slots[i].a, a) == 0 && strcmp(t->slots[i].b, b) == 0) { *found = 1; return i; }
    }
    *found = 0;
    return -1;
}

static int cpt_grow(CharPairTable* t) {
    int nc = t->capacity * 2;
    CpEntry* ns = (CpEntry*)calloc((size_t)nc, sizeof(CpEntry));
    if (!ns) return -1;
    uint64_t mask = (uint64_t)(nc - 1);
    for (int i = 0; i < t->capacity; i++) {
        if (t->slots[i].count == 0) continue;
        int idx = (int)(cpt_hash(t->slots[i].a, t->slots[i].b) & mask);
        for (int p = 0; p < nc; p++) {
            int j = (int)(((uint64_t)idx + (uint64_t)p) & mask);
            if (ns[j].count == 0) { ns[j] = t->slots[i]; break; }
        }
    }
    free(t->slots);
    t->slots = ns;
    t->capacity = nc;
    return 0;
}

CharPairTable* cpt_create(void) {
    CharPairTable* t = (CharPairTable*)calloc(1, sizeof(CharPairTable));
    if (!t) return NULL;
    t->slots = (CpEntry*)calloc(CPT_INIT_CAP, sizeof(CpEntry));
    if (!t->slots) { free(t); return NULL; }
    t->capacity = CPT_INIT_CAP;
    pthread_mutex_init(&t->lock, NULL);
    return t;
}

void cpt_destroy(CharPairTable* t) {
    if (!t) return;
    free(t->slots);
    pthread_mutex_destroy(&t->lock);
    free(t);
}

uint32_t cpt_observe(CharPairTable* t, const char* a, const char* b) {
    if (!t || !a || !b || !a[0] || !b[0]) return 0;
    if (strlen(a) >= CP_MAXB || strlen(b) >= CP_MAXB) return 0;
    if (strcmp(a, b) == 0) return 0;                 /* 自环不计（与既有连边判据一致） */
    pthread_mutex_lock(&t->lock);
    if ((t->size + 1) * 10 >= t->capacity * 7) {
        if (cpt_grow(t) != 0) { pthread_mutex_unlock(&t->lock); return 0; }
    }
    int found = 0;
    int i = cpt_find(t, a, b, &found);
    if (i < 0) { pthread_mutex_unlock(&t->lock); return 0; }
    if (!found) {
        snprintf(t->slots[i].a, CP_MAXB, "%s", a);
        snprintf(t->slots[i].b, CP_MAXB, "%s", b);
        t->slots[i].count = 0;
        t->size++;
    }
    t->slots[i].count++;
    uint32_t c = t->slots[i].count;
    pthread_mutex_unlock(&t->lock);
    return c;
}

uint32_t cpt_get(const CharPairTable* t, const char* a, const char* b) {
    if (!t || !a || !b) return 0;
    pthread_mutex_lock((pthread_mutex_t*)&t->lock);
    int found = 0;
    int i = cpt_find(t, a, b, &found);
    uint32_t c = (found && i >= 0) ? t->slots[i].count : 0;
    pthread_mutex_unlock((pthread_mutex_t*)&t->lock);
    return c;
}

int cpt_pair_count(const CharPairTable* t) { return t ? t->size : 0; }

void cpt_foreach(const CharPairTable* t,
                 void (*fn)(const char* a, const char* b, uint32_t c, void* ud), void* ud) {
    if (!t || !fn) return;
    pthread_mutex_lock((pthread_mutex_t*)&t->lock);
    for (int i = 0; i < t->capacity; i++)
        if (t->slots[i].count > 0) fn(t->slots[i].a, t->slots[i].b, t->slots[i].count, ud);
    pthread_mutex_unlock((pthread_mutex_t*)&t->lock);
}

int cpt_save(const CharPairTable* t, const char* path) {
    if (!t || !path) return -1;
    FILE* fp = fopen(path, "wb");
    if (!fp) return -1;
    pthread_mutex_lock((pthread_mutex_t*)&t->lock);
    uint32_t hdr[3] = { CP_MAGIC, CP_VERSION, (uint32_t)t->size };
    if (fwrite(hdr, sizeof(uint32_t), 3, fp) != 3) { pthread_mutex_unlock((pthread_mutex_t*)&t->lock); fclose(fp); return -1; }
    for (int i = 0; i < t->capacity; i++) {
        if (t->slots[i].count == 0) continue;
        unsigned char la = (unsigned char)strlen(t->slots[i].a);
        unsigned char lb = (unsigned char)strlen(t->slots[i].b);
        if (fwrite(&la, 1, 1, fp) != 1 || fwrite(t->slots[i].a, 1, la, fp) != la ||
            fwrite(&lb, 1, 1, fp) != 1 || fwrite(t->slots[i].b, 1, lb, fp) != lb ||
            fwrite(&t->slots[i].count, sizeof(uint32_t), 1, fp) != 1) {
            pthread_mutex_unlock((pthread_mutex_t*)&t->lock);
            fclose(fp);
            return -1;
        }
    }
    pthread_mutex_unlock((pthread_mutex_t*)&t->lock);
    fclose(fp);
    return 0;
}

int cpt_load(CharPairTable* t, const char* path) {
    if (!t || !path) return -1;
    FILE* fp = fopen(path, "rb");
    if (!fp) return -1;
    uint32_t hdr[3] = {0};
    /* 兼容 v2（键 ≤7 字节的中文单字表）：读得进，存盘时统一按 CP_VERSION 写。
     * 2026-10-27 实测线上已有 288,468 条 v2 数据，直接作废等于把历史统计扔了。 */
    if (fread(hdr, sizeof(uint32_t), 3, fp) != 3 || hdr[0] != CP_MAGIC ||
        (hdr[1] != CP_VERSION && hdr[1] != 2u)) {
        fclose(fp);
        LOG_WARNING("[charpair] %s 头无效（旧格式或损坏），忽略", path);
        return -1;
    }
    uint32_t n = hdr[2];
    int loaded = 0;
    for (uint32_t i = 0; i < n; i++) {
        unsigned char la = 0, lb = 0;
        char a[CP_MAXB] = {0}, b[CP_MAXB] = {0};
        uint32_t c = 0;
        if (fread(&la, 1, 1, fp) != 1 || la >= CP_MAXB) break;
        if (fread(a, 1, la, fp) != la) break;
        a[la] = '\0';
        if (fread(&lb, 1, 1, fp) != 1 || lb >= CP_MAXB) break;
        if (fread(b, 1, lb, fp) != lb) break;
        b[lb] = '\0';
        if (fread(&c, sizeof(uint32_t), 1, fp) != 1) break;
        pthread_mutex_lock(&t->lock);
        if ((t->size + 1) * 10 >= t->capacity * 7) cpt_grow(t);
        int found = 0;
        int idx = cpt_find(t, a, b, &found);
        if (idx >= 0) {
            if (!found) { snprintf(t->slots[idx].a, CP_MAXB, "%s", a); snprintf(t->slots[idx].b, CP_MAXB, "%s", b); t->size++; }
            t->slots[idx].count = c;
            loaded++;
        }
        pthread_mutex_unlock(&t->lock);
    }
    fclose(fp);
    LOG_INFO("[charpair] 已加载 %d 条字对计数（%s，字符串键 v%u）", loaded, path, CP_VERSION);
    return 0;
}

/* ==================== 全局单例 ==================== */

static CharPairTable* g_cpt = NULL;
static pthread_mutex_t g_cpt_lock = PTHREAD_MUTEX_INITIALIZER;

/* 与 PROB-ASSIST 共用开关：PIVOTMIND_PROB_ASSIST=0 时整体关。 */
static int cpt_enabled(void) {
    const char* e = getenv("PIVOTMIND_PROB_ASSIST");
    return !(e && e[0] == '0');
}

CharPairTable* cpt_global(void) {
    if (!cpt_enabled()) return NULL;
    pthread_mutex_lock(&g_cpt_lock);
    if (!g_cpt) {
        g_cpt = cpt_create();
        if (g_cpt) {
            const char* p = pm_file(PM_FILE_CHARPAIRS);
            if (p) cpt_load(g_cpt, p);      /* 不存在/旧格式：空表起步 */
        }
    }
    CharPairTable* r = g_cpt;
    pthread_mutex_unlock(&g_cpt_lock);
    return r;
}

void cpt_global_shutdown_save(void) {
    pthread_mutex_lock(&g_cpt_lock);
    if (g_cpt) {
        const char* p = pm_file(PM_FILE_CHARPAIRS);
        if (p) {
            if (cpt_save(g_cpt, p) == 0)
                LOG_INFO("[charpair] 已保存 %d 条字对计数（%s，字符串键）", cpt_pair_count(g_cpt), p);
            else
                LOG_WARNING("[charpair] 存盘失败：%s", p);
        }
    }
    pthread_mutex_unlock(&g_cpt_lock);
}

int cp_observe_tokens(const char* text) {
    /* v0.6.12：从「只计中文单字」扩成「中英双轨 token 对」——
     *   · CJK 单字     → 一个 token（3 字节）
     *   · 拉丁字母/数字 → 一个 token（小写化，≤31 字节；英文按空格已是词）
     *   · 空白/标点/emoji/其它文字 → 断链（不产生跨类假对）
     * 中文语义不变（仍是逐字成对）；英文从此也能算 P(w) 与左右熵。
     * 为什么英文也要：左熵/右熵判的是「这个单位的搭配自由度」，
     * 对英文即「搭配词是否多样」—— 同一个公式，换切分单位即可。 */
    if (!text || !text[0]) return 0;
    CharPairTable* t = cpt_global();
    if (!t) return 0;

    char prev[CP_MAXB] = {0};
    int used = 0, pairs = 0;
    const char* p = text;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x80) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_') {
                char one[CP_MAXB];
                int n = 0;
                while (*p) {
                    unsigned char d = (unsigned char)*p;
                    if (!((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') ||
                          (d >= '0' && d <= '9') || d == '_')) break;
                    if (n < CP_MAXB - 1) one[n++] = (char)((d >= 'A' && d <= 'Z') ? (char)(d + 32) : (char)d);
                    p++;
                }
                one[n] = '\0';
                if (prev[0]) { cpt_observe(t, prev, one); pairs++; }
                snprintf(prev, CP_MAXB, "%s", one);
                used++;
                continue;
            }
            /* 空白 = 软分隔：中文"甲 乙"与英文"machine learning"都靠它分词，
             * 因此**保留 prev**，让跨空白的相邻 token 仍成对（英文 bigram 靠这条）。
             * 标点才是硬断链。 */
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { p++; continue; }
            p++;                      /* 标点/其它 ASCII：断链 */
            prev[0] = '\0';
            continue;
        }
        int len = (c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : ((c & 0xF8) == 0xF0 ? 4 : 1));
        /* ⚠ 3 字节 ≠ 汉字：全角标点（，。：；！？、）与假名都是 3 字节。
         * 只认 U+4E00..U+9FFF，其余当断链 —— 否则会造出「者：」这类「字+标点」对，
         * 既污染 PMI/左右熵，又会被词晋升当成真词建进词表。 */
        if (len == 3) {
            unsigned int cp3 = ((unsigned int)(p[0] & 0x0F) << 12)
                             | ((unsigned int)(p[1] & 0x3F) << 6)
                             |  (unsigned int)(p[2] & 0x3F);
            if (cp3 < 0x4E00 || cp3 > 0x9FFF) { p += 3; prev[0] = 0; continue; }
            char one[CP_MAXB] = {0};
            memcpy(one, p, 3);
            p += 3;
            if (prev[0]) { cpt_observe(t, prev, one); pairs++; }
            snprintf(prev, CP_MAXB, "%s", one);
            used++;
            continue;
        }
        p += len;                     /* 2 字节拉丁扩展 / 4 字节 emoji：断链 */
        prev[0] = '\0';
    }
    {
        static int cached = -1;
        if (cached < 0) { const char* e = getenv("CP_LOG"); cached = (e && e[0] == '1'); }
        if (cached)
            fprintf(stderr, "[token对] 计数token=%d 相邻对=%d 文本：%.40s\n", used, pairs, text);
    }
    return used;
}

int cp_observe_text(const char* text) {
    /* 保留旧名（article_reader 在用）：语义已扩为 token 双轨，中文行为不变。 */
    return cp_observe_tokens(text);
}
