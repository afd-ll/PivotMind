/**
 * @file word_embed.c
 * @brief 邻词分布指纹（一阶分布表示）—— 见 word_embed.h 的设计说明
 *
 * 数据来源：charpairs.bin（字符级左右邻共现，线上 83 万对 / 1.1 亿次观测）。
 * 为什么不直接用拓扑边：实测词节点的边是「同句共现的兄弟」（电脑↔屏幕↔坏了），
 * 不含位置方向；而 charpairs 是**有序**的字对（a→b），才分得出「左邻/右邻」。
 */
#include "word_embed.h"
#include "charpair.h"
#include "pivotmind_paths.h"
#include "error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define WEMB_F           300      /* 特征词数（高频单字） */
#define WEMB_DIM         (WEMB_F * 2)
#define WEMB_MAX_WORDS   8000
#define WEMB_HASH        16384    /* 2^14，装载 6000 项 ≈ 0.37 */
#define WEMB_MAGIC       0x45574D50u   /* "PMWE" */
#define WEMB_MIN_NNZ     3               /* 低于此非零维数 ⇒ 余弦不可信 */
#define WEMB_VERSION     1u

static float*        g_vec   = NULL;          /* nwords × WEMB_DIM */
static char        (*g_words)[4] = NULL;      /* nwords 个词，单字最多 3 字节 */
static uint64_t*     g_freq  = NULL;
static uint16_t*     g_nnz   = NULL;          /* 每行非零维数（判稀疏度） */          /* 词的自身频次（PPMI 分母） */
static int           g_nwords = 0;
static int           g_ready  = 0;
static int           g_nfeat  = 0;
static char          g_feat[WEMB_F][4];
static int           g_feat_row[WEMB_F];      /* 特征词在词表里的行号（PPMI 取频次用） */
static uint16_t      g_hidx[WEMB_HASH];       /* 开放寻址：存 row+1，0=空 */
static uint64_t      g_total  = 0;            /* 全部字对计数之和 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;   /* 只护「首次构建」 */

/* ---------------- 基础工具 ---------------- */

