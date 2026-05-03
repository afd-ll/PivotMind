#include "../include/pretrain.h"
#include "../include/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

// ========== 宏定义 ==========

#define UNIGRAM_TABLE_SIZE 10000000
#define MAX_LINE_LEN 8192
#define PAIR_BUFFER_SIZE 4096

// ========== 前向声明 ==========

static int _build_unigram_table(PretrainState* state);
static int _sample_negative(PretrainState* state, int exclude_id, int* out_ids, int count);
static float _compute_subsample_prob(Vocab* vocab, int word_id, float thresh);
static int _dynamic_window_size(PretrainConfig* config, int position, int seq_len);
static float _train_pair_with_lr(PretrainState* state, int center_id, int context_id,
                                  float lr, int neg_count, float distance_weight);
static void _process_text_line(PretrainState* state, const char* text,
                                PretrainConfig* config, float lr,
                                int* total_pairs, float* total_loss, int* subsampled);
static int _compare_pair_score(const void* a, const void* b);
static int _save_checkpoint_extended(PretrainState* state, const char* filepath, int current_line);

// ========== 配置创建 ==========

PretrainConfig* pretrain_config_create_default(void) {
    PretrainConfig* cfg = (PretrainConfig*)malloc(sizeof(PretrainConfig));
    if (!cfg) return NULL;

    cfg->embedding_dim = 64;
    cfg->window_size = 5;
    cfg->window_size_max = 10;
    cfg->negative_samples = 5;
    cfg->learning_rate = 0.025f;
    cfg->learning_rate_min = 0.0001f;
    cfg->epochs = 10;
    cfg->batch_size = 256;
    cfg->subsample_thresh = 1e-3f;
    cfg->min_count = 1;
    cfg->max_vocab_size = 50000;
    cfg->num_workers = 1;
    cfg->sample_rate = 1.0f;
    cfg->save_every_n_lines = 0;
    cfg->validate_every_n_lines = 0;
    cfg->verbose = 1;

    // 新增默认配置
    cfg->mode = PRETRAIN_MODE_SKIPGRAM;     // 默认Skip-gram
    cfg->use_momentum = 1;                   // 默认使用动量
    cfg->momentum = 0.9f;                    // 动量系数
    cfg->use_grad_clip = 1;                  // 默认梯度裁剪
    cfg->grad_clip_value = 5.0f;             // 裁剪阈值
    cfg->phrase_min_count = 5;              // 短语检测最小共现
    cfg->phrase_threshold = 100.0f;          // PMI阈值
    cfg->use_position_weight = 1;           // 使用位置权重
    cfg->warmup_ratio = 0.1f;               // 预热比例
    return cfg;
}

PretrainConfig* pretrain_config_create_quality(void) {
    PretrainConfig* cfg = (PretrainConfig*)malloc(sizeof(PretrainConfig));
    if (!cfg) return NULL;

    cfg->embedding_dim = 128;
    cfg->window_size = 8;
    cfg->window_size_max = 15;
    cfg->negative_samples = 10;
    cfg->learning_rate = 0.025f;
    cfg->learning_rate_min = 0.0001f;
    cfg->epochs = 20;
    cfg->batch_size = 512;
    cfg->subsample_thresh = 1e-4f;
    cfg->min_count = 2;
    cfg->max_vocab_size = 100000;
    cfg->num_workers = 1;
    cfg->sample_rate = 1.0f;
    cfg->save_every_n_lines = 5000;
    cfg->validate_every_n_lines = 5000;
    cfg->verbose = 1;

    // 新增高质量配置
    cfg->mode = PRETRAIN_MODE_SKIPGRAM;
    cfg->use_momentum = 1;
    cfg->momentum = 0.9f;
    cfg->use_grad_clip = 1;
    cfg->grad_clip_value = 5.0f;
    cfg->phrase_min_count = 10;
    cfg->phrase_threshold = 100.0f;
    cfg->use_position_weight = 1;
    cfg->warmup_ratio = 0.1f;
    return cfg;
}

void pretrain_config_destroy(PretrainConfig* cfg) {
    if (cfg) free(cfg);
}

// ========== 状态管理 ==========

PretrainState* pretrain_state_create(Vocab* vocab, int embedding_dim) {
    PretrainState* state = (PretrainState*)malloc(sizeof(PretrainState));
    if (!state) return NULL;
    memset(state, 0, sizeof(PretrainState));

    state->vocab = vocab;
    state->embedding_layer = layer_create_embedding(vocab->size, embedding_dim);
    if (!state->embedding_layer) {
        free(state);
        return NULL;
    }

    int vocab_size = vocab->size;
    state->context_weights = (float*)calloc(vocab_size * embedding_dim, sizeof(float));
    if (!state->context_weights) {
        layer_destroy(state->embedding_layer);
        free(state);
        return NULL;
    }

    // Xavier初始化
    float scale = sqrtf(2.0f / (vocab_size + embedding_dim));
    for (int i = 0; i < vocab_size * embedding_dim; i++) {
        state->context_weights[i] = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * scale;
    }

    // 初始化动量缓冲区
    state->grad_momentum_embed = (float*)calloc(vocab_size * embedding_dim, sizeof(float));
    state->grad_momentum_context = (float*)calloc(vocab_size * embedding_dim, sizeof(float));

    // 初始化批处理缓冲区
    state->batch_buffer_size = PAIR_BUFFER_SIZE;
    state->batch_buffer = (WordPair*)malloc(state->batch_buffer_size * sizeof(WordPair));

    // 默认Skip-gram模式
    state->mode = PRETRAIN_MODE_SKIPGRAM;
    state->total_words = 0;
    state->trained_words = 0;
    state->epoch = 0;
    state->loss = 0.0f;
    state->loss_count = 0.0f;
    state->current_lr = 0.025f;
    state->lr_decay_factor = 0.99f;
    state->use_hs = 0;
    state->use_subsample = 1;
    state->best_loss = 1e9f;
    state->no_improve_count = 0;
    state->train_start_time = clock();
    state->total_pairs = 0;
    state->subsampled_pairs = 0;
    state->neg_samples_used = 0;

    return state;
}

// 使用指定配置创建预训练状态
PretrainState* pretrain_state_create_with_config(Vocab* vocab, PretrainConfig* config) {
    if (!vocab || !config) return NULL;

    PretrainState* state = pretrain_state_create(vocab, config->embedding_dim);
    if (!state) return NULL;

    state->mode = config->mode;
    state->current_lr = config->learning_rate;

    // 计算预热步数
    state->warmup_steps = (int)(config->batch_size * config->epochs * config->warmup_ratio);

    return state;
}

int pretrain_state_init_advanced(PretrainState* state, PretrainConfig* config) {
    if (!state || !config) return -1;

    state->current_lr = config->learning_rate;
    state->use_subsample = (config->subsample_thresh > 0);

    // 构建unigram表用于负采样
    if (state->vocab && state->vocab->size > 0) {
        if (_build_unigram_table(state) != 0) {
            fprintf(stderr, "[预训练] 警告: unigram表构建失败，将使用均匀采样\n");
        }
    }

    return 0;
}

