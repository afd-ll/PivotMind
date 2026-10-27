/**
 * @file intent_router.c
 * @brief 意图概率路由 —— 见 intent_router.h 的设计说明
 *
 * 结构照 DeepSeekMoE 的路由，但 affinity 用朴素贝叶斯（解析解，无梯度）：
 *   score[i] = log P(intent_i) + mean_j log P(字_j | intent_i) + bias_i
 * 用**平均**对数似然而不是求和 —— 朴素贝叶斯的独立性假设在字级别会过度自信，
 * 求和会让长句的概率接近 0/1，平均才对长度鲁棒。
 */
#include "intent_router.h"
#include "pivotmind_paths.h"
#include "dialog_system.h"          /* recognize_intent / DialogIntent（自举打标） */
#include "cognitive_controller.h"   /* get_intent_base_weights */
#include "error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define IR_MAX_VOCAB 6000
#define IR_MAGIC     0x52495250u   /* "PRIR" */
#define IR_VER       1u
#define IR_GAMMA     0.001f        /* 负载均衡步长（照 MoE 的 auxiliary-loss-free） */
#define IR_KEYWORD_BOOST 2.0f     /* 关键词强命中时的 log 偏置 */
#define IR_SHARED    0.40f         /* shared expert 占比：default_base 保留 40% */
#define IR_HASH      16384

static char  (*g_words)[4] = NULL;
static int     g_nvocab = 0;
static float*  g_logp = NULL;                       /* IR_INTENT_MAX × g_nvocab */
static float   g_logprior[IR_INTENT_MAX];
static float   g_bias[IR_INTENT_MAX];
static int     g_ready = 0;
static float   g_last_entropy = 0.0f;
static uint16_t g_hidx[IR_HASH];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_init = PTHREAD_MUTEX_INITIALIZER;

/* ---------------- 基础工具 ---------------- */