static uint32_t _hash(const char* s) {
    uint32_t h = 2166136261u;              /* FNV-1a */
    while (s && *s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

/** 只认单字 CJK（U+4E00..U+9FFF，3 字节 UTF-8）。多字词不建指纹。 */
static int _is_cjk1(const char* s) {
    if (!s || strlen(s) != 3) return 0;
    unsigned int cp = ((unsigned int)((unsigned char)s[0] & 0x0F) << 12)
                    | ((unsigned int)((unsigned char)s[1] & 0x3F) << 6)
                    |  (unsigned int)((unsigned char)s[2] & 0x3F);
    return cp >= 0x4E00 && cp <= 0x9FFF;
}

static int _find_row(const char* w) {
    if (!w || !w[0]) return -1;
    uint32_t h = _hash(w) & (WEMB_HASH - 1);
    uint32_t start = h;
    while (g_hidx[h]) {
        int r = (int)g_hidx[h] - 1;
        if (r < g_nwords && strcmp(g_words[r], w) == 0) return r;
        h = (h + 1) & (WEMB_HASH - 1);
        if (h == start) break;
    }
    return -1;
}

static int _add_row(const char* w) {
    if (!w || !w[0] || strlen(w) >= 4) return -1;
    int ex = _find_row(w);
    if (ex >= 0) return ex;
    if (g_nwords >= WEMB_MAX_WORDS) return -1;
    uint32_t h = _hash(w) & (WEMB_HASH - 1);
    uint32_t start = h;
    while (g_hidx[h]) {
        h = (h + 1) & (WEMB_HASH - 1);
        if (h == start) return -1;
    }
    int r = g_nwords++;
    snprintf(g_words[r], 4, "%s", w);
    g_hidx[h] = (uint16_t)(r + 1);
    return r;
}

static int _feat_idx(const char* w) {
    for (int i = 0; i < g_nfeat; i++)
        if (strcmp(g_feat[i], w) == 0) return i;
    return -1;
}

static void _reset(void) {
    free(g_vec);   g_vec = NULL;
    free(g_words); g_words = NULL;
    free(g_freq);  g_freq = NULL;
    free(g_nnz);   g_nnz = NULL;
    g_nwords = 0; g_ready = 0; g_nfeat = 0; g_total = 0;
    memset(g_hidx, 0, sizeof(g_hidx));
    memset(g_feat, 0, sizeof(g_feat));
    for (int i = 0; i < WEMB_F; i++) g_feat_row[i] = -1;
}

static int _alloc_tables(void) {
    g_vec = (float*)calloc((size_t)WEMB_MAX_WORDS * WEMB_DIM, sizeof(float));
    g_words = (char (*)[4])calloc(WEMB_MAX_WORDS, 4);
    g_freq = (uint64_t*)calloc(WEMB_MAX_WORDS, sizeof(uint64_t));
    g_nnz  = (uint16_t*)calloc(WEMB_MAX_WORDS, sizeof(uint16_t));
    if (!g_vec || !g_words || !g_freq || !g_nnz) { _reset(); return -1; }
    return 0;
}

static void _default_path(char* out, size_t n) {
    snprintf(out, n, "%s/data/word_embed.bin", pm_home());
}

/* ---------------- 构建 ---------------- */

typedef struct { uint64_t total; } _Pass1;

static void _cb_freq(const char* a, const char* b, uint32_t c, void* ud) {
    _Pass1* p = (_Pass1*)ud;
    if (!_is_cjk1(a) || !_is_cjk1(b)) return;      /* 只计单字对 */
    p->total += c;
    int ra = _add_row(a);
    int rb = _add_row(b);
    if (ra >= 0) g_freq[ra] += c;
    if (rb >= 0) g_freq[rb] += c;
}

typedef struct { long skipped; } _Pass2;

static void _cb_matrix(const char* a, const char* b, uint32_t c, void* ud) {
    _Pass2* p2 = (_Pass2*)ud;
    if (!_is_cjk1(a) || !_is_cjk1(b)) return;
    int ia = _feat_idx(a), ib = _feat_idx(b);
    if (ia < 0 && ib < 0) {                 /* 两侧都不是特征词 ⇒ 这一对没有位置信息 */
        if (p2) p2->skipped++;
        return;
    }
    int ra = _find_row(a), rb = _find_row(b);
    if (ra >= 0 && ib >= 0) g_vec[(size_t)ra * WEMB_DIM + WEMB_F + ib] += (float)c;  /* a 的右邻 b */
    if (rb >= 0 && ia >= 0) g_vec[(size_t)rb * WEMB_DIM + ia]            += (float)c; /* b 的左邻 a */
}

static int _cmp_freq(const void* pa, const void* pb) {
    int ra = *(const int*)pa, rb = *(const int*)pb;
    if (g_freq[ra] == g_freq[rb]) return 0;
    return (g_freq[ra] < g_freq[rb]) ? 1 : -1;      /* 降序 */
}

static void _pick_features(void) {
    /* 按频次降序选 top-F。用 qsort 而不是选择排序（8000×200 会到 1.6 亿次比较）。 */
    int* idx = (int*)malloc(sizeof(int) * (size_t)g_nwords);
    if (!idx) return;
    for (int r = 0; r < g_nwords; r++) idx[r] = r;
    qsort(idx, (size_t)g_nwords, sizeof(int), _cmp_freq);
    int np = (g_nwords < WEMB_F) ? g_nwords : WEMB_F;
    g_nfeat = np;
    for (int i = 0; i < np; i++) {
        snprintf(g_feat[i], 4, "%s", g_words[idx[i]]);
        g_feat_row[i] = idx[i];      /* 预存行号，省掉 PPMI 里的重复查表 */
    }
    free(idx);
}

static void _ppmi_normalize(void) {
    if (g_total == 0) return;
    for (int r = 0; r < g_nwords; r++) {
        float* v = g_vec + (size_t)r * WEMB_DIM;
        double nrm = 0.0;
        for (int k = 0; k < WEMB_DIM; k++) {
            float c = v[k];
            if (c <= 0.0f) { v[k] = 0.0f; continue; }
            int fi = (k < WEMB_F) ? k : (k - WEMB_F);
            double fq = (fi < g_nfeat && g_feat_row[fi] >= 0)
                      ? (double)g_freq[g_feat_row[fi]] : 1.0;   /* 特征词频次（预存行号） */
            double fw = (double)g_freq[r];
            if (fq < 1.0) fq = 1.0;
            if (fw < 1.0) fw = 1.0;
            /* PPMI（A/B 结论：不要换成 sqrt(TF)×IDF）。
             * 实测对比 —— 类内/跨类平均余弦比：
             *   PPMI        : 0.104 / 0.025 = **4.15x**（区分度成立）
             *   sqrt(TF)IDF : 0.449 / 0.342 = 1.31x（区分度不足，类内类间一起虚高）
             * 所以保留 PPMI 的 0 截断 —— 稀疏恰恰是它区分得开的原因。 */
            double pmi = log(((double)c * (double)g_total) / (fw * fq));
            v[k] = (pmi > 0.0) ? (float)pmi : 0.0f;
            nrm += (double)v[k] * (double)v[k];
        }
        int nnz = 0;
        for (int k = 0; k < WEMB_DIM; k++) if (v[k] != 0.0f) nnz++;
        g_nnz[r] = (uint16_t)nnz;
        if (nrm > 1e-12) {
            float inv = (float)(1.0 / sqrt(nrm));
            for (int k = 0; k < WEMB_DIM; k++) v[k] *= inv;
        }
    }
}

int wemb_build(void) {
    CharPairTable* cp = cpt_global();
    if (!cp) return -1;
    pthread_mutex_lock(&g_lock);
    _reset();
    if (_alloc_tables() != 0) { pthread_mutex_unlock(&g_lock); return -1; }

    _Pass1 p1 = {0};
    cpt_foreach(cp, _cb_freq, &p1);
    g_total = p1.total;
    if (g_nwords <= 0 || g_total == 0) {
        _reset();
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    _pick_features();
    _Pass2 p2 = {0};
    cpt_foreach(cp, _cb_matrix, &p2);
    _ppmi_normalize();

    g_ready = 1;
    int n = g_nwords, nf = g_nfeat;
    long skipped = p2.skipped;
    pthread_mutex_unlock(&g_lock);
    LOG_INFO("[指纹] 构建完成: %d 词 × %d 维（特征词 %d，总计数 %llu，无特征邻居跳过 %ld 对）",
             n, WEMB_DIM, nf, (unsigned long long)p1.total, skipped);
    return n;
}

/* ---------------- 存 / 读 ---------------- */

int wemb_save(const char* path) {
    char buf[1024];
    if (!path) { _default_path(buf, sizeof(buf)); path = buf; }
    pthread_mutex_lock(&g_lock);
    if (!g_ready || g_nwords <= 0) { pthread_mutex_unlock(&g_lock); return -1; }
    FILE* fp = fopen(path, "wb");
    if (!fp) { pthread_mutex_unlock(&g_lock); return -1; }
    uint32_t hdr[5] = { WEMB_MAGIC, WEMB_VERSION, (uint32_t)g_nwords,
                        (uint32_t)WEMB_DIM, (uint32_t)g_nfeat };
    int ok = (fwrite(hdr, sizeof(uint32_t), 5, fp) == 5);
    if (ok) ok = (fwrite(g_feat, 1, sizeof(g_feat), fp) == sizeof(g_feat));
    if (ok) ok = (fwrite(g_words, 1, (size_t)g_nwords * 4, fp) == (size_t)g_nwords * 4);
    if (ok) ok = (fwrite(g_vec, sizeof(float), (size_t)g_nwords * WEMB_DIM, fp)
                  == (size_t)g_nwords * WEMB_DIM);
    fclose(fp);
    pthread_mutex_unlock(&g_lock);
    if (!ok) { LOG_ERROR("[指纹] 存盘失败: %s", path); return -1; }
    LOG_INFO("[指纹] 已存盘 %s（%d 词）", path, g_nwords);
    return 0;
}

int wemb_load(const char* path) {
    char buf[1024];
    if (!path) { _default_path(buf, sizeof(buf)); path = buf; }
    FILE* fp = fopen(path, "rb");
    if (!fp) return -1;
    uint32_t hdr[5] = {0};
    if (fread(hdr, sizeof(uint32_t), 5, fp) != 5 ||
        hdr[0] != WEMB_MAGIC || hdr[1] != WEMB_VERSION ||
        hdr[3] != WEMB_DIM || hdr[2] > WEMB_MAX_WORDS || hdr[4] > WEMB_F) {
        fclose(fp);
        LOG_WARNING("[指纹] %s 头无效（版本/维度不符），忽略", path);
        return -1;
    }
    pthread_mutex_lock(&g_lock);
    _reset();
    if (_alloc_tables() != 0) { pthread_mutex_unlock(&g_lock); fclose(fp); return -1; }
    g_nwords = (int)hdr[2];
    g_nfeat  = (int)hdr[4];
    int ok = (fread(g_feat, 1, sizeof(g_feat), fp) == sizeof(g_feat));
    if (ok) ok = (fread(g_words, 1, (size_t)g_nwords * 4, fp) == (size_t)g_nwords * 4);
    if (ok) ok = (fread(g_vec, sizeof(float), (size_t)g_nwords * WEMB_DIM, fp)
                  == (size_t)g_nwords * WEMB_DIM);
    if (!ok) {
        _reset(); pthread_mutex_unlock(&g_lock); fclose(fp);
        LOG_ERROR("[指纹] %s 数据不完整", path);
        return -1;
    }
    /* 重建 hash 索引 */
    memset(g_hidx, 0, sizeof(g_hidx));
    for (int r = 0; r < g_nwords; r++) {
        uint32_t h = _hash(g_words[r]) & (WEMB_HASH - 1);
        uint32_t start = h;
        while (g_hidx[h]) {
            h = (h + 1) & (WEMB_HASH - 1);
            if (h == start) break;
        }
        if (!g_hidx[h]) g_hidx[h] = (uint16_t)(r + 1);
    }
    /* nnz 不落盘，加载后重算（扫一遍即可，成本极低） */
    for (int r = 0; r < g_nwords; r++) {
        const float* v = g_vec + (size_t)r * WEMB_DIM;
        int nnz = 0;
        for (int k = 0; k < WEMB_DIM; k++) if (v[k] != 0.0f) nnz++;
        g_nnz[r] = (uint16_t)nnz;
    }
    /* 频次表不落盘：PPMI 已在存盘前算完，归一化向量不再需要频次。
     * 但 wemb_similar 只用向量点积，所以够用。 */
    g_ready = 1;
    int n = g_nwords;
    pthread_mutex_unlock(&g_lock);
    fclose(fp);
    LOG_INFO("[指纹] 已加载 %s（%d 词）", path, n);
    return n;
}

/* ---------------- 查询 ---------------- */

int wemb_ensure(void) {
    if (g_ready) return 1;
    /* 懒加载必须串行：learn worker 是多线程，无锁会让多个线程同时 build
     * （1.7 秒 × N，且互相 _reset 掉对方的表）。init 锁只护「首次构建」，
     * 内部 load/build 用的仍是 g_lock，两把锁不会嵌套成环。 */
    pthread_mutex_lock(&g_init_lock);
    int ok = g_ready;
    if (!ok) {
        char buf[1024];
        _default_path(buf, sizeof(buf));
        if (wemb_load(buf) > 0) {
            ok = 1;
        } else if (wemb_build() > 0) {
            wemb_save(buf);      /* 建完就落盘，下次直接读 */
            ok = 1;
        }
    }
    pthread_mutex_unlock(&g_init_lock);
    return ok;
}

const float* wemb_vector(const char* word) {
    if (!word || !word[0]) return NULL;
    if (!wemb_ensure()) return NULL;
    int r = _find_row(word);
    if (r < 0) return NULL;
    return g_vec + (size_t)r * WEMB_DIM;
}

float wemb_cosine(const float* a, const float* b) {
    if (!a || !b) return 0.0f;
    double s = 0.0;
    for (int k = 0; k < WEMB_DIM; k++) s += (double)a[k] * (double)b[k];
    return (float)s;    /* 两个向量都已 L2 归一化 ⇒ 点积即余弦 */
}

float wemb_similar(const char* a, const char* b) {
    const float* va = wemb_vector(a);
    const float* vb = wemb_vector(b);
    if (!va || !vb) return -1.0f;   /* 无指纹：调用方回退，别当「完全不相似」 */
    /* 稀疏度闸门：只剩 1–2 个非零维时余弦会饱和到 1.00（实测「树/河/车」全 1.00），
     * 那是假置信不是真相似 ⇒ 视为不可用，让调用方回退。 */
    if (wemb_nnz(a) < WEMB_MIN_NNZ || wemb_nnz(b) < WEMB_MIN_NNZ) return -1.0f;
    return wemb_cosine(va, vb);
}

int wemb_dim(void) { return WEMB_DIM; }

int wemb_nnz(const char* word) {
    if (!word || !word[0] || !wemb_ensure()) return 0;
    int r = _find_row(word);
    return (r >= 0) ? (int)g_nnz[r] : 0;
}

int wemb_centroid(const char** words, int n, float* out) {
    if (!words || n <= 0 || !out) return -1;
    memset(out, 0, sizeof(float) * WEMB_DIM);
    int used = 0;
    for (int i = 0; i < n; i++) {
        const float* v = wemb_vector(words[i]);
        if (!v || wemb_nnz(words[i]) < WEMB_MIN_NNZ) continue;
        for (int k = 0; k < WEMB_DIM; k++) out[k] += v[k];
        used++;
    }
    if (used == 0) return -1;
    double nrm = 0.0;
    for (int k = 0; k < WEMB_DIM; k++) nrm += (double)out[k] * (double)out[k];
    if (nrm < 1e-12) return -1;
    float inv = (float)(1.0 / sqrt(nrm));
    for (int k = 0; k < WEMB_DIM; k++) out[k] *= inv;
    return used;
}

void wemb_stats(int* nwords, int* ndim, int* ready) {
    if (nwords) *nwords = g_nwords;
    if (ndim)   *ndim   = WEMB_DIM;
    if (ready)  *ready  = g_ready;
}

int wemb_feature_words(const char** out, int max) {
    if (!out || max <= 0 || !g_ready) return 0;
    int n = (g_nfeat < max) ? g_nfeat : max;
    for (int i = 0; i < n; i++) out[i] = g_feat[i];
    return n;
}