int _build_unigram_table(PretrainState* state) {
    Vocab* vocab = state->vocab;
    if (!vocab || vocab->size <= 5) return -1;

    // 计算词频分布 (使用0.75次幂，和Word2Vec一致)
    double total_freq = 0.0;
    double* probs = (double*)malloc(vocab->size * sizeof(double));
    if (!probs) return -1;

    for (int i = 0; i < vocab->size; i++) {
        double f = (double)vocab->entries[i].freq;
        probs[i] = pow(f, 0.75);
        total_freq += probs[i];
    }

    // 构建累积分布
    state->unigram_cumsum = (float*)malloc(vocab->size * sizeof(float));
    if (!state->unigram_cumsum) {
        free(probs);
        return -1;
    }

    for (int i = 0; i < vocab->size; i++) {
        probs[i] /= total_freq;
        state->unigram_cumsum[i] = (float)probs[i];
        if (i > 0) state->unigram_cumsum[i] += state->unigram_cumsum[i-1];
    }

    // 构建采样表
    state->unigram_table_size = UNIGRAM_TABLE_SIZE;
    state->unigram_table = (int*)malloc(state->unigram_table_size * sizeof(int));
    if (!state->unigram_table) {
        free(probs);
        free(state->unigram_cumsum);
        state->unigram_cumsum = NULL;
        return -1;
    }

    int table_idx = 0;
    for (int i = 0; i < vocab->size && table_idx < UNIGRAM_TABLE_SIZE; i++) {
        int count = (int)(probs[i] * UNIGRAM_TABLE_SIZE);
        for (int j = 0; j < count && table_idx < UNIGRAM_TABLE_SIZE; j++) {
            state->unigram_table[table_idx++] = i;
        }
    }
    // 填满剩余
    while (table_idx < UNIGRAM_TABLE_SIZE) {
        state->unigram_table[table_idx++] = rand() % vocab->size;
    }

    free(probs);
    return 0;
}

int _sample_negative(PretrainState* state, int exclude_id, int* out_ids, int count) {
    if (!state || !out_ids || count <= 0) return -1;

    Vocab* vocab = state->vocab;
    for (int i = 0; i < count; i++) {
        if (state->unigram_table && state->unigram_table_size > 0) {
            // 基于词频的采样
            int idx = rand() % state->unigram_table_size;
            int sampled = state->unigram_table[idx];
            int tries = 0;
            while ((sampled == exclude_id || sampled < 5) && tries < 100) {
                idx = rand() % state->unigram_table_size;
                sampled = state->unigram_table[idx];
                tries++;
            }
            out_ids[i] = sampled;
        } else {
            // 均匀采样
            int sampled;
            do {
                sampled = rand() % vocab->size;
            } while (sampled == exclude_id || sampled < 5);
            out_ids[i] = sampled;
        }
    }
    return 0;
}

void pretrain_state_destroy(PretrainState* state) {
    if (!state) return;
    if (state->context_weights) free(state->context_weights);
    if (state->unigram_table) free(state->unigram_table);
    if (state->unigram_cumsum) free(state->unigram_cumsum);
    if (state->embedding_layer) layer_destroy(state->embedding_layer);
    // 新增：释放动量缓冲区
    if (state->grad_momentum_embed) free(state->grad_momentum_embed);
    if (state->grad_momentum_context) free(state->grad_momentum_context);
    if (state->batch_buffer) free(state->batch_buffer);
    free(state);
}

int pretrain_save_weights(PretrainState* state, const char* filepath) {
    if (!state || !state->embedding_layer) return -1;

    FILE* fp = fopen(filepath, "wb");
    if (!fp) {
        fprintf(stderr, "[预训练] 无法创建文件: %s\n", filepath);
        return -1;
    }

    Tensor* w = state->embedding_layer->weights;
    int vocab_size = w->shape[0];
    int embed_dim = w->shape[1];

    // 文件头
    int magic = 0x50524554; // "PRET"
    fwrite(&magic, sizeof(int), 1, fp);
    fwrite(&vocab_size, sizeof(int), 1, fp);
    fwrite(&embed_dim, sizeof(int), 1, fp);

    // 写入词嵌入
    fwrite(w->data, sizeof(float), vocab_size * embed_dim, fp);

    // 写入上下文权重
    fwrite(state->context_weights, sizeof(float), vocab_size * embed_dim, fp);

    // 写入统计信息
    fwrite(&state->total_pairs, sizeof(int), 1, fp);
    fwrite(&state->loss, sizeof(float), 1, fp);

    fclose(fp);
    printf("[预训练] 权重已保存: %s (vocab=%d, dim=%d, pairs=%d)\n",
           filepath, vocab_size, embed_dim, state->total_pairs);
    return 0;
}

int pretrain_save_embeddings_only(PretrainState* state, const char* filepath) {
    if (!state || !state->embedding_layer) return -1;

    FILE* fp = fopen(filepath, "wb");
    if (!fp) return -1;

    Tensor* w = state->embedding_layer->weights;
    int vocab_size = w->shape[0];
    int embed_dim = w->shape[1];

    fwrite(&vocab_size, sizeof(int), 1, fp);
    fwrite(&embed_dim, sizeof(int), 1, fp);
    fwrite(w->data, sizeof(float), vocab_size * embed_dim, fp);

    fclose(fp);
    printf("[预训练] 嵌入向量已保存: %s (vocab=%d, dim=%d)\n",
           filepath, vocab_size, embed_dim);
    return 0;
}

PretrainState* pretrain_state_load(Vocab* vocab, const char* weights_path) {
    FILE* fp = fopen(weights_path, "rb");
    if (!fp) return NULL;

    int magic, vocab_size, embed_dim;
    fread(&magic, sizeof(int), 1, fp);

    // 检测格式
    int is_full_format = (magic == 0x50524554);

    if (is_full_format) {
        fread(&vocab_size, sizeof(int), 1, fp);
        fread(&embed_dim, sizeof(int), 1, fp);
    } else {
        // 兼容旧格式：magic实际是vocab_size
        vocab_size = magic;
        fread(&embed_dim, sizeof(int), 1, fp);
    }

    PretrainState* state = (PretrainState*)malloc(sizeof(PretrainState));
    if (!state) { fclose(fp); return NULL; }
    memset(state, 0, sizeof(PretrainState));

    state->vocab = vocab;
    state->embedding_layer = layer_create_embedding(vocab_size, embed_dim);
    if (!state->embedding_layer) {
        free(state);
        fclose(fp);
        return NULL;
    }

    if (is_full_format) {
        // 已在前面读取了 magic, vocab_size, embed_dim
        fread(state->embedding_layer->weights->data, sizeof(float),
              vocab_size * embed_dim, fp);
        // 读取上下文权重
        state->context_weights = (float*)malloc(vocab_size * embed_dim * sizeof(float));
        if (state->context_weights) {
            fread(state->context_weights, sizeof(float), vocab_size * embed_dim, fp);
        }
        // 读取统计
        fread(&state->total_pairs, sizeof(int), 1, fp);
        fread(&state->loss, sizeof(float), 1, fp);
    } else {
        // 旧格式：vocab_size和embed_dim已在前面读取
        fread(state->embedding_layer->weights->data, sizeof(float),
              vocab_size * embed_dim, fp);
        state->context_weights = (float*)calloc(vocab_size * embed_dim, sizeof(float));
    }

    state->train_start_time = clock();
    fclose(fp);
    printf("[预训练] 权重已加载: %s (vocab=%d, dim=%d)\n",
           weights_path, vocab_size, embed_dim);
    return state;
}

// ========== 核心训练函数 ==========

static float sigmoid(float x) {
    if (x > 10.0f) return 1.0f;
    if (x < -10.0f) return 0.0f;
    return 1.0f / (1.0f + expf(-x));
}