static uint32_t _hash(const char* s) {
    uint32_t h = 2166136261u;
    while (s && *s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

static int _is_cjk1(const char* s) {
    if (!s || strlen(s) != 3) return 0;
    unsigned int cp = ((unsigned int)((unsigned char)s[0] & 0x0F) << 12)
                    | ((unsigned int)((unsigned char)s[1] & 0x3F) << 6)
                    |  (unsigned int)((unsigned char)s[2] & 0x3F);
    return cp >= 0x4E00 && cp <= 0x9FFF;
}

static int _find_row(const char* w) {
    if (!w || !w[0] || !g_nvocab) return -1;
    uint32_t h = _hash(w) & (IR_HASH - 1), start = h;
    while (g_hidx[h]) {
        int r = (int)g_hidx[h] - 1;
        if (r < g_nvocab && strcmp(g_words[r], w) == 0) return r;
        h = (h + 1) & (IR_HASH - 1);
        if (h == start) break;
    }
    return -1;
}

static int _add_row(const char* w) {
    int ex = _find_row(w);
    if (ex >= 0) return ex;
    if (g_nvocab >= IR_MAX_VOCAB || !g_words) return -1;
    uint32_t h = _hash(w) & (IR_HASH - 1), start = h;
    while (g_hidx[h]) {
        h = (h + 1) & (IR_HASH - 1);
        if (h == start) return -1;
    }
    int r = g_nvocab++;
    snprintf(g_words[r], 4, "%s", w);
    g_hidx[h] = (uint16_t)(r + 1);
    return r;
}

static void _default_path(char* out, size_t n) {
    snprintf(out, n, "%s/data/intent_model.bin", pm_home());
}

static void _reset(void) {
    free(g_words); g_words = NULL;
    free(g_logp);  g_logp = NULL;
    g_nvocab = 0; g_ready = 0; g_last_entropy = 0.0f;
    memset(g_hidx, 0, sizeof(g_hidx));
    for (int i = 0; i < IR_INTENT_MAX; i++) { g_logprior[i] = 0.0f; g_bias[i] = 0.0f; }
}

/* ---------------- 训练（自举打标） ---------------- */

int ir_train(const char* pipe_path, int max_lines) {
    if (!pipe_path) return -1;
    FILE* fp = fopen(pipe_path, "rb");
    if (!fp) { LOG_ERROR("[意图] 打不开语料 %s", pipe_path); return -1; }

    pthread_mutex_lock(&g_lock);
    _reset();
    g_words = (char (*)[4])calloc(IR_MAX_VOCAB, 4);
    /* uint32 计数矩阵：10 × 6000 = 240 KB */
    uint32_t* cnt = (uint32_t*)calloc((size_t)IR_INTENT_MAX * IR_MAX_VOCAB, sizeof(uint32_t));
    uint32_t  prior[IR_INTENT_MAX] = {0};
    uint32_t  total[IR_INTENT_MAX] = {0};
    if (!g_words || !cnt) { free(cnt); _reset(); pthread_mutex_unlock(&g_lock); return -1; }

    static char line[4096];
    int used = 0, labelled = 0;
    while (fgets(line, sizeof(line), fp) && (max_lines <= 0 || used < max_lines)) {
        used++;
        char* bar = strchr(line, '|');
        if (!bar) continue;
        *bar = 0;                                  /* 只取问句 */
        IntentResult ir = recognize_intent(line);
        if (ir.intent <= INTENT_UNKNOWN || ir.intent >= IR_INTENT_MAX) continue;
        /* 自举打标的质量闸门：
         * recognize_intent 的 else 分支把所有未命中都归 QUERY ⇒ 实测 QUERY 占 90.8%，
         * 先验压倒性、概率被压平（top2 只差 0.2）。这里剔除「判为 QUERY 但没有任何
         * 疑问词」的样本 —— 那些多半是闲聊被兜底成了查询。 */
        if (ir.intent == INTENT_QUERY) {
            static const char* QW[] = {"什么", "哪个", "多少", "谁", "吗", "？", "?", "如何", "怎么"};
            int has = 0;
            for (int qi = 0; QW[qi]; qi++) if (strstr(line, QW[qi])) { has = 1; break; }
            if (!has) continue;
        }
        int it = (int)ir.intent;
        labelled++;
        prior[it]++;
        /* 逐字统计（只 CJK 单字，与 charpairs / 指纹同粒度） */
        for (const char* p = line; *p; ) {
            unsigned char c = (unsigned char)*p;
            int len = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
            if (len == 3) {
                char one[4] = {0};
                memcpy(one, p, 3);
                if (_is_cjk1(one)) {
                    int r = _add_row(one);
                    if (r >= 0) { cnt[(size_t)it * IR_MAX_VOCAB + r]++; total[it]++; }
                }
            }
            p += len;
        }
    }
    fclose(fp);

    if (labelled <= 0 || g_nvocab <= 0) {
        free(cnt); _reset(); pthread_mutex_unlock(&g_lock);
        LOG_ERROR("[意图] 训练失败：无有效样本或词表为空");
        return -1;
    }

    /* 转成 log 概率（加一平滑） */
    g_logp = (float*)calloc((size_t)IR_INTENT_MAX * g_nvocab, sizeof(float));
    if (!g_logp) { free(cnt); _reset(); pthread_mutex_unlock(&g_lock); return -1; }
    for (int it = 0; it < IR_INTENT_MAX; it++) {
        double denom = (double)total[it] + (double)g_nvocab;   /* 加一平滑的分母 */
        if (denom < 1.0) denom = 1.0;
        for (int v = 0; v < g_nvocab; v++) {
            double p = ((double)cnt[(size_t)it * IR_MAX_VOCAB + v] + 1.0) / denom;
            g_logp[(size_t)it * g_nvocab + v] = (float)log(p);
        }
    }
    double psum = 0.0;
    for (int it = 0; it < IR_INTENT_MAX; it++) psum += (double)prior[it] + 1.0;
    /* 先验压到 0.15：实测满额先验会让分布被 90% 的 QUERY 压平（top2 差 0.2），
     * 而字级似然的量级不足以扭转。弱化后让似然主导，先验只做轻微偏置。 */
    for (int it = 0; it < IR_INTENT_MAX; it++)
        g_logprior[it] = (float)(0.15 * log(((double)prior[it] + 1.0) / psum));

    g_ready = 1;
    int nv = g_nvocab;
    pthread_mutex_unlock(&g_lock);
    free(cnt);
    LOG_INFO("[意图] 训练完成：%d 行（自举打标 %d）· 词表 %d · 每意图样本 "
             "Q=%u E=%u C=%u D=%u H=%u CHAT=%u L=%u",
             used, labelled, nv,
             prior[1], prior[2], prior[3], prior[4], prior[5], prior[6], prior[7]);
    return used;
}

/* ---------------- 存 / 读 ---------------- */

int ir_save(const char* path) {
    char buf[1024];
    if (!path) { _default_path(buf, sizeof(buf)); path = buf; }
    pthread_mutex_lock(&g_lock);
    if (!g_ready || !g_words || !g_logp) { pthread_mutex_unlock(&g_lock); return -1; }
    FILE* fp = fopen(path, "wb");
    if (!fp) { pthread_mutex_unlock(&g_lock); return -1; }
    uint32_t hdr[4] = { IR_MAGIC, IR_VER, (uint32_t)IR_INTENT_MAX, (uint32_t)g_nvocab };
    int ok = (fwrite(hdr, sizeof(uint32_t), 4, fp) == 4);
    if (ok) ok = (fwrite(g_words, 1, (size_t)g_nvocab * 4, fp) == (size_t)g_nvocab * 4);
    if (ok) ok = (fwrite(g_logprior, sizeof(float), IR_INTENT_MAX, fp) == IR_INTENT_MAX);
    if (ok) ok = (fwrite(g_logp, sizeof(float), (size_t)IR_INTENT_MAX * g_nvocab, fp)
                  == (size_t)IR_INTENT_MAX * g_nvocab);
    fclose(fp);
    pthread_mutex_unlock(&g_lock);
    if (!ok) { LOG_ERROR("[意图] 存盘失败 %s", path); return -1; }
    LOG_INFO("[意图] 已存盘 %s（%d 词 × %d 意图）", path, g_nvocab, IR_INTENT_MAX);
    return 0;
}

int ir_load(const char* path) {
    char buf[1024];
    if (!path) { _default_path(buf, sizeof(buf)); path = buf; }
    FILE* fp = fopen(path, "rb");
    if (!fp) return -1;
    uint32_t hdr[4] = {0};
    if (fread(hdr, sizeof(uint32_t), 4, fp) != 4 || hdr[0] != IR_MAGIC ||
        hdr[1] != IR_VER || hdr[2] != IR_INTENT_MAX ||
        hdr[3] == 0 || hdr[3] > IR_MAX_VOCAB) {
        fclose(fp);
        LOG_WARNING("[意图] %s 头无效，忽略", path);
        return -1;
    }
    pthread_mutex_lock(&g_lock);
    _reset();
    g_words = (char (*)[4])calloc(IR_MAX_VOCAB, 4);
    g_nvocab = (int)hdr[3];
    g_logp = (float*)calloc((size_t)IR_INTENT_MAX * g_nvocab, sizeof(float));
    if (!g_words || !g_logp) { _reset(); pthread_mutex_unlock(&g_lock); fclose(fp); return -1; }
    int ok = (fread(g_words, 1, (size_t)g_nvocab * 4, fp) == (size_t)g_nvocab * 4);
    if (ok) ok = (fread(g_logprior, sizeof(float), IR_INTENT_MAX, fp) == IR_INTENT_MAX);
    if (ok) ok = (fread(g_logp, sizeof(float), (size_t)IR_INTENT_MAX * g_nvocab, fp)
                  == (size_t)IR_INTENT_MAX * g_nvocab);
    fclose(fp);
    if (!ok) { _reset(); pthread_mutex_unlock(&g_lock); return -1; }
    for (int r = 0; r < g_nvocab; r++) {
        uint32_t h = _hash(g_words[r]) & (IR_HASH - 1), start = h;
        while (g_hidx[h]) {
            h = (h + 1) & (IR_HASH - 1);
            if (h == start) break;
        }
        if (!g_hidx[h]) g_hidx[h] = (uint16_t)(r + 1);
    }
    g_ready = 1;
    int nv = g_nvocab;
    pthread_mutex_unlock(&g_lock);
    LOG_INFO("[意图] 已加载 %s（%d 词 × %d 意图）", path, nv, IR_INTENT_MAX);
    return nv;
}

int ir_ensure(void) {
    if (g_ready) return 1;
    pthread_mutex_lock(&g_init);
    int ok = g_ready;
    if (!ok) {
        char buf[1024];
        _default_path(buf, sizeof(buf));
        ok = (ir_load(buf) > 0) ? 1 : 0;
    }
    pthread_mutex_unlock(&g_init);
    return ok;
}

/* ---------------- 推理 ---------------- */

int ir_classify(const char* text, IntentDist* out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!text || !text[0] || !ir_ensure()) return 0;

    float score[IR_INTENT_MAX];
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < IR_INTENT_MAX; i++) score[i] = g_logprior[i] + g_bias[i];

    int nch = 0;
    for (const char* p = text; *p; ) {
        unsigned char c = (unsigned char)*p;
        int len = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
        if (len == 3) {
            char one[4] = {0};
            memcpy(one, p, 3);
            if (_is_cjk1(one)) {
                int r = _find_row(one);
                if (r >= 0) {
                    for (int i = 0; i < IR_INTENT_MAX; i++)
                        score[i] += g_logp[(size_t)i * g_nvocab + r];
                    nch++;
                }
            }
        }
        p += len;
    }
    if (nch == 0) { pthread_mutex_unlock(&g_lock); return 0; }
    /* 用平均对数似然而非求和：字级朴素贝叶斯求和会过度自信，长句概率贴 0/1 */
    for (int i = 0; i < IR_INTENT_MAX; i++) score[i] /= (float)nch;

    /* ── 关键词强提示（与概率互补，不是二选一）──
     * 实测：字级朴素贝叶斯按字独立累加，**表达不了「为什么」这种组合特征**
     * ⇒ 「为什么要睡觉」被「查询」抢走（0.512 vs 0.488）。
     * 而 recognize_intent 的关键词匹配恰好擅长明确疑问结构。
     * 置信 ≥ 0.8 时给该意图一个 log 偏置，让规则点火、概率细化。 */
    {
        IntentResult kr = recognize_intent(text);
        if (kr.intent > INTENT_UNKNOWN && kr.intent < IR_INTENT_MAX && kr.confidence >= 0.8f)
            score[(int)kr.intent] += IR_KEYWORD_BOOST;
    }

    /* softmax */
    float mx = score[0];
    for (int i = 1; i < IR_INTENT_MAX; i++) if (score[i] > mx) mx = score[i];
    float sum = 0.0f;
    for (int i = 0; i < IR_INTENT_MAX; i++) { out->prob[i] = expf(score[i] - mx); sum += out->prob[i]; }
    if (sum <= 0.0f) { pthread_mutex_unlock(&g_lock); return 0; }
    for (int i = 0; i < IR_INTENT_MAX; i++) out->prob[i] /= sum;

    /* 熵（bit）→ 温度调度的依据 */
    float H = 0.0f;
    for (int i = 0; i < IR_INTENT_MAX; i++)
        if (out->prob[i] > 1e-9f) H -= out->prob[i] * log2f(out->prob[i]);
    out->entropy = H;
    g_last_entropy = H;

    /* top-K */
    int K = IR_TOP_K;
    for (int k = 0; k < K; k++) {
        int bi = -1;
        float bv = -1.0f;
        for (int i = 0; i < IR_INTENT_MAX; i++) {
            int used = 0;
            for (int j = 0; j < k; j++) if (out->top[j] == i) used = 1;
            if (used) continue;
            if (out->prob[i] > bv) { bv = out->prob[i]; bi = i; }
        }
        out->top[k] = bi;
        out->g[k] = bv;              /* 先放原概率，下面在 top-K 内归一化 */
    }
    float gsum = 0.0f;
    for (int k = 0; k < K; k++) gsum += out->g[k];
    for (int k = 0; k < K; k++) out->g[k] = (gsum > 0.0f) ? out->g[k] / gsum : 0.0f;
    out->n = K;

    /* 负载均衡 bias（照 MoE 的 auxiliary-loss-free：过载减、欠载加，不进梯度） */
    int top1 = out->top[0];
    if (top1 >= 0) {
        g_bias[top1] -= IR_GAMMA;
        float add = IR_GAMMA / (float)(IR_INTENT_MAX - 1);
        for (int i = 0; i < IR_INTENT_MAX; i++) if (i != top1) g_bias[i] += add;
    }
    pthread_mutex_unlock(&g_lock);
    return out->n;
}

