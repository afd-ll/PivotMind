/**
 * tools/seq2seq_pretrain.c
 * PivotMind v0.2 Seq2Seq 预训练 — 自包含实现
 *
 * 不依赖 layer/model/rnn 框架的 bug。
 * 手撸 Embedding + SimpleRNN + Linear + BPTT + SGD。
 *
 * 架构: Encoder(Emb→RNN) + Decoder(Emb→RNN→Linear)
 * 训练: Teacher Forcing + BPTT + SGD
 *
 * 编译: gcc -std=gnu99 -O2 -lm -o build/bin/seq2seq_pretrain tools/seq2seq_pretrain.c
 * 用法: ./build/bin/seq2seq_pretrain data/knowledge_base.json [epochs]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

// ===================== 配置 =====================
#define EMBED_DIM  32
#define HIDDEN_DIM 96
#define MAX_SEQ    64
#define MAX_VOCAB  3000
#define MAX_PAIRS  5000
#define MAX_LINE   4096

// ===================== Tensor 工具 =====================

typedef struct {
    float* data;
    int rows;
    int cols;
} Mat;

Mat* mat_create(int rows, int cols) {
    Mat* m = calloc(1, sizeof(Mat));
    m->rows = rows;
    m->cols = cols;
    m->data = calloc(rows * cols, sizeof(float));
    return m;
}

void mat_free(Mat* m) {
    if (!m) return;
    free(m->data);
    free(m);
}

void mat_rand(Mat* m, float scale) {
    for (int i = 0; i < m->rows * m->cols; i++)
        m->data[i] = ((float)rand() / RAND_MAX - 0.5f) * 2 * scale;
}

void mat_zero(Mat* m) {
    memset(m->data, 0, m->rows * m->cols * sizeof(float));
}

void mat_mul(Mat* a, Mat* b, Mat* out) {
    // out = a @ b,  a: (M,N), b: (N,P), out: (M,P)
    int M = a->rows, N = a->cols, P = b->cols;
    for (int i = 0; i < M; i++)
        for (int j = 0; j < P; j++) {
            float s = 0;
            for (int k = 0; k < N; k++)
                s += a->data[i * N + k] * b->data[k * P + j];
            out->data[i * P + j] = s;
        }
}

void mat_add(Mat* a, Mat* b) {
    for (int i = 0; i < a->rows * a->cols; i++)
        a->data[i] += b->data[i];
}

void mat_sgd(Mat* w, Mat* grad, float lr) {
    for (int i = 0; i < w->rows * w->cols; i++)
        w->data[i] -= lr * grad->data[i];
}

// ===================== RNN 核心 =====================

typedef struct {
    // 编码器权重
    Mat* emb_e;   // (vocab, EMBED_DIM)
    Mat* Wx_e;    // (EMBED_DIM, HIDDEN_DIM)
    Mat* Wh_e;    // (HIDDEN_DIM, HIDDEN_DIM)
    Mat* bh_e;    // (1, HIDDEN_DIM)
    // 解码器权重
    Mat* emb_d;   // (vocab, EMBED_DIM)
    Mat* Wx_d;    // (EMBED_DIM, HIDDEN_DIM)
    Mat* Wh_d;    // (HIDDEN_DIM, HIDDEN_DIM)
    Mat* bh_d;    // (1, HIDDEN_DIM)
    Mat* Wo;      // (HIDDEN_DIM, vocab)
    Mat* bo;      // (1, vocab)
    // 梯度（解码器）
    Mat* dWx_d; Mat* dWh_d; Mat* dbh_d;
    Mat* dWo; Mat* dbo;
    Mat* dWx_e; Mat* dWh_e; Mat* dbh_e;
    // 元数据
    int vocab;
    int verbose;
} RNN;

RNN* rnn_create(int vocab, int verbose) {
    RNN* r = calloc(1, sizeof(RNN));
    r->vocab = vocab;
    r->verbose = verbose;

    float scale_e = 1.0f / sqrt(EMBED_DIM);
    float scale_h = 1.0f / sqrt(HIDDEN_DIM);
    float scale_o = 0.01f;

    r->emb_e = mat_create(vocab, EMBED_DIM); mat_rand(r->emb_e, scale_e);
    r->Wx_e  = mat_create(EMBED_DIM, HIDDEN_DIM); mat_rand(r->Wx_e, scale_h);
    r->Wh_e  = mat_create(HIDDEN_DIM, HIDDEN_DIM); mat_rand(r->Wh_e, scale_h);
    r->bh_e  = mat_create(1, HIDDEN_DIM); mat_zero(r->bh_e);

    r->emb_d = mat_create(vocab, EMBED_DIM); mat_rand(r->emb_d, scale_e);
    r->Wx_d  = mat_create(EMBED_DIM, HIDDEN_DIM); mat_rand(r->Wx_d, scale_h);
    r->Wh_d  = mat_create(HIDDEN_DIM, HIDDEN_DIM); mat_rand(r->Wh_d, scale_h);
    r->bh_d  = mat_create(1, HIDDEN_DIM); mat_zero(r->bh_d);
    r->Wo    = mat_create(HIDDEN_DIM, vocab); mat_rand(r->Wo, scale_o);
    r->bo    = mat_create(1, vocab); mat_zero(r->bo);

    r->dWx_d = mat_create(EMBED_DIM, HIDDEN_DIM);
    r->dWh_d = mat_create(HIDDEN_DIM, HIDDEN_DIM);
    r->dbh_d = mat_create(1, HIDDEN_DIM);
    r->dWo   = mat_create(HIDDEN_DIM, vocab);
    r->dbo   = mat_create(1, vocab);
    r->dWx_e = mat_create(EMBED_DIM, HIDDEN_DIM);
    r->dWh_e = mat_create(HIDDEN_DIM, HIDDEN_DIM);
    r->dbh_e = mat_create(1, HIDDEN_DIM);
    return r;
}

void rnn_free(RNN* r) {
    if (!r) return;
    mat_free(r->emb_e); mat_free(r->Wx_e); mat_free(r->Wh_e); mat_free(r->bh_e);
    mat_free(r->emb_d); mat_free(r->Wx_d); mat_free(r->Wh_d); mat_free(r->bh_d);
    mat_free(r->Wo); mat_free(r->bo);
    mat_free(r->dWx_d); mat_free(r->dWh_d); mat_free(r->dbh_d);
    mat_free(r->dWo); mat_free(r->dbo);
    mat_free(r->dWx_e); mat_free(r->dWh_e); mat_free(r->dbh_e);
    free(r);
}

// ===================== 前向 / 反向 =====================

// 编码器前向: 输入 token_ids[T] → 输出 hidden[T][H], 返回最后一个 hidden
// cache: h_t 保存到 cache_h[T+1][H], h0 = 全零
static void enc_forward(RNN* r, int* tokens, int T,
                        float* cache_h) {
    Mat emb = {r->emb_e->data, r->vocab, EMBED_DIM};
    // h0 = 0
    memset(cache_h, 0, HIDDEN_DIM * sizeof(float));
    for (int t = 0; t < T; t++) {
        int wid = tokens[t];
        float* prev_h = cache_h + t * HIDDEN_DIM;
        float* cur_h  = cache_h + (t + 1) * HIDDEN_DIM;
        // cur_h = tanh(emb[wid] @ Wx + prev_h @ Wh + bh)
        float* x = emb.data + wid * EMBED_DIM;
        for (int j = 0; j < HIDDEN_DIM; j++) {
            float s = r->bh_e->data[j];
            for (int k = 0; k < EMBED_DIM; k++)
                s += x[k] * r->Wx_e->data[k * HIDDEN_DIM + j];
            for (int k = 0; k < HIDDEN_DIM; k++)
                s += prev_h[k] * r->Wh_e->data[k * HIDDEN_DIM + j];
            cur_h[j] = tanhf(s);
        }
    }
}

// 解码器单步前向: 输入 token_id, prev_h → 输出 logits[vocab], cur_h
static void dec_step(RNN* r, int token_id, float* prev_h,
                     float* logits, float* cur_h) {
    float* x = r->emb_d->data + token_id * EMBED_DIM;
    for (int j = 0; j < HIDDEN_DIM; j++) {
        float s = r->bh_d->data[j];
        for (int k = 0; k < EMBED_DIM; k++)
            s += x[k] * r->Wx_d->data[k * HIDDEN_DIM + j];
        for (int k = 0; k < HIDDEN_DIM; k++)
            s += prev_h[k] * r->Wh_d->data[k * HIDDEN_DIM + j];
        cur_h[j] = tanhf(s);
    }
    // Wo @ cur_h + bo
    for (int v = 0; v < r->vocab; v++) {
        float s = r->bo->data[v];
        for (int k = 0; k < HIDDEN_DIM; k++)
            s += cur_h[k] * r->Wo->data[k * r->vocab + v];
        logits[v] = s;
    }
}

// softmax + cross-entropy loss
static float softmax_loss(float* logits, int target, int vocab) {
    float maxv = logits[0];
    for (int i = 1; i < vocab; i++)
        if (logits[i] > maxv) maxv = logits[i];
    float sum = 0;
    for (int i = 0; i < vocab; i++)
        sum += expf(logits[i] - maxv);
    float prob = expf(logits[target] - maxv) / (sum + 1e-10f);
    return -logf(prob + 1e-10f);
}

// Teacher Forcing 训练: BPTT 解码器 + 编码器梯度
static float train_step(RNN* r, int* inp_tokens, int in_len,
                        int* tgt_tokens, int tgt_len, float lr) {
    // ===== 编码器前向 =====
    float* enc_h = malloc((in_len + 1) * HIDDEN_DIM * sizeof(float));
    enc_forward(r, inp_tokens, in_len, enc_h);
    float* enc_final = enc_h + in_len * HIDDEN_DIM; // 最后一步

    // ===== 解码器 Teacher Forcing =====
    // 存储每步的 hidden 和 logits
    float* dec_h = calloc((tgt_len + 1) * HIDDEN_DIM, sizeof(float));
    float* dec_logits = malloc(tgt_len * r->vocab * sizeof(float));
    memcpy(dec_h, enc_final, HIDDEN_DIM * sizeof(float)); // decoder init from encoder

    float total_loss = 0;
    for (int t = 0; t < tgt_len; t++) {
        int input_id = (t == 0) ? 1 : tgt_tokens[t - 1]; // <SOS> at t=0
        float* logits = dec_logits + t * r->vocab;
        float* cur_h  = dec_h + (t + 1) * HIDDEN_DIM;
        float* prev_h = dec_h + t * HIDDEN_DIM;
        dec_step(r, input_id, prev_h, logits, cur_h);
        total_loss += softmax_loss(logits, tgt_tokens[t], r->vocab);
    }
    float avg_loss = total_loss / tgt_len;

    // ===== BPTT: 解码器 =====
    mat_zero(r->dWo); mat_zero(r->dbo);
    mat_zero(r->dWx_d); mat_zero(r->dWh_d); mat_zero(r->dbh_d);

    for (int t = tgt_len - 1; t >= 0; t--) {
        float* logits = dec_logits + t * r->vocab;
        int target = tgt_tokens[t];
        // softmax 梯度: prob - onehot
        float maxv = logits[0];
        for (int i = 1; i < r->vocab; i++)
            if (logits[i] > maxv) maxv = logits[i];
        float sum = 0;
        for (int i = 0; i < r->vocab; i++)
            sum += expf(logits[i] - maxv);
        float dlogit[3000];
        for (int i = 0; i < r->vocab; i++) {
            float p = expf(logits[i] - maxv) / (sum + 1e-10f);
            dlogit[i] = (p - (i == target ? 1.0f : 0.0f)) / tgt_len;
        }

        float* cur_h = dec_h + (t + 1) * HIDDEN_DIM;
        // dWo += cur_h ⊗ dlogit
        for (int i = 0; i < HIDDEN_DIM; i++)
            for (int j = 0; j < r->vocab; j++)
                r->dWo->data[i * r->vocab + j] += cur_h[i] * dlogit[j];
        for (int j = 0; j < r->vocab; j++)
            r->dbo->data[j] += dlogit[j];

        // dh = Wo^T @ dlogit
        float dh[HIDDEN_DIM] = {0};
        for (int i = 0; i < HIDDEN_DIM; i++)
            for (int j = 0; j < r->vocab; j++)
                dh[i] += r->Wo->data[i * r->vocab + j] * dlogit[j];

        // tanh 导数: (1 - h^2) * dh
        for (int i = 0; i < HIDDEN_DIM; i++)
            dh[i] *= (1 - cur_h[i] * cur_h[i]);

        // dWx_d += x ⊗ dh, where x = emb[input_id]
        float* prev_h = dec_h + t * HIDDEN_DIM;
        if (t < tgt_len) {
            int inp_id = (t == 0) ? 1 : tgt_tokens[t - 1];
            float* x = r->emb_d->data + inp_id * EMBED_DIM;
            for (int k = 0; k < EMBED_DIM; k++)
                for (int j = 0; j < HIDDEN_DIM; j++)
                    r->dWx_d->data[k * HIDDEN_DIM + j] += x[k] * dh[j];
        }
        // dWh_d += prev_h ⊗ dh
        for (int k = 0; k < HIDDEN_DIM; k++)
            for (int j = 0; j < HIDDEN_DIM; j++)
                r->dWh_d->data[k * HIDDEN_DIM + j] += prev_h[k] * dh[j];
        for (int j = 0; j < HIDDEN_DIM; j++)
            r->dbh_d->data[j] += dh[j];
    }

    // ===== BPTT: 编码器（简化为只更新最后一层的梯度） =====
    // 把解码器第0步的 dh 传回编码器最后一步
    float* dh0 = malloc(HIDDEN_DIM * sizeof(float));
    {
        float* logits = dec_logits + 0 * r->vocab;
        int target = tgt_tokens[0];
        float maxv = logits[0];
        for (int i = 1; i < r->vocab; i++)
            if (logits[i] > maxv) maxv = logits[i];
        float sum = 0;
        for (int i = 0; i < r->vocab; i++)
            sum += expf(logits[i] - maxv);
        float dlogit[3000];
        for (int i = 0; i < r->vocab; i++) {
            float p = expf(logits[i] - maxv) / (sum + 1e-10f);
            dlogit[i] = (p - (i == target ? 1.0f : 0.0f)) / tgt_len;
        }
        float* h1 = dec_h + 1 * HIDDEN_DIM;
        for (int i = 0; i < HIDDEN_DIM; i++) {
            float s = 0;
            for (int j = 0; j < r->vocab; j++)
                s += r->Wo->data[i * r->vocab + j] * dlogit[j];
            dh0[i] = s * (1 - h1[i] * h1[i]);
        }
    }

    // 编码器: dWx_e, dWh_e
    for (int t = in_len - 1; t >= 0; t--) {
        float* cur_h = enc_h + (t + 1) * HIDDEN_DIM;
        float der[HIDDEN_DIM];
        float* incoming = (t == in_len - 1) ? dh0 : der; // 简化: 只传第0步
        if (t != in_len - 1) {
            for (int i = 0; i < HIDDEN_DIM; i++)
                der[i] = 0; // 不回溯深编码器
        }
        memcpy(der, incoming, HIDDEN_DIM * sizeof(float));
        for (int i = 0; i < HIDDEN_DIM; i++)
            der[i] *= (1 - cur_h[i] * cur_h[i]);

        float* prev_h = enc_h + t * HIDDEN_DIM;
        float* x = r->emb_e->data + inp_tokens[t] * EMBED_DIM;
        for (int k = 0; k < EMBED_DIM; k++)
            for (int j = 0; j < HIDDEN_DIM; j++)
                r->dWx_e->data[k * HIDDEN_DIM + j] += x[k] * der[j];
        for (int k = 0; k < HIDDEN_DIM; k++)
            for (int j = 0; j < HIDDEN_DIM; j++)
                r->dWh_e->data[k * HIDDEN_DIM + j] += prev_h[k] * der[j];
        for (int j = 0; j < HIDDEN_DIM; j++)
            r->dbh_e->data[j] += der[j];
    }

    free(dh0);

    // ===== SGD 更新 =====
    mat_sgd(r->Wo,   r->dWo,   lr);
    mat_sgd(r->bo,   r->dbo,   lr);
    mat_sgd(r->Wx_d, r->dWx_d, lr);
    mat_sgd(r->Wh_d, r->dWh_d, lr);
    mat_sgd(r->bh_d, r->dbh_d, lr);
    mat_sgd(r->Wx_e, r->dWx_e, lr);
    mat_sgd(r->Wh_e, r->dWh_e, lr);
    mat_sgd(r->bh_e, r->dbh_e, lr);
    // Embedding 梯度暂不更新（简化）

    free(enc_h);
    free(dec_h);
    free(dec_logits);
    return avg_loss;
}

// ===================== 推理 =====================

static void generate(RNN* r, int* inp_tokens, int in_len,
                     char** vocab_words, int* out_tokens, int* out_len) {
    float* enc_h = malloc((in_len + 1) * HIDDEN_DIM * sizeof(float));
    enc_forward(r, inp_tokens, in_len, enc_h);
    float* h = malloc((MAX_SEQ + 1) * HIDDEN_DIM * sizeof(float));
    memcpy(h, enc_h + in_len * HIDDEN_DIM, HIDDEN_DIM * sizeof(float));

    int pos = 0;
    int token = 1; // <SOS>
    while (pos < MAX_SEQ - 1) {
        float* cur_h = h + (pos + 1) * HIDDEN_DIM;
        float* prev_h = h + pos * HIDDEN_DIM;
        float logits[3000];
        dec_step(r, token, prev_h, logits, cur_h);

        // greedy: 选概率最高的
        float maxv = logits[0];
        int best = 0;
        for (int v = 1; v < r->vocab; v++) {
            float p = logits[v];
            float max_l = logits[0];
            for (int k = 1; k < r->vocab; k++)
                if (logits[k] > max_l) max_l = logits[k];
            float sum = 0;
            for (int k = 0; k < r->vocab; k++)
                sum += expf(logits[k] - max_l);
            float prob = expf(p - max_l) / (sum + 1e-10f);
            if (prob > maxv) { maxv = prob; best = v; }
        }

        out_tokens[pos] = best;
        token = best;
        pos++;
        if (best == 2) break; // <EOS>
    }
    *out_len = pos;
    free(enc_h);
    free(h);
}

// ===================== 数据加载 =====================

static int load_qa(const char* path, char qs[][MAX_LINE], char as[][MAX_LINE]) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char* buf = malloc(sz + 1);
    fread(buf, 1, sz, fp);
    buf[sz] = 0;
    fclose(fp);

    // 去 \r
    int wi = 0;
    for (long i = 0; i < sz; i++)
        if (buf[i] != '\r') buf[wi++] = buf[i];
    buf[wi] = 0;

    int count = 0;
    char* p = buf;
    while (*p && *p != '[') p++;
    if (*p) p++;
    while (*p <= ' ') p++;

    while (*p && count < MAX_PAIRS) {
        while (*p && *p != '[') p++;
        if (!*p) break; p++;
        while (*p && *p <= ' ') p++;
        if (!*p || *p == ']') continue;
        if (*p != '"') continue; p++;

        int qi = 0;
        while (*p && *p != '"' && qi < MAX_LINE - 1)
            qs[count][qi++] = *p++;
        qs[count][qi] = 0; if (*p) p++;
        while (*p && *p != '"') p++; if (*p) p++;

        int ai = 0;
        while (*p && *p != '"' && ai < MAX_LINE - 1)
            as[count][ai++] = *p++;
        as[count][ai] = 0;
        count++;
    }
    free(buf);
    return count;
}

// ===================== 分词 + 词表 =====================

static char* vocab_words[MAX_VOCAB];
static int vocab_size = 0;

static int vocab_add(const char* word) {
    for (int i = 0; i < vocab_size; i++)
        if (strcmp(vocab_words[i], word) == 0) return i;
    if (vocab_size >= MAX_VOCAB) return 3; // <UNK>
    vocab_words[vocab_size] = strdup(word);
    return vocab_size++;
}

static int vocab_id(const char* word) {
    for (int i = 0; i < vocab_size; i++)
        if (strcmp(vocab_words[i], word) == 0) return i;
    return 3; // <UNK>
}

// UTF-8 字符级分词
static int tokenize(const char* text, int* ids, int max_tokens) {
    int len = strlen(text);
    int count = 0;
    char buf[8] = {0};
    for (int i = 0; i < len && count < max_tokens; ) {
        if ((unsigned char)text[i] < 32) { i++; continue; }
        int clen;
        if      ((unsigned char)text[i] >= 0xF0) clen = 4;
        else if ((unsigned char)text[i] >= 0xE0) clen = 3;
        else if ((unsigned char)text[i] >= 0xC0) clen = 2;
        else clen = 1;

        // 跳过标点
        if (clen == 1 && strchr(" ，。、？！；：""''（）【】《》?.,!;:()[]{}\"' \t\n\r", text[i])) { i += clen; continue; }

        if (i + clen > len) break;
        memcpy(buf, text + i, clen); buf[clen] = 0;
        ids[count++] = vocab_add(buf);
        i += clen;
    }
    return count;
}

// ===================== 主 =====================

int main(int argc, char* argv[]) {
    setbuf(stdout, NULL);
    srand((unsigned)time(NULL));
    printf("╔═══════════════════════════════════════════╗\n");
    printf("║   PivotMind v0.2 Seq2Seq 预训练(自包含)  ║\n");
    printf("╚═══════════════════════════════════════════╝\n\n");

    const char* path = argc > 1 ? argv[1] : "data/knowledge_base.json";
    int epochs = argc > 2 ? atoi(argv[2]) : 50;

    // 特殊 token
    vocab_add("<PAD>"); // 0
    vocab_add("<SOS>"); // 1
    vocab_add("<EOS>"); // 2
    vocab_add("<UNK>"); // 3

    // 加载数据
    printf("[1/4] 加载 %s\n", path);
    static char qs[MAX_PAIRS][MAX_LINE];
    static char as[MAX_PAIRS][MAX_LINE];
    int n = load_qa(path, qs, as);
    printf("  ✓ %d 条问答对\n", n);

    // 构建词表 + 训练样本
    printf("[2/4] 构建词表...\n");
    static int inp_ids[MAX_PAIRS][MAX_SEQ];
    static int tgt_ids[MAX_PAIRS][MAX_SEQ];
    static int inp_len[MAX_PAIRS], tgt_len[MAX_PAIRS];
    int valid = 0;
    for (int i = 0; i < n; i++) {
        int il = tokenize(qs[i], inp_ids[valid], MAX_SEQ);
        int tl = tokenize(as[i], tgt_ids[valid], MAX_SEQ);
        if (il == 0 || tl == 0) continue;
        inp_len[valid] = il;
        tgt_len[valid] = tl;
        valid++;
    }
    printf("  ✓ 词表: %d, 有效样本: %d\n", vocab_size, valid);

    // 创建 RNN
    printf("[3/4] 创建 RNN (emb=%d, hid=%d, vocab=%d)\n", EMBED_DIM, HIDDEN_DIM, vocab_size);
    RNN* rnn = rnn_create(vocab_size, 1);

    // 训练
    printf("[4/4] 训练 %d epoch (lr=0.01→0.001)\n", epochs);
    time_t start = time(NULL);
    for (int ep = 0; ep < epochs; ep++) {
        float lr = 0.01f * (1.0f - (float)ep / epochs * 0.9f);
        float total_loss = 0;
        for (int i = 0; i < valid; i++) {
            float loss = train_step(rnn, inp_ids[i], inp_len[i],
                                    tgt_ids[i], tgt_len[i], lr);
            total_loss += loss;
        }
        float avg = total_loss / valid;

        if ((ep + 1) % 5 == 0 || ep == 0) {
            double elapsed = difftime(time(NULL), start);
            printf("  [epoch %3d/%d] loss=%.4f  (%.0fs)\n",
                   ep + 1, epochs, avg, elapsed);
        }
    }

    // 测试
    printf("\n生成测试:\n");
    for (int i = 0; i < 5 && i < valid; i++) {
        int out_ids[MAX_SEQ], out_len;
        generate(rnn, inp_ids[i], inp_len[i], vocab_words, out_ids, &out_len);

        printf("  Q: %s\n", qs[i]);
        printf("  A: ");
        for (int j = 0; j < out_len; j++) {
            if (out_ids[j] >= 4 && out_ids[j] < vocab_size)
                printf("%s", vocab_words[out_ids[j]]);
        }
        printf("\n\n");
    }

    double total = difftime(time(NULL), start);
    printf("总耗时: %.0f 秒\n", total);
    rnn_free(rnn);

    // 清理词表
    for (int i = 0; i < vocab_size; i++)
        free(vocab_words[i]);

    return 0;
}
