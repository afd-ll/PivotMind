/**
 * tools/seq2seq_trainer.c
 * PivotMind v0.2 Seq2Seq 预训练工具
 * 
 * 从 knowledge_base.json 加载 QA 数据，训练 Seq2Seq 模型并保存权重。
 * 词表基于 UTF-8 字符级分词（适配中文）。
 *
 * 编译: gcc -Iinclude -I. -Ilibs -std=gnu99 -O2 -fopenmp -D_USE_MATH_DEFINES -pthread
 *        -o build/bin/seq2seq_trainer tools/seq2seq_trainer.c
 *        src/tensor.c src/tensor_pool.c src/matrix_ops.c src/gradient_ops.c
 *        src/layer.c src/layer_rnn.c src/layer_rnn_backward.c src/layer_lstm.c
 *        src/layer_gru.c src/model.c src/optimizer.c src/trainer.c
 *        src/scheduler.c src/generative_model.c src/seq2seq_train.c
 *        src/sequence_generation.c src/model_io.c src/error.c src/attention.c
 *        src/vocab.c src/utf8_tokenizer.c -lm
 * 用法: ./build/bin/seq2seq_trainer [knowledge_base.json] [epochs]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#include "generative_model.h"
#include "tensor.h"
#include "model.h"
#include "model_io.h"
#include "utf8_tokenizer.h"

#define MAX_LINE 4096
#define MAX_PAIRS 5000
#define MAX_TOKENS 100
#define MAX_SEQ_LEN 64

// ========== JSON QA 加载 ==========

static int load_qa_json(const char* filepath, char questions[][MAX_LINE],
                        char answers[][MAX_LINE], int max_pairs) {
    FILE* fp = fopen(filepath, "rb");
    if (!fp) { printf("[错误] 无法打开: %s\n", filepath); return 0; }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize <= 0) { fclose(fp); return 0; }

    char* content = (char*)malloc(fsize + 1);
    size_t actual = fread(content, 1, fsize, fp);
    content[actual] = '\0';
    fclose(fp);

    // 去掉 \r
    int wi = 0;
    for (size_t i = 0; i < actual; i++)
        if (content[i] != '\r') content[wi++] = content[i];
    content[wi] = '\0';

    int count = 0;
    char* p = content;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    while (*p && (*p <= ' ')) p++;

    while (*p && count < max_pairs) {
        while (*p && *p != '[') p++;
        if (!*p) break;
        p++;
        while (*p && (*p <= ' ')) p++;
        if (!*p || *p == ']') continue;
        if (*p != '"') continue;
        p++;

        int qi = 0;
        while (*p && *p != '"' && qi < MAX_LINE - 1) {
            if (*p == '\\' && *(p+1)) p++;
            questions[count][qi++] = *p;
            p++;
        }
        questions[count][qi] = '\0';
        if (*p == '"') p++;
        while (*p && *p != '"') p++;
        if (!*p) break;
        p++;

        int ai = 0;
        while (*p && *p != '"' && ai < MAX_LINE - 1) {
            if (*p == '\\' && *(p+1)) p++;
            answers[count][ai++] = *p;
            p++;
        }
        answers[count][ai] = '\0';
        count++;
    }

    free(content);
    return count;
}

// ========== 训练数据构建 ==========

// 将文本分词并转为 token ID 序列
// input_ids 和 target_ids 都是 padded 到 max_seq_len
// target 是 <SOS> + answer + <EOS> 格式
// input 是 question（不包含特殊 token，编码器内部处理）

typedef struct {
    int input_len;
    int target_len;
    int input_ids[MAX_SEQ_LEN];
    int target_ids[MAX_SEQ_LEN];
} TrainPair;

static int build_vocab_and_samples(const char questions[][MAX_LINE],
                                    const char answers[][MAX_LINE],
                                    int pair_count,
                                    GenVocabulary* vocab,
                                    TrainPair* pairs) {
    int valid = 0;
    int unk_id = gen_vocab_get_word_id(vocab, "<UNK>");

    for (int i = 0; i < pair_count; i++) {
        // 分词
        char* q_tokens[MAX_TOKENS];
        char* a_tokens[MAX_TOKENS];
        int qc = tokenize_text(questions[i], q_tokens, MAX_TOKENS);
        int ac = tokenize_text(answers[i], a_tokens, MAX_TOKENS);

        if (qc == 0 || ac == 0) {
            for (int j = 0; j < qc; j++) free(q_tokens[j]);
            for (int j = 0; j < ac; j++) free(a_tokens[j]);
            continue;
        }

        // 限制长度
        if (qc > MAX_SEQ_LEN - 1) qc = MAX_SEQ_LEN - 1;
        if (ac > MAX_SEQ_LEN - 3) ac = MAX_SEQ_LEN - 3;

        // 输入: question tokens
        pairs[valid].input_len = qc;
        for (int j = 0; j < qc; j++) {
            int id = gen_vocab_add_word(vocab, q_tokens[j]);
            pairs[valid].input_ids[j] = (id >= 0) ? id : unk_id;
            free(q_tokens[j]);
        }

        // 目标: <SOS> + answer tokens + <EOS>
        pairs[valid].target_len = ac + 2;
        pairs[valid].target_ids[0] = 1; // <SOS>
        for (int j = 0; j < ac; j++) {
            int id = gen_vocab_add_word(vocab, a_tokens[j]);
            pairs[valid].target_ids[j + 1] = (id >= 0) ? id : unk_id;
            free(a_tokens[j]);
        }
        pairs[valid].target_ids[ac + 1] = 2; // <EOS>

        valid++;
    }

    return valid;
}

// ========== 训练过程 ==========

static float train_epoch(Seq2SeqModel* model, TrainPair* pairs, int count,
                         float lr, int verbose) {
    float total_loss = 0.0f;

    for (int i = 0; i < count; i++) {
        int in_len = pairs[i].input_len;
        int tgt_len = pairs[i].target_len;

        // 输入 tensor: shape (in_len,) float32
        Tensor* input_t = tensor_create(DT_FLOAT32, 1, (size_t[]){(size_t)in_len});
        for (int j = 0; j < in_len; j++)
            ((float*)input_t->data)[j] = (float)pairs[i].input_ids[j];

        // 目标 tensor: shape (tgt_len,) float32
        Tensor* target_t = tensor_create(DT_FLOAT32, 1, (size_t[]){(size_t)tgt_len});
        for (int j = 0; j < tgt_len; j++)
            ((float*)target_t->data)[j] = (float)pairs[i].target_ids[j];

        // 训练
        float loss = seq2seq_train_with_teacher_forcing(model, input_t, target_t, lr, 0);
        total_loss += loss;

        tensor_destroy(input_t);
        tensor_destroy(target_t);

        if (verbose && (i + 1) % 50 == 0)
            printf("    [%4d/%d] loss=%.4f\r", i + 1, count, total_loss / (i + 1));
    }

    return total_loss / count;
}

// ========== 验证：用模型生成回复 ==========

static void test_generation(Seq2SeqModel* model, GenVocabulary* vocab,
                            const char* input, int max_len) {
    char* response = generate_response(model, vocab, input, max_len);
    printf("  Q: %s\n", input);
    printf("  A: %s\n\n", response ? response : "(null)");
    free(response);
}

// ========== 主函数 ==========

int main(int argc, char* argv[]) {
    printf("╔═══════════════════════════════════════════╗\n");
    printf("║   PivotMind v0.2 Seq2Seq 预训练工具      ║\n");
    printf("╚═══════════════════════════════════════════╝\n\n");

    const char* kb_file = argc > 1 ? argv[1] : "data/knowledge_base.json";
    int epochs = argc > 2 ? atoi(argv[2]) : 100;
    int embed_dim = 64;
    int hidden_dim = 128;
    int max_seq_len = MAX_SEQ_LEN;

    // 1. 加载 QA 数据
    printf("[1/6] 加载知识库: %s\n", kb_file);
    static char questions[MAX_PAIRS][MAX_LINE];
    static char answers[MAX_PAIRS][MAX_LINE];
    int pair_count = load_qa_json(kb_file, questions, answers, MAX_PAIRS);
    if (pair_count == 0) {
        printf("  错误: 无有效数据\n");
        return 1;
    }
    printf("  ✓ %d 条问答对\n", pair_count);

    // 2. 构建词表
    printf("[2/6] 构建词表...\n");
    GenVocabulary* vocab = gen_vocab_create(5000);

    // 构建训练样本（这会同时填词表）
    static TrainPair pairs[MAX_PAIRS];
    int train_count = build_vocab_and_samples(questions, answers, pair_count,
                                               vocab, pairs);
    printf("  ✓ 词表大小: %d, 有效训练样本: %d\n", vocab->size, train_count);

    // 3. 创建模型
    printf("[3/6] 创建 Seq2Seq 模型 (emb=%d, hidden=%d)...\n", embed_dim, hidden_dim);
    Seq2SeqModel* model = seq2seq_create(vocab->size, embed_dim, hidden_dim, max_seq_len);
    if (!model) {
        printf("  错误: 无法创建模型\n");
        gen_vocab_destroy(vocab);
        return 1;
    }
    printf("  ✓ 编码器: %zu 层, 解码器: %zu 层\n",
           model->encoder->num_layers, model->decoder->num_layers);

    // 4. 训练
    printf("[4/6] 开始训练 (lr=0.01, epochs=%d)...\n", epochs);

    // 先试几轮看初始 loss
    float initial_loss = train_epoch(model, pairs, train_count, 0.01f, 1);
    printf("  初始平均 loss: %.4f\n", initial_loss);

    time_t start = time(NULL);
    for (int ep = 0; ep < epochs; ep++) {
        float lr = 0.01f * (1.0f - (float)ep / epochs * 0.9f); // 线性衰减
        float avg_loss = train_epoch(model, pairs, train_count, lr, 0);

        if ((ep + 1) % 10 == 0 || ep == 0) {
            double elapsed = difftime(time(NULL), start);
            printf("  [epoch %3d/%d] loss=%.4f  (%.0fs)\n",
                   ep + 1, epochs, avg_loss, elapsed);
        }
    }
    double total_time = difftime(time(NULL), start);
    printf("  ✓ 训练完成, 耗时 %.0f 秒\n", total_time);

    // 5. 测试
    printf("[5/6] 生成测试...\n");
    for (int i = 0; i < 5 && i < train_count; i++) {
        test_generation(model, vocab, questions[i], 50);
    }

    // 6. 保存模型
    printf("[6/6] 保存模型...\n");
    const char* model_file = "seq2seq_model.bin";
    bool ok = model_save(model->encoder, "seq2seq_encoder.bin", NULL);
    ok = ok && model_save(model->decoder, "seq2seq_decoder.bin", NULL);
    
    if (ok) {
        printf("  ✓ 模型已保存到 seq2seq_encoder.bin / seq2seq_decoder.bin\n");
    } else {
        printf("  × 保存失败\n");
    }

    // 保存词表
    FILE* vf = fopen("seq2seq_vocab.txt", "w");
    if (vf) {
        for (int i = 0; i < vocab->size; i++)
            fprintf(vf, "%s\n", vocab->words[i]);
        fclose(vf);
        printf("  ✓ 词表已保存到 seq2seq_vocab.txt\n");
    }

    // 清理
    seq2seq_destroy(model);
    gen_vocab_destroy(vocab);
    printf("\n✓ 完成!\n");
    return 0;
}
