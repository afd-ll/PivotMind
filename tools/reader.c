/**
 * tools/reader.c
 * PivotMind v0.2 读书工具
 *
 * 读一本 .txt 书 → 训练字符 embedding → 导出种子文件
 *
 * 原理：滑动窗口预测下一个字，自监督学习。
 * 读完后每个字的向量反映它的语义上下文。
 *
 * 编译: gcc -std=gnu99 -O2 -Iinclude -I. -Ilibs -fopenmp
 *        -o build/bin/reader tools/reader.c
 *        src/tensor.c src/model.c src/model_io.c src/error.c
 *        src/layer.c src/string_pool.c src/huarong_topology.c
 *        src/node_hash.c src/cognitive_params.c src/multi_topology.c -lm
 * 用法: ./build/bin/reader 书.txt [epochs]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "common.h"

// ===================== 配置 =====================
#define MAX_VOCAB 5000   // 最大不同字符数
#define MAX_BIGRAM 2000  // 最大双字词数
#define MAX_CHARS 800000 // 最大文本字符
#define MAX_EPOCHS 20
#define WINDOW 4         // 滑动窗口大小（前后各看几个字）
#define LR 0.02f

// ===================== 词表 =====================
static char* vocab_words[MAX_VOCAB];
static float vocab_emb[MAX_VOCAB][NODE_FEATURE_DIM]; // 训练好的向量
static int vocab_size = 4; // 0=<PAD> 1=<SOS> 2=<EOS> 3=<UNK>

static int vocab_add(const char* word) {
    for (int i = 4; i < vocab_size; i++)
        if (strcmp(vocab_words[i], word) == 0) return i;
    if (vocab_size >= MAX_VOCAB) return 3;
    vocab_words[vocab_size] = strdup(word);
    // 随机初始化
    for (int d = 0; d < NODE_FEATURE_DIM; d++)
        vocab_emb[vocab_size][d] = ((float)rand()/RAND_MAX - 0.5f) * 0.1f;
    return vocab_size++;
}

// ===================== 读取文本 =====================

static char* read_file(const char* path, long* out_len) {
    FILE* fp = fopen(path, "rb");
    if (!fp) { printf("[错误] 打不开: %s\n", path); return NULL; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz > MAX_CHARS) sz = MAX_CHARS;
    fseek(fp, 0, SEEK_SET);
    char* buf = malloc(sz + 1);
    long got = fread(buf, 1, sz, fp);
    buf[got] = 0;
    fclose(fp);
    *out_len = got;
    return buf;
}

// ===================== 分出句子 =====================

// 标记是否为句子结束标点
static int is_sent_end(const char* p) {
    // 中文句号 ！？。！？…——\n
    unsigned char c = (unsigned char)*p;
    if (c == '\n' || c == '\r') return 1;
    if (c == 0xEF) { // 全角标点如 。！？
        if (p[1] == 0xBC && (p[2] == 0x81 || p[2] == 0x89)) return 1; // ！？
        if (p[1] == 0xBC && p[2] == 0x8E) return 1; // 。
    }
    return 0;
}

// UTF-8 字符长度
static int utf8_len(unsigned char c) {
    if (c >= 0xF0) return 4;
    if (c >= 0xE0) return 3;
    if (c >= 0xC0) return 2;
    return 1;
}

// 将文本转为 token ID 序列（只保留中文字符和字母数字，跳过标点/空白）
static int text_to_ids(const char* text, long len, int* ids, int max_ids) {
    int count = 0;
    for (long i = 0; i < len && count < max_ids; ) {
        int clen = utf8_len((unsigned char)text[i]);
        if (i + clen > len) break;

        // 跳过空白和标点
        if (clen == 1 && (text[i] <= ' ' || strchr(".,!?;:\"'()[]{}", text[i]))) {
            i += clen; continue;
        }
        // 跳过中文标点
        if (clen == 3 && is_sent_end(text + i)) {
            i += clen; continue;
        }

        char buf[8] = {0};
        memcpy(buf, text + i, clen);
        ids[count++] = vocab_add(buf);
        i += clen;
    }
    return count;
}

// ===================== 训练 =====================
static int* cooc = NULL; // 共现矩阵（扁平化，vocab_size × vocab_size）
static int cooc_max = 0;

// 训练
static void train_epoch(int* ids, int n, float lr) {
    if (n <= WINDOW * 2 + 1) return;
    // 初始化共现矩阵
    if (!cooc) {
        cooc_max = vocab_size;
        cooc = calloc(cooc_max * cooc_max, sizeof(int));
    }
    
    for (int t = WINDOW; t < n - WINDOW; t++) {
        int target = ids[t];

        // 计算上下文平均向量
        float ctx[NODE_FEATURE_DIM] = {0};
        for (int w = -WINDOW; w <= WINDOW; w++) {
            if (w == 0) continue;
            int idx = ids[t + w];
            for (int d = 0; d < NODE_FEATURE_DIM; d++)
                ctx[d] += vocab_emb[idx][d];
            // 记录共现
            if (cooc) {
                int a = target, b = idx;
                if (a >= 0 && a < cooc_max && b >= 0 && b < cooc_max)
                    cooc[a * cooc_max + b]++;
            }
        }
        float norm = 0;
        for (int d = 0; d < NODE_FEATURE_DIM; d++) norm += ctx[d] * ctx[d];
        norm = sqrtf(norm) + 1e-10f;
        for (int d = 0; d < NODE_FEATURE_DIM; d++) ctx[d] /= norm;

        // 计算对目标词的预测分数: ctx · emb[target]
        float score = 0;
        for (int d = 0; d < NODE_FEATURE_DIM; d++)
            score += ctx[d] * vocab_emb[target][d];

        // 对负采样词计算（随机选3个非目标词）
        float loss = -score; // 简化的损失：最大化正样本分数
        for (int s = 0; s < 3; s++) {
            int neg = rand() % vocab_size;
            if (neg < 4) neg = 4 + rand() % (vocab_size - 4);
            float neg_score = 0;
            for (int d = 0; d < NODE_FEATURE_DIM; d++)
                neg_score += ctx[d] * vocab_emb[neg][d];
            loss += logf(1 + expf(neg_score)); // 推远负样本
        }

        // 更新目标词 embedding（拉近上下文）
        for (int d = 0; d < NODE_FEATURE_DIM; d++)
            vocab_emb[target][d] += lr * ctx[d];

        // 更新窗口内的词（通过上下文）
        for (int w = -WINDOW; w <= WINDOW; w++) {
            if (w == 0) continue;
            int idx = ids[t + w];
            for (int d = 0; d < NODE_FEATURE_DIM; d++)
                vocab_emb[idx][d] += lr * vocab_emb[target][d] / (WINDOW * 2);
        }
    }
}

// ===================== 导出种子 =====================

// 用训练好的向量创建一个 MasterTopology 种子文件
#include "multi_topology.h"
#include "string_pool.h"

static int export_seed(const char* out_path) {
    MasterTopology* master = master_topology_create(9);
    master_add_sub_topology(master, TOPO_VOCABULARY, "词汇拓扑", vocab_size + 100, 10);
    master_add_sub_topology(master, TOPO_CONCEPT, "概念拓扑", vocab_size + 100, 9);

    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    SubTopology* concepts = master_get_sub_topology_by_type(master, TOPO_CONCEPT);
    if (!vocab || !concepts) { master_topology_destroy(master); return -1; }

    // 添加每个字到词汇拓扑
    int added = 0;
    for (int i = 4; i < vocab_size; i++) {
        ReasoningNode* n = huarong_net_add_node(vocab->net, vocab_words[i],
                                                 vocab_emb[i], NODE_FEATURE_DIM);
        if (n) {
            node_hash_add(vocab->node_hash, n);
            n->activation = 0.5f;
            added++;
        }
    }

    // 也作为概念添加到概念拓扑
    for (int i = 4; i < vocab_size; i++) {
        ReasoningNode* n = huarong_net_add_node(concepts->net, vocab_words[i],
                                                 vocab_emb[i], NODE_FEATURE_DIM);
        if (n) {
            node_hash_add(concepts->node_hash, n);
            n->activation = 0.5f;
        }
    }

    // 建立连接：基于训练时记录的共现次数
    int links = 0;
    int min_cooc = 3; // 至少共现这么多次才连线
    
    // 方法1: 共现计数高的互相连线
    if (cooc) {
        for (int a = 4; a < vocab_size && links < 5000; a++) {
            int node_a = a - 4; // 拓扑中的 node_id
            for (int b = a + 1; b < vocab_size && links < 5000; b++) {
                int cnt = cooc[a * cooc_max + b] + cooc[b * cooc_max + a];
                if (cnt >= min_cooc) {
                    int node_b = b - 4;
                    float weight = 0.3f + 0.7f * (cnt < 50 ? (float)cnt / 50.0f : 1.0f);
                    master_add_cross_link(master, 0, node_a, 0, node_b, weight, "cooc");
                    links++;
                }
            }
        }
    }
    
    // 如果共现太稀疏，用余弦相似度补充
    if (links < 100) {
        for (int a = 4; a < vocab_size && links < 5000; a++) {
            float best_sim = 0;
            int best_idx = -1;
            for (int b = 4; b < vocab_size; b++) {
                if (a == b) continue;
                float sim = cosine_similarity(vocab_emb[a], vocab_emb[b], NODE_FEATURE_DIM);
                if (sim > best_sim) { best_sim = sim; best_idx = b; }
            }
            if (best_idx >= 0 && best_sim > 0.2f) {
                master_add_cross_link(master, 0, a-4, 0, best_idx-4, best_sim, "semantic");
                links++;
            }
        }
    }

    printf("  导出: %d 节点, %d 链接\n", added, links);
    master_save_state(master, out_path);
    master_topology_destroy(master);
    return added;
}

// ===================== 主函数 =====================

int main(int argc, char* argv[]) {
    setbuf(stdout, NULL);
    srand((unsigned)time(NULL));

    printf("╔═══════════════════════════════════════════╗\n");
    printf("║     PivotMind 读书工具 v0.2              ║\n");
    printf("╚═══════════════════════════════════════════╝\n\n");

    const char* book_path = argc > 1 ? argv[1] : NULL;
    int epochs = argc > 2 ? atoi(argv[2]) : MAX_EPOCHS;
    if (!book_path) {
        printf("用法: ./build/bin/reader 书.txt [epochs]\n");
        return 1;
    }

    // 1. 读文本
    printf("[1/4] 阅读: %s\n", book_path);
    long len;
    char* text = read_file(book_path, &len);
    if (!text) return 1;
    printf("  ✓ %ld 字节\n", len);

    // 2. 转 token 序列
    printf("[2/4] 分词...\n");
    static int ids[MAX_CHARS];
    int n = text_to_ids(text, len, ids, MAX_CHARS);
    free(text);
    printf("  ✓ %d 个 token, 词表 %d 个字\n", n, vocab_size);

    // 3. 训练
    printf("[3/4] 训练 %d epoch (窗口=%d, lr=%.3f)...\n", epochs, WINDOW, LR);
    time_t start = time(NULL);
    for (int ep = 0; ep < epochs; ep++) {
        float lr = LR * (1.0f - (float)ep / epochs * 0.8f);
        train_epoch(ids, n, lr);
        if ((ep + 1) % 5 == 0 || ep == 0) {
            double elapsed = difftime(time(NULL), start);
            printf("  [epoch %3d/%d]  %.0fs\n", ep + 1, epochs, elapsed);
        }
    }

    // 4. 导出种子
    printf("[4/4] 导出种子...\n");
    const char* seed_path = "pivotmind_state.dat";
    int exported = export_seed(seed_path);
    if (exported > 0)
        printf("  ✓ 已导出 %s (%d 字)\n", seed_path, exported);
    else
        printf("  × 导出失败\n");

    // 清理词表
    for (int i = 4; i < vocab_size; i++)
        free(vocab_words[i]);

    double total = difftime(time(NULL), start);
    printf("\n✓ 读完！耗时 %.0f 秒\n", total);
    return 0;
}