static float _compute_subsample_prob(Vocab* vocab, int word_id, float thresh) {
    if (!vocab || word_id < 0 || word_id >= vocab->size) return 1.0f;
    if (word_id < 5) return 0.0f; // 跳过特殊token

    float freq = (float)vocab->entries[word_id].freq;
    float total = (float)vocab->max_freq;
    if (total <= 0) return 1.0f;

    float ratio = freq / total;
    // Word2Vec下采样公式: P(discard) = 1 - sqrt(t/f)
    float prob = sqrtf(thresh / ratio);
    if (prob >= 1.0f) return 1.0f;

    // 随机决定是否保留
    float rand_val = (float)rand() / RAND_MAX;
    return (rand_val < prob) ? prob : 0.0f;
}

static int _dynamic_window_size(PretrainConfig* config, int position, int seq_len) {
    int base = config->window_size;
    int max_win = config->window_size_max;
    if (max_win <= base) return base;

    // 动态窗口：句子中间用大窗口，两端用小窗口
    float pos_ratio = (float)position / (seq_len > 1 ? seq_len - 1 : 1);
    float win_range = (float)(max_win - base);
    int dynamic_win = base + (int)(win_range * (1.0f - fabsf(pos_ratio - 0.5f) * 2.0f));
    return dynamic_win > base ? dynamic_win : base;
}

static float _train_pair_with_lr(PretrainState* state, int center_id, int context_id,
                                  float lr, int neg_count, float distance_weight) {
    float* embed_w = (float*)state->embedding_layer->weights->data;
    int embed_dim = state->embedding_layer->weights->shape[1];
    int vocab_size = state->embedding_layer->weights->shape[0];

    if (center_id < 0 || center_id >= vocab_size ||
        context_id < 0 || context_id >= vocab_size) {
        return 0.0f;
    }

    float* center_vec = embed_w + center_id * embed_dim;
    float* context_vec = state->context_weights + context_id * embed_dim;

    // 正样本
    float score = 0.0f;
    for (int d = 0; d < embed_dim; d++) {
        score += center_vec[d] * context_vec[d];
    }
    float sig = sigmoid(score);
    float grad_scale = lr * distance_weight * (1.0f - sig);
    float loss = -logf(sig + 1e-7f);

    float* grad_center = (float*)calloc(embed_dim, sizeof(float));
    if (!grad_center) return loss;

    for (int d = 0; d < embed_dim; d++) {
        grad_center[d] += grad_scale * context_vec[d];
        context_vec[d] += grad_scale * center_vec[d];
    }

    // 负样本
    int* neg_ids = (int*)malloc(neg_count * sizeof(int));
    if (neg_ids) {
        _sample_negative(state, context_id, neg_ids, neg_count);

        for (int n = 0; n < neg_count; n++) {
            int neg_id = neg_ids[n];
            float* neg_vec = state->context_weights + neg_id * embed_dim;

            score = 0.0f;
            for (int d = 0; d < embed_dim; d++) {
                score += center_vec[d] * neg_vec[d];
            }
            sig = sigmoid(score);
            grad_scale = lr * distance_weight * (0.0f - sig);
            loss += -logf(1.0f - sig + 1e-7f);

            for (int d = 0; d < embed_dim; d++) {
                grad_center[d] += grad_scale * neg_vec[d];
                neg_vec[d] += grad_scale * center_vec[d];
            }
        }
        free(neg_ids);
    }

    for (int d = 0; d < embed_dim; d++) {
        center_vec[d] += grad_center[d];
    }

    free(grad_center);
    return loss;
}

// 处理一行文本（支持Skip-gram和CBOW模式）
static void _process_text_line(PretrainState* state, const char* text,
                                PretrainConfig* config, float lr,
                                int* total_pairs, float* total_loss, int* subsampled) {
    int ids[1024];
    int seq_len = vocab_encode(state->vocab, text, ids, 1024);
    if (seq_len < 2) return;

    int vocab_size = state->embedding_layer->weights->shape[0];

    for (int i = 0; i < seq_len; i++) {
        if (ids[i] < 5) continue;

        // 下采样检查
        if (config->subsample_thresh > 0 && state->use_subsample) {
            float keep_prob = _compute_subsample_prob(state->vocab, ids[i],
                                                       config->subsample_thresh);
            if (keep_prob <= 0.0f) {
                (*subsampled)++;
                continue;
            }
        }

        // 动态窗口
        int dynamic_win = _dynamic_window_size(config, i, seq_len);
        int half_win = dynamic_win / 2;

        if (state->mode == PRETRAIN_MODE_Cbow) {
            // CBOW模式: 用上下文词预测中心词
            int context_ids[64];
            int ctx_count = 0;

            for (int j = fmax(0, i - half_win); j <= fmin(seq_len - 1, i + half_win); j++) {
                if (i == j || ids[j] < 5) continue;
                if (ids[j] >= vocab_size) continue;
                if (ctx_count < 64) {
                    context_ids[ctx_count++] = ids[j];
                }
            }

            if (ctx_count > 0) {
                float loss = pretrain_train_cbow(state, context_ids, ctx_count,
                                                  ids[i], lr, config->negative_samples);
                *total_loss += loss;
                (*total_pairs)++;
            }
        } else {
            // Skip-gram模式: 用中心词预测上下文词
            for (int j = fmax(0, i - half_win); j <= fmin(seq_len - 1, i + half_win); j++) {
                if (i == j || ids[j] < 5) continue;

                // 距离衰减权重（可选位置权重）
                int distance = abs(i - j);
                float dist_weight = 1.0f / (1.0f + 0.1f * distance);

                // 添加到批处理缓冲区
                if (state->batch_buffer && state->batch_buffer_count < state->batch_buffer_size) {
                    WordPair* pair = &state->batch_buffer[state->batch_buffer_count++];
                    pair->center_id = ids[i];
                    pair->context_id = ids[j];
                    pair->distance = distance;
                    pair->weight = dist_weight;

                    // 批次满时刷新
                    if (state->batch_buffer_count >= state->batch_buffer_size) {
                        pretrain_flush_batch(state, config);
                    }
                }

                float loss = _train_pair_with_lr(state, ids[i], ids[j], lr,
                                                 config->negative_samples, dist_weight);
                *total_loss += loss;
                (*total_pairs)++;
            }
        }
    }
}

// ========== 预训练主循环 ==========