int ir_mixed_topology_weights(const IntentDist* d, float* out, int n_topo) {
    if (!d || d->n <= 0 || !out || n_topo <= 0) return 0;
    int n = (n_topo < MAX_SUBTOPOS) ? n_topo : MAX_SUBTOPOS;
    float shared[MAX_SUBTOPOS], tmp[MAX_SUBTOPOS];
    get_intent_base_weights(INTENT_UNKNOWN, shared);   /* default_base = shared expert */
    float routed[MAX_SUBTOPOS];
    for (int t = 0; t < n; t++) routed[t] = 0.0f;
    for (int k = 0; k < d->n; k++) {
        if (d->top[k] < 0) continue;
        get_intent_base_weights(d->top[k], tmp);
        for (int t = 0; t < n; t++) routed[t] += d->g[k] * tmp[t];
    }
    for (int t = 0; t < n; t++)
        out[t] = IR_SHARED * shared[t] + (1.0f - IR_SHARED) * routed[t];
    return 1;
}

void ir_stats(int* n_intent, int* n_vocab, float* last_entropy, int* ready) {
    if (n_intent) *n_intent = IR_INTENT_MAX;
    if (n_vocab)  *n_vocab = g_nvocab;
    if (last_entropy) *last_entropy = g_last_entropy;
    if (ready) *ready = g_ready;
}

void ir_biases(float* out, int max) {
    if (!out || max <= 0) return;
    int n = (max < IR_INTENT_MAX) ? max : IR_INTENT_MAX;
    for (int i = 0; i < n; i++) out[i] = g_bias[i];
}
