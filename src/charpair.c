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
#define CP_VERSION    2u
#define CPT_INIT_CAP  1024
#define CP_MAXB       8             /* 单个字的字节上限（中文单字 3） */
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
    if (fread(hdr, sizeof(uint32_t), 3, fp) != 3 || hdr[0] != CP_MAGIC || hdr[1] != CP_VERSION) {
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

int cp_observe_text(const char* text) {
    /* v0.6.11 起不再需要词汇拓扑：按文本直接计。原来的 vocab 形参从未被读过
     * （只写 (void)vocab 丢掉），接线检查判据 B 会把它记成「显式丢弃」⇒ 直接删形参。 */
    if (!text || !text[0]) return 0;
    CharPairTable* t = cpt_global();
    if (!t) return 0;

    /* 逐字取中文单字（3 字节 UTF-8），相邻成对计数 */
    char prev[CP_MAXB] = {0};
    int used = 0, pairs = 0;
    for (const char* p = text; *p; ) {
        unsigned char c = (unsigned char)*p;
        int len = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
        char one[CP_MAXB] = {0};
        if (len < CP_MAXB) { memcpy(one, p, (size_t)len); one[len] = '\0'; }
        p += len;
        if (len != 3) { prev[0] = '\0'; continue; }   /* 非中文单字 ⇒ 断开相邻关系 */
        if (prev[0]) { cpt_observe(t, prev, one); pairs++; }
        snprintf(prev, CP_MAXB, "%s", one);
        used++;
    }
    {
        static int cached = -1;
        if (cached < 0) { const char* e = getenv("CP_LOG"); cached = (e && e[0] == '1'); }
        if (cached)
            fprintf(stderr, "[字对] 计数字数=%d 相邻对=%d 文本：%.40s\n", used, pairs, text);
    }
    return used;
}