int pretrain_from_file(PretrainState* state, const char* filepath, PretrainConfig* config) {
    FILE* fp = fopen(filepath, "r");
    if (!fp) {
        fprintf(stderr, "[预训练] 无法打开文件: %s\n", filepath);
        return -1;
    }

    printf("[预训练] 开始处理: %s\n", filepath);
    if (config->verbose) {
        printf("[预训练] 配置: dim=%d, window=%d, neg=%d, lr=%.4f, subsample=%.4f\n",
               config->embedding_dim, config->window_size, config->negative_samples,
               config->learning_rate, config->subsample_thresh);
    }

    char line[MAX_LINE_LEN];
    int total_pairs = 0;
    int line_count = 0;
    float total_loss = 0.0f;
    int subsampled = 0;
    float lr = state->current_lr;

    clock_t epoch_start = clock();

    while (fgets(line, sizeof(line), fp)) {
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0 || line[0] == '#') continue;

        // 处理对话格式: 问题|答案
        char* pipe = strchr(line, '|');
        if (pipe) {
            *pipe = '\0';
            _process_text_line(state, line, config, lr, &total_pairs, &total_loss, &subsampled);
            _process_text_line(state, pipe + 1, config, lr, &total_pairs, &total_loss, &subsampled);
        } else {
            _process_text_line(state, line, config, lr, &total_pairs, &total_loss, &subsampled);
        }

        line_count++;

        // 动态学习率衰减
        if (line_count % 5000 == 0 && line_count > 0) {
            lr = lr * state->lr_decay_factor;
            if (lr < config->learning_rate_min) lr = config->learning_rate_min;
            state->current_lr = lr;
        }

        if (config->verbose && line_count % 1000 == 0) {
            float elapsed = (float)(clock() - epoch_start) / CLOCKS_PER_SEC;
            float speed = elapsed > 0 ? total_pairs / elapsed : 0.0f;
            float avg_loss = total_pairs > 0 ? total_loss / total_pairs : 0.0f;
            printf("\r[预训练] 行=%d, 词对=%d, 损失=%.4f, lr=%.5f, 速度=%.0f 词/秒, 采样跳过=%d",
                   line_count, total_pairs, avg_loss, lr, speed, subsampled);
            fflush(stdout);
        }

        // 检查点保存（使用扩展格式）
        if (config->save_every_n_lines > 0 && line_count % config->save_every_n_lines == 0) {
            char ckpt_path[256];
            snprintf(ckpt_path, sizeof(ckpt_path), "pretrain_ckpt_%d.bin", line_count);
            _save_checkpoint_extended(state, ckpt_path, line_count);
        }
    }

    fclose(fp);

    float elapsed = (float)(clock() - epoch_start) / CLOCKS_PER_SEC;
    float avg_loss = total_pairs > 0 ? total_loss / total_pairs : 0.0f;
    float speed = elapsed > 0 ? total_pairs / elapsed : 0.0f;

    if (config->verbose) {
        printf("\n[预训练] 完成: %s\n", filepath);
        printf("  行数=%d, 词对=%d, 损失=%.4f\n", line_count, total_pairs, avg_loss);
        printf("  用时=%.1fs, 速度=%.0f 词/秒\n", elapsed, speed);
        printf("  下采样跳过=%d (%.1f%%)\n", subsampled,
               total_pairs + subsampled > 0 ?
               100.0f * subsampled / (total_pairs + subsampled) : 0.0f);
    }

    state->total_words += total_pairs;
    state->trained_words += total_pairs;
    state->total_pairs += total_pairs;
    state->loss = avg_loss;
    state->loss_count += (float)total_pairs;
    state->subsampled_pairs += subsampled;
    state->neg_samples_used += total_pairs * config->negative_samples;

    // 刷新剩余批处理缓冲区
    if (state->batch_buffer_count > 0) {
        pretrain_flush_batch(state, config);
    }

    return total_pairs;
}

int pretrain_from_files(PretrainState* state, const char** filepaths, int count,
                        PretrainConfig* config) {
    int total = 0;

    for (int epoch = 0; epoch < config->epochs; epoch++) {
        state->epoch = epoch;
        printf("\n========== 预训练 Epoch %d/%d ==========\n", epoch + 1, config->epochs);

        // 每个epoch重置学习率
        state->current_lr = config->learning_rate;

        for (int i = 0; i < count; i++) {
            int n = pretrain_from_file(state, filepaths[i], config);
            if (n > 0) total += n;
        }

        // Epoch结束统计
        float elapsed = (float)(clock() - state->train_start_time) / CLOCKS_PER_SEC;
        printf("[Epoch %d] 总词对=%d, 总损失=%.4f, 用时=%.1fs\n",
               epoch + 1, total, state->loss, elapsed);
    }

    return total;
}

int pretrain_from_text(PretrainState* state, const char* text, PretrainConfig* config) {
    if (!state || !text || !config) return -1;

    char* buf = strdup(text);
    if (!buf) return -1;

    int total_pairs = 0;
    float total_loss = 0.0f;
    int subsampled = 0;
    float lr = state->current_lr;

    char* saveptr;
    char* line = strtok_r(buf, "\n", &saveptr);
    while (line) {
        // 去除换行
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }
        if (len > 0 && line[0] != '#') {
            char* pipe = strchr(line, '|');
            if (pipe) {
                *pipe = '\0';
                _process_text_line(state, line, config, lr, &total_pairs, &total_loss, &subsampled);
                _process_text_line(state, pipe + 1, config, lr, &total_pairs, &total_loss, &subsampled);
            } else {
                _process_text_line(state, line, config, lr, &total_pairs, &total_loss, &subsampled);
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    free(buf);

    state->total_words += total_pairs;
    state->trained_words += total_pairs;
    state->total_pairs += total_pairs;
    state->loss = total_pairs > 0 ? total_loss / total_pairs : 0.0f;

    return total_pairs;
}

// ========== 查询API ==========

int pretrain_get_embedding(PretrainState* state, const char* word, float* vec) {
    if (!state || !word || !vec) return -1;

    int id = vocab_lookup(state->vocab, word);
    if (id == VOCAB_UNK_ID) return -1;

    return pretrain_get_embedding_by_id(state, id, vec);
}

int pretrain_get_embedding_by_id(PretrainState* state, int word_id, float* vec) {
    if (!state || word_id < 0 || !vec) return -1;

    Tensor* w = state->embedding_layer->weights;
    if (word_id >= (int)w->shape[0]) return -1;

    int dim = w->shape[1];
    float* embed_data = (float*)w->data;
    memcpy(vec, embed_data + word_id * dim, dim * sizeof(float));
    return 0;
}

float pretrain_cosine_similarity(const float* vec1, const float* vec2, int dim) {
    if (!vec1 || !vec2 || dim <= 0) return 0.0f;

    float dot = 0.0f, norm1 = 0.0f, norm2 = 0.0f;
    for (int i = 0; i < dim; i++) {
        dot += vec1[i] * vec2[i];
        norm1 += vec1[i] * vec1[i];
        norm2 += vec2[i] * vec2[i];
    }
    norm1 = sqrtf(norm1 + 1e-7f);
    norm2 = sqrtf(norm2 + 1e-7f);
    return dot / (norm1 * norm2 + 1e-7f);
}

float pretrain_euclidean_distance(const float* vec1, const float* vec2, int dim) {
    if (!vec1 || !vec2 || dim <= 0) return 0.0f;

    float dist = 0.0f;
    for (int i = 0; i < dim; i++) {
        float diff = vec1[i] - vec2[i];
        dist += diff * diff;
    }
    return sqrtf(dist);
}

static int _compare_pair_score(const void* a, const void* b) {
    typedef struct { int id; float score; } Cand;
    const Cand* pa = (const Cand*)a;
    const Cand* pb = (const Cand*)b;
    if (pa->score < pb->score) return 1;
    if (pa->score > pb->score) return -1;
    return 0;
}

int pretrain_find_similar(PretrainState* state, const char* word, int top_k,
                          char** results, float* scores) {
    if (!state || !word || !results || !scores) return 0;

    float vec[256]; // 最大dim=128
    int dim = state->embedding_layer->weights->shape[1];

    if (dim > 256) dim = 256;

    if (pretrain_get_embedding(state, word, vec) != 0) return 0;

    return pretrain_find_similar_by_vec(state, vec, top_k, results, scores);
}

int pretrain_find_similar_by_vec(PretrainState* state, const float* vec, int top_k,
                                 char** results, float* scores) {
    if (!state || !vec || !results || !scores) return 0;

    float* embed_data = (float*)state->embedding_layer->weights->data;
    int dim = state->embedding_layer->weights->shape[1];
    int vocab_size = state->embedding_layer->weights->shape[0];

    // 预计算查询向量范数
    float query_norm = 0.0f;
    for (int d = 0; d < dim; d++) query_norm += vec[d] * vec[d];
    query_norm = sqrtf(query_norm + 1e-7f);
    if (query_norm < 1e-7f) return 0;

    // 使用部分排序找top_k
    int k = top_k < vocab_size ? top_k : vocab_size;

    // 定义局部结构避免匿名结构问题
    typedef struct { int id; float score; } Cand;
    Cand* candidates = (Cand*)malloc(k * sizeof(Cand));

    if (!candidates) return 0;

    // 第一遍：收集前k个
    int filled = 0;
    for (int i = 0; i < vocab_size; i++) {
        float* wvec = embed_data + i * dim;
        float dot = 0.0f, norm = 0.0f;
        for (int d = 0; d < dim; d++) {
            dot += vec[d] * wvec[d];
            norm += wvec[d] * wvec[d];
        }
        norm = sqrtf(norm + 1e-7f);
        float sim = dot / (query_norm * norm + 1e-7f);

        if (filled < k) {
            candidates[filled].id = i;
            candidates[filled].score = sim;
            filled++;
            if (filled == k) {
                qsort(candidates, k, sizeof(candidates[0]), _compare_pair_score);
            }
        } else if (sim > candidates[k-1].score) {
            // 替换最小的
            candidates[k-1].id = i;
            candidates[k-1].score = sim;
            // 部分排序维护top-k性质
            for (int p = k - 2; p >= 0; p--) {
                if (candidates[p].score < candidates[p+1].score) {
                    Cand tmp = candidates[p];
                    candidates[p] = candidates[p+1];
                    candidates[p+1] = tmp;
                } else break;
            }
        }
    }

    int found = 0;
    for (int i = 0; i < filled; i++) {
        results[found] = state->vocab->entries[candidates[i].id].word;
        scores[found] = candidates[i].score;
        found++;
    }

    free(candidates);
    return found;
}

// ========== 统计和评估 ==========

void pretrain_print_progress(PretrainState* state) {
    if (!state) return;
    float elapsed = state->train_start_time ?
                    (float)(clock() - state->train_start_time) / CLOCKS_PER_SEC : 0.0f;
    printf("[预训练] epoch=%d, trained=%d, loss=%.4f, lr=%.5f, time=%.1fs\n",
           state->epoch, state->trained_words, state->loss,
           state->current_lr, elapsed);
}

void pretrain_print_stats(PretrainState* state) {
    if (!state) return;
    float elapsed = state->train_start_time ?
                    (float)(clock() - state->train_start_time) / CLOCKS_PER_SEC : 0.0f;
    float avg_loss = state->loss_count > 0 ? state->loss / state->loss_count : state->loss;
    float speed = elapsed > 0 ? state->trained_words / elapsed : 0.0f;

    printf("========== 预训练统计 ==========\n");
    printf("  总词对:     %d\n", state->total_pairs);
    printf("  损失:       %.4f\n", avg_loss);
    printf("  用时:       %.1fs\n", elapsed);
    printf("  速度:       %.0f 词/秒\n", speed);
    printf("  当前学习率: %.5f\n", state->current_lr);
    printf("  下采样跳过: %d (%.1f%%)\n", state->subsampled_pairs,
           state->total_pairs > 0 ?
           100.0f * state->subsampled_pairs / state->total_pairs : 0.0f);
    printf("  负采样数:   %d (平均每对%.1f个)\n",
           state->neg_samples_used,
           state->total_pairs > 0 ?
           (float)state->neg_samples_used / state->total_pairs : 0.0f);
    printf("  词表大小:   %d\n", state->vocab ? state->vocab->size : 0);
    printf("================================\n");
}

float pretrain_get_speed(PretrainState* state) {
    if (!state) return 0.0f;
    float elapsed = state->train_start_time ?
                    (float)(clock() - state->train_start_time) / CLOCKS_PER_SEC : 0.0f;
    return elapsed > 0 ? state->trained_words / elapsed : 0.0f;
}

float pretrain_compute_coverage(PretrainState* state, const char* text) {
    if (!state || !text || !state->vocab) return 0.0f;

    int ids[1024];
    int seq_len = vocab_encode(state->vocab, text, ids, 1024);
    if (seq_len == 0) return 0.0f;

    int covered = 0;
    for (int i = 0; i < seq_len; i++) {
        if (ids[i] != VOCAB_UNK_ID) covered++;
    }
    return (float)covered / seq_len;
}

float pretrain_evaluate_analogy(PretrainState* state, const char** test_pairs, int count) {
    if (!state || !test_pairs || count <= 0) return 0.0f;

    int correct = 0;
    int dim = state->embedding_layer->weights->shape[1];

    if (dim > 256) return 0.0f;

    for (int i = 0; i < count; i++) {
        // 格式: "king man woman queen"
        // 期望: king - man + woman ≈ queen
        char buf[256];
        strncpy(buf, test_pairs[i], sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        char* words[4] = {0};
        int wc = 0;
        char* token = strtok(buf, " \t");
        while (token && wc < 4) {
            words[wc++] = token;
            token = strtok(NULL, " \t");
        }
        if (wc < 4) continue;

        // 获取词向量
        float v1[256], v2[256], v3[256];
        if (pretrain_get_embedding(state, words[0], v1) != 0) continue;
        if (pretrain_get_embedding(state, words[1], v2) != 0) continue;
        if (pretrain_get_embedding(state, words[2], v3) != 0) continue;

        // v_result = v1 - v2 + v3
        float v_result[256];
        for (int d = 0; d < dim; d++) {
            v_result[d] = v1[d] - v2[d] + v3[d];
        }

        // 找最相似的词
        char* results[1];
        float scores[1];
        results[0] = (char*)"";

        pretrain_find_similar_by_vec(state, v_result, 1, results, scores);

        if (results[0] && strcmp(results[0], words[3]) == 0) {
            correct++;
        }
    }

    return (float)correct / count;
}

int pretrain_normalize_embeddings(PretrainState* state) {
    if (!state || !state->embedding_layer) return -1;

    float* data = (float*)state->embedding_layer->weights->data;
    int vocab_size = state->embedding_layer->weights->shape[0];
    int dim = state->embedding_layer->weights->shape[1];

    for (int i = 0; i < vocab_size; i++) {
        float* vec = data + i * dim;
        float norm = 0.0f;
        for (int d = 0; d < dim; d++) norm += vec[d] * vec[d];
        norm = sqrtf(norm + 1e-7f);
        for (int d = 0; d < dim; d++) vec[d] /= norm;
    }

    printf("[预训练] 嵌入向量已归一化\n");
    return 0;
}

void pretrain_print_top_words(PretrainState* state, int top_n) {
    if (!state || !state->vocab) return;
    Vocab* vocab = state->vocab;

    printf("========== 最高频词表 (top %d) ==========\n", top_n);
    int print_n = top_n < vocab->size ? top_n : vocab->size;

    for (int i = 0; i < print_n; i++) {
        printf("  [%5d] %s (freq=%d)\n",
               vocab->entries[i].id,
               vocab->entries[i].word,
               vocab->entries[i].freq);
    }
    printf("=========================================\n");
}

// ========== CBOW模型训练 ==========

float pretrain_train_cbow(PretrainState* state, const int* context_ids, int context_count,
                           int center_id, float lr, int neg_count) {
    if (!state || !context_ids || context_count <= 0 || center_id < 0) return 0.0f;

    float* embed_w = (float*)state->embedding_layer->weights->data;
    int embed_dim = state->embedding_layer->weights->shape[1];
    int vocab_size = state->embedding_layer->weights->shape[0];

    if (center_id >= vocab_size) return 0.0f;

    // 步骤1: 计算上下文向量的平均（CBOW的核心）
    float* hidden = (float*)calloc(embed_dim, sizeof(float));
    if (!hidden) return 0.0f;

    float* grad_context = (float*)calloc(embed_dim, sizeof(float));
    if (!grad_context) { free(hidden); return 0.0f; }

    int valid_context = 0;
    for (int c = 0; c < context_count; c++) {
        if (context_ids[c] < 0 || context_ids[c] >= vocab_size) continue;
        float* ctx_vec = embed_w + context_ids[c] * embed_dim;
        for (int d = 0; d < embed_dim; d++) {
            hidden[d] += ctx_vec[d];
        }
        valid_context++;
    }

    if (valid_context == 0) {
        free(hidden);
        free(grad_context);
        return 0.0f;
    }

    // 平均池化
    float inv_count = 1.0f / valid_context;
    for (int d = 0; d < embed_dim; d++) {
        hidden[d] *= inv_count;
    }

    float total_loss = 0.0f;

    // 步骤2: 正样本（中心词）
    float* center_vec = state->context_weights + center_id * embed_dim;
    float score = 0.0f;
    for (int d = 0; d < embed_dim; d++) {
        score += hidden[d] * center_vec[d];
    }
    float sig = sigmoid(score);
    float grad_scale = lr * (1.0f - sig);
    total_loss += -logf(sig + 1e-7f);

    for (int d = 0; d < embed_dim; d++) {
        grad_context[d] += grad_scale * center_vec[d];
        center_vec[d] += grad_scale * hidden[d];
    }

    // 步骤3: 负样本
    int* neg_ids = (int*)malloc(neg_count * sizeof(int));
    if (neg_ids) {
        _sample_negative(state, center_id, neg_ids, neg_count);

        for (int n = 0; n < neg_count; n++) {
            int neg_id = neg_ids[n];
            if (neg_id < 0 || neg_id >= vocab_size) continue;
            float* neg_vec = state->context_weights + neg_id * embed_dim;

            score = 0.0f;
            for (int d = 0; d < embed_dim; d++) {
                score += hidden[d] * neg_vec[d];
            }
            sig = sigmoid(score);
            grad_scale = lr * (0.0f - sig);
            total_loss += -logf(1.0f - sig + 1e-7f);

            for (int d = 0; d < embed_dim; d++) {
                grad_context[d] += grad_scale * neg_vec[d];
                neg_vec[d] += grad_scale * hidden[d];
            }
        }
        free(neg_ids);
    }

    // 步骤4: 将梯度分散到各上下文词
    grad_scale = 1.0f / valid_context;
    for (int c = 0; c < context_count; c++) {
        if (context_ids[c] < 0 || context_ids[c] >= vocab_size) continue;
        float* ctx_vec = embed_w + context_ids[c] * embed_dim;
        for (int d = 0; d < embed_dim; d++) {
            ctx_vec[d] += grad_scale * grad_context[d];
        }
    }

    free(hidden);
    free(grad_context);
    return total_loss;
}

// ========== 批量梯度累积 ==========

void pretrain_flush_batch(PretrainState* state, PretrainConfig* config) {
    if (!state || !state->batch_buffer || state->batch_buffer_count <= 0) return;

    float* embed_w = (float*)state->embedding_layer->weights->data;
    int embed_dim = state->embedding_layer->weights->shape[1];
    int vocab_size = state->embedding_layer->weights->shape[0];
    int count = state->batch_buffer_count;
    float inv_count = 1.0f / count;
    float lr = state->current_lr;

    // 如果使用warmup，调整学习率
    if (state->warmup_steps > 0 && state->global_step < state->warmup_steps) {
        float warmup_factor = (float)state->global_step / (float)state->warmup_steps;
        lr *= warmup_factor;
    }

    // 动量系数
    float mom = (config && config->use_momentum) ? config->momentum : 0.0f;

    // 累积所有词对的梯度
    float* grad_embed = (float*)calloc(vocab_size * embed_dim, sizeof(float));
    float* grad_ctx = (float*)calloc(vocab_size * embed_dim, sizeof(float));
    if (!grad_embed || !grad_ctx) {
        if (grad_embed) free(grad_embed);
        if (grad_ctx) free(grad_ctx);
        state->batch_buffer_count = 0;
        return;
    }

    float batch_loss = 0.0f;

    for (int b = 0; b < count; b++) {
        int center = state->batch_buffer[b].center_id;
        int context = state->batch_buffer[b].context_id;

        if (center < 0 || center >= vocab_size || context < 0 || context >= vocab_size) continue;

        float* center_vec = embed_w + center * embed_dim;
        float* context_vec = state->context_weights + context * embed_dim;

        // 正样本
        float score = 0.0f;
        for (int d = 0; d < embed_dim; d++) {
            score += center_vec[d] * context_vec[d];
        }
        float sig = sigmoid(score);
        float gs = lr * (1.0f - sig);
        batch_loss += -logf(sig + 1e-7f);

        for (int d = 0; d < embed_dim; d++) {
            grad_embed[center * embed_dim + d] += gs * context_vec[d] * inv_count;
            grad_ctx[context * embed_dim + d] += gs * center_vec[d] * inv_count;
        }

        // 负样本
        int neg_count = config ? config->negative_samples : 5;
        int neg_ids_buf[20];
        int actual_neg = neg_count < 20 ? neg_count : 20;
        _sample_negative(state, context, neg_ids_buf, actual_neg);

        for (int n = 0; n < actual_neg; n++) {
            int neg_id = neg_ids_buf[n];
            if (neg_id < 0 || neg_id >= vocab_size) continue;
            float* neg_vec = state->context_weights + neg_id * embed_dim;

            score = 0.0f;
            for (int d = 0; d < embed_dim; d++) {
                score += center_vec[d] * neg_vec[d];
            }
            sig = sigmoid(score);
            gs = lr * (0.0f - sig);
            batch_loss += -logf(1.0f - sig + 1e-7f);

            for (int d = 0; d < embed_dim; d++) {
                grad_embed[center * embed_dim + d] += gs * neg_vec[d] * inv_count;
                grad_ctx[neg_id * embed_dim + d] += gs * center_vec[d] * inv_count;
            }
        }
    }

    // 梯度裁剪
    if (config && config->use_grad_clip && config->grad_clip_value > 0) {
        float clip_val = config->grad_clip_value;
        for (int i = 0; i < vocab_size * embed_dim; i++) {
            if (grad_embed[i] > clip_val) grad_embed[i] = clip_val;
            if (grad_embed[i] < -clip_val) grad_embed[i] = -clip_val;
            if (grad_ctx[i] > clip_val) grad_ctx[i] = clip_val;
            if (grad_ctx[i] < -clip_val) grad_ctx[i] = -clip_val;
        }
    }

    // 动量更新
    if (mom > 0.0f && state->grad_momentum_embed && state->grad_momentum_context) {
        for (int i = 0; i < vocab_size * embed_dim; i++) {
            state->grad_momentum_embed[i] = mom * state->grad_momentum_embed[i] + grad_embed[i];
            state->grad_momentum_context[i] = mom * state->grad_momentum_context[i] + grad_ctx[i];
            embed_w[i] += state->grad_momentum_embed[i];
            state->context_weights[i] += state->grad_momentum_context[i];
        }
    } else {
        for (int i = 0; i < vocab_size * embed_dim; i++) {
            embed_w[i] += grad_embed[i];
            state->context_weights[i] += grad_ctx[i];
        }
    }

    // 更新统计
    state->loss = count > 0 ? batch_loss / count : 0.0f;
    state->global_step++;

    free(grad_embed);
    free(grad_ctx);
    state->batch_buffer_count = 0;
}

// ========== 检查点恢复 ==========

PretrainState* pretrain_state_resume(Vocab* vocab, const char* ckpt_path) {
    if (!vocab || !ckpt_path) return NULL;

    FILE* fp = fopen(ckpt_path, "rb");
    if (!fp) {
        fprintf(stderr, "[预训练] 无法打开检查点: %s\n", ckpt_path);
        return NULL;
    }

    // 读取文件头
    int magic;
    fread(&magic, sizeof(int), 1, fp);
    if (magic != 0x50524554) {
        fprintf(stderr, "[预训练] 无效的检查点格式\n");
        fclose(fp);
        return NULL;
    }

    int vocab_size, embed_dim;
    fread(&vocab_size, sizeof(int), 1, fp);
    fread(&embed_dim, sizeof(int), 1, fp);

    PretrainState* state = pretrain_state_create(vocab, embed_dim);
    if (!state) { fclose(fp); return NULL; }

    // 读取嵌入权重
    fread(state->embedding_layer->weights->data, sizeof(float),
          vocab_size * embed_dim, fp);

    // 读取上下文权重
    if (state->context_weights) {
        fread(state->context_weights, sizeof(float), vocab_size * embed_dim, fp);
    }

    // 读取统计信息
    fread(&state->total_pairs, sizeof(int), 1, fp);
    fread(&state->loss, sizeof(float), 1, fp);

    // 读取扩展检查点数据
    int has_extended = 0;
    if (fread(&has_extended, sizeof(int), 1, fp) == 1 && has_extended == 0x45585431) {
        // 扩展格式: 包含动量和恢复信息
        fread(&state->resume_epoch, sizeof(int), 1, fp);
        fread(&state->resume_line, sizeof(int), 1, fp);
        fread(&state->global_step, sizeof(int), 1, fp);
        fread(&state->current_lr, sizeof(float), 1, fp);
        fread(&state->epoch, sizeof(int), 1, fp);

        // 读取动量缓冲区
        if (state->grad_momentum_embed) {
            fread(state->grad_momentum_embed, sizeof(float), vocab_size * embed_dim, fp);
        }
        if (state->grad_momentum_context) {
            fread(state->grad_momentum_context, sizeof(float), vocab_size * embed_dim, fp);
        }

        printf("[预训练] 从检查点恢复: epoch=%d, line=%d, step=%d\n",
               state->epoch, state->resume_line, state->global_step);
    } else {
        // 旧格式检查点，无恢复信息
        state->resume_epoch = 0;
        state->resume_line = 0;
    }

    state->train_start_time = clock();
    fclose(fp);

    printf("[预训练] 检查点已加载: %s (vocab=%d, dim=%d, pairs=%d)\n",
           ckpt_path, vocab_size, embed_dim, state->total_pairs);
    return state;
}

// ========== 保存扩展检查点 ==========

static int _save_checkpoint_extended(PretrainState* state, const char* filepath,
                                      int current_line) {
    if (!state || !state->embedding_layer) return -1;

    FILE* fp = fopen(filepath, "wb");
    if (!fp) return -1;

    Tensor* w = state->embedding_layer->weights;
    int vocab_size = w->shape[0];
    int embed_dim = w->shape[1];

    // 文件头
    int magic = 0x50524554;
    fwrite(&magic, sizeof(int), 1, fp);
    fwrite(&vocab_size, sizeof(int), 1, fp);
    fwrite(&embed_dim, sizeof(int), 1, fp);

    // 写入权重
    fwrite(w->data, sizeof(float), vocab_size * embed_dim, fp);
    fwrite(state->context_weights, sizeof(float), vocab_size * embed_dim, fp);

    // 写入统计
    fwrite(&state->total_pairs, sizeof(int), 1, fp);
    fwrite(&state->loss, sizeof(float), 1, fp);

    // 扩展标记
    int ext_marker = 0x45585431; // "EXT1"
    fwrite(&ext_marker, sizeof(int), 1, fp);

    // 恢复信息
    fwrite(&state->epoch, sizeof(int), 1, fp);
    fwrite(&current_line, sizeof(int), 1, fp);
    fwrite(&state->global_step, sizeof(int), 1, fp);
    fwrite(&state->current_lr, sizeof(float), 1, fp);
    fwrite(&state->epoch, sizeof(int), 1, fp);

    // 动量缓冲区
    if (state->grad_momentum_embed) {
        fwrite(state->grad_momentum_embed, sizeof(float), vocab_size * embed_dim, fp);
    }
    if (state->grad_momentum_context) {
        fwrite(state->grad_momentum_context, sizeof(float), vocab_size * embed_dim, fp);
    }

    fclose(fp);
    printf("[预训练] 扩展检查点已保存: %s\n", filepath);
    return 0;
}

// ========== 短语检测 ==========

int pretrain_detect_phrases(Vocab* vocab, const char** texts, int text_count,
                            int min_count, float pmi_threshold,
                            char** out_phrases, int max_phrases) {
    if (!vocab || !texts || !out_phrases) return 0;

    // 统计二元组频率
    typedef struct {
        int word1_id;
        int word2_id;
        int count;
    } BigramEntry;

    int bigram_cap = 10000;
    int bigram_count = 0;
    BigramEntry* bigrams = (BigramEntry*)malloc(bigram_cap * sizeof(BigramEntry));
    if (!bigrams) return 0;

    // 统计总词频（用于PMI计算）
    double total_words_count = 0.0;
    for (int i = 0; i < vocab->size; i++) {
        total_words_count += vocab->entries[i].freq;
    }

    // 扫描文本统计共现
    for (int t = 0; t < text_count; t++) {
        int ids[1024];
        int len = vocab_encode(vocab, texts[t], ids, 1024);

        for (int i = 0; i < len - 1; i++) {
            if (ids[i] < 5 || ids[i + 1] < 5) continue;

            // 查找已有二元组
            int found = -1;
            for (int b = 0; b < bigram_count; b++) {
                if (bigrams[b].word1_id == ids[i] && bigrams[b].word2_id == ids[i + 1]) {
                    found = b;
                    break;
                }
            }

            if (found >= 0) {
                bigrams[found].count++;
            } else if (bigram_count < bigram_cap) {
                bigrams[bigram_count].word1_id = ids[i];
                bigrams[bigram_count].word2_id = ids[i + 1];
                bigrams[bigram_count].count = 1;
                bigram_count++;
            }
        }
    }

    // 计算PMI并筛选短语
    int phrase_count = 0;
    for (int b = 0; b < bigram_count && phrase_count < max_phrases; b++) {
        if (bigrams[b].count < min_count) continue;

        int w1 = bigrams[b].word1_id;
        int w2 = bigrams[b].word2_id;
        float freq_w1 = (float)vocab->entries[w1].freq / total_words_count;
        float freq_w2 = (float)vocab->entries[w2].freq / total_words_count;
        float freq_pair = (float)bigrams[b].count / total_words_count;

        // PMI = log(P(x,y) / (P(x) * P(y)))
        float pmi = logf(freq_pair / (freq_w1 * freq_w2 + 1e-10f) + 1e-10f);

        if (pmi > pmi_threshold) {
            // 组合短语
            char phrase[256];
            snprintf(phrase, sizeof(phrase), "%s_%s",
                    vocab->entries[w1].word, vocab->entries[w2].word);
            out_phrases[phrase_count] = strdup(phrase);
            phrase_count++;
        }
    }

    free(bigrams);
    printf("[预训练] 检测到 %d 个短语\n", phrase_count);
    return phrase_count;
}

// ========== 词汇增量扩展 ==========

int pretrain_expand_vocab(PretrainState* state, Vocab* new_vocab, int init_strategy) {
    if (!state || !new_vocab || !state->embedding_layer) return -1;

    Tensor* w = state->embedding_layer->weights;
    int old_vocab_size = w->shape[0];
    int embed_dim = w->shape[1];

    // 计算新增词数
    int new_words = new_vocab->size - old_vocab_size;
    if (new_words <= 0) return 0;  // 没有新词

    // 扩展嵌入矩阵
    float* new_embed = (float*)realloc(w->data,
                                        new_vocab->size * embed_dim * sizeof(float));
    if (!new_embed) return -1;
    w->data = new_embed;

    // 扩展上下文权重
    float* new_ctx = (float*)realloc(state->context_weights,
                                      new_vocab->size * embed_dim * sizeof(float));
    if (!new_ctx) return -1;
    state->context_weights = new_ctx;

    // 扩展动量缓冲区
    if (state->grad_momentum_embed) {
        float* new_mom_e = (float*)realloc(state->grad_momentum_embed,
                                            new_vocab->size * embed_dim * sizeof(float));
        if (new_mom_e) state->grad_momentum_embed = new_mom_e;
    }
    if (state->grad_momentum_context) {
        float* new_mom_c = (float*)realloc(state->grad_momentum_context,
                                            new_vocab->size * embed_dim * sizeof(float));
        if (new_mom_c) state->grad_momentum_context = new_mom_c;
    }

    // 初始化新词向量
    float scale = sqrtf(2.0f / (new_vocab->size + embed_dim));

    if (init_strategy == 1 && state->vocab && old_vocab_size > 0) {
        // 策略1: 使用相近词的平均值初始化
        for (int i = old_vocab_size; i < new_vocab->size; i++) {
            float* new_vec = (float*)w->data + i * embed_dim;
            float* new_ctx_vec = state->context_weights + i * embed_dim;

            for (int d = 0; d < embed_dim; d++) {
                new_vec[d] = 0.0f;
                new_ctx_vec[d] = 0.0f;
            }

            // 简单策略：取已有向量的平均值加随机扰动
            for (int d = 0; d < embed_dim; d++) {
                float sum = 0.0f;
                for (int r = 0; r < old_vocab_size && r < 100; r++) {
                    sum += ((float*)w->data)[r * embed_dim + d];
                }
                new_vec[d] = sum / (old_vocab_size < 100 ? old_vocab_size : 100);
                new_vec[d] += ((float)rand() / RAND_MAX * 2.0f - 1.0f) * scale * 0.1f;
                new_ctx_vec[d] = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * scale;
            }
        }
    } else {
        // 策略0: 随机初始化
        for (int i = old_vocab_size * embed_dim; i < new_vocab->size * embed_dim; i++) {
            ((float*)w->data)[i] = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * scale;
            state->context_weights[i] = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * scale;
        }
    }

    // 初始化新动量缓冲区为0
    if (state->grad_momentum_embed) {
        for (int i = old_vocab_size * embed_dim; i < new_vocab->size * embed_dim; i++) {
            state->grad_momentum_embed[i] = 0.0f;
        }
    }
    if (state->grad_momentum_context) {
        for (int i = old_vocab_size * embed_dim; i < new_vocab->size * embed_dim; i++) {
            state->grad_momentum_context[i] = 0.0f;
        }
    }

    // 更新维度信息
    w->shape[0] = new_vocab->size;
    state->vocab = new_vocab;

    // 重建unigram表
    if (state->unigram_table) {
        free(state->unigram_table);
        state->unigram_table = NULL;
    }
    if (state->unigram_cumsum) {
        free(state->unigram_cumsum);
        state->unigram_cumsum = NULL;
    }
    _build_unigram_table(state);

    printf("[预训练] 词表扩展: %d -> %d (+%d词)\n",
           old_vocab_size, new_vocab->size, new_words);
    return 0;
}

// ========== 嵌入质量评估 ==========

float pretrain_evaluate_quality(PretrainState* state, const char** test_texts,
                                 int count, float* out_coherence) {
    if (!state || !test_texts || count <= 0) return 0.0f;

    float* embed_data = (float*)state->embedding_layer->weights->data;
    int dim = state->embedding_layer->weights->shape[1];
    int vocab_size = state->embedding_layer->weights->shape[0];
    float total_score = 0.0f;

    for (int t = 0; t < count; t++) {
        int ids[1024];
        int len = vocab_encode(state->vocab, test_texts[t], ids, 1024);
        if (len < 2) continue;

        // 计算文本相干性: 相邻词的平均相似度
        float coherence = 0.0f;
        int pair_count = 0;

        for (int i = 0; i < len - 1; i++) {
            if (ids[i] < 0 || ids[i] >= vocab_size) continue;
            if (ids[i + 1] < 0 || ids[i + 1] >= vocab_size) continue;

            float* v1 = embed_data + ids[i] * dim;
            float* v2 = embed_data + ids[i + 1] * dim;
            coherence += pretrain_cosine_similarity(v1, v2, dim);
            pair_count++;
        }

        if (pair_count > 0) {
            coherence /= pair_count;
        }

        if (out_coherence) {
            out_coherence[t] = coherence;
        }

        total_score += coherence;
    }

    float avg_score = total_score / count;

    // 计算嵌入空间均匀性（理想情况下向量应均匀分布）
    float uniformity = 0.0f;
    int sample_count = vocab_size < 100 ? vocab_size : 100;
    for (int i = 5; i < sample_count; i++) {
        float* vi = embed_data + i * dim;
        for (int j = i + 1; j < sample_count; j++) {
            float* vj = embed_data + j * dim;
            float sim = pretrain_cosine_similarity(vi, vj, dim);
            uniformity += sim * sim;  // 低值表示更好的均匀性
        }
    }
    int pairs = sample_count * (sample_count - 1) / 2;
    if (pairs > 0) uniformity /= pairs;

    // 综合分数: 高相干性 + 低均匀性
    float quality = avg_score * 0.7f + (1.0f - fminf(uniformity, 1.0f)) * 0.3f;
    return fmaxf(0.0f, fminf(quality, 1.0f));
}

// ========== 训练进度 ==========

float pretrain_get_progress(PretrainState* state, int total_lines) {
    if (!state || total_lines <= 0) return 0.0f;

    // 基于已训练词对估算进度
    float epoch_progress = (float)state->trained_words / (total_lines * state->vocab->size * 2);
    float total_progress = ((float)state->epoch + epoch_progress);

    return fminf(total_progress * 100.0f, 100.0f);
}
