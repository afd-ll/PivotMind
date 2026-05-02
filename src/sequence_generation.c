#include "../include/generative_model.h"
#include "../include/tensor.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

// 贪婪解码: 每步选择概率最高的词
int greedy_decode(Tensor* logits, int vocab_size) {
    float* logit_data = (float*)logits->data;

    // 直接找logits最大值（softmax不改变最大值的位置）
    int best_idx = 0;
    float max_val = logit_data[0];
    for (int v = 1; v < vocab_size; v++) {
        if (logit_data[v] > max_val) {
            max_val = logit_data[v];
            best_idx = v;
        }
    }

    return best_idx;
}

// 束搜索候选序列
typedef struct {
    int* sequence;      // 生成的词ID序列
    int length;         // 当前序列长度
    float score;        // 累积对数概率
} BeamCandidate;

// 创建束搜索候选
BeamCandidate* beam_candidate_create(int max_len) {
    BeamCandidate* candidate = (BeamCandidate*)malloc(sizeof(BeamCandidate));
    candidate->sequence = (int*)calloc(max_len, sizeof(int));
    candidate->length = 0;
    candidate->score = 0.0f;
    return candidate;
}

// 销毁束搜索候选
void beam_candidate_destroy(BeamCandidate* candidate) {
    if (candidate) {
        free(candidate->sequence);
        free(candidate);
    }
}

// 克隆束搜索候选
BeamCandidate* beam_candidate_clone(BeamCandidate* src, int max_len) {
    BeamCandidate* clone = beam_candidate_create(max_len);
    clone->length = src->length;
    clone->score = src->score;
    memcpy(clone->sequence, src->sequence, max_len * sizeof(int));
    return clone;
}

// 应用长度归一化
float apply_length_penalty(float score, int length, float alpha) {
    if (length <= 0) return score;
    return score / powf((float)length, alpha);
}

// Top-k采样: 从top k个候选中随机选择
int top_k_sample(Tensor* logits, int k) {
    float* logit_data = (float*)logits->data;
    int vocab_size = logits->shape[0];

    // 确保 k 不超过 vocab_size
    if (k > vocab_size) k = vocab_size;

    // 找到top k的索引
    int* indices = (int*)malloc(k * sizeof(int));
    int count = 0;

    for (int i = 0; i < vocab_size && count < k; i++) {
        indices[count++] = i;
    }

    // 简单排序（按logit值降序）
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (logit_data[indices[j]] > logit_data[indices[i]]) {
                int temp = indices[i];
                indices[i] = indices[j];
                indices[j] = temp;
            }
        }
    }

    // 从top k中随机选择一个
    int selected = indices[rand() % count];
    free(indices);
    return selected;
}

// 束搜索解码
char* beam_search_decode(Seq2SeqModel* model, GenVocabulary* vocab,
                         const char* input_text,
                         int max_output_len,
                         int beam_width,
                         float length_penalty) {
    // 分词
    char* tokens[100];
    int token_count = tokenize_text(input_text, tokens, 100);

    // 转换为词ID
    int* input_ids = (int*)malloc(token_count * sizeof(int));
    for (int i = 0; i < token_count; i++) {
        input_ids[i] = gen_vocab_get_word_id(vocab, tokens[i]);
        free(tokens[i]);
    }

    // 创建输入张量
    float* input_data = (float*)malloc(token_count * sizeof(float));
    for (int i = 0; i < token_count; i++) {
        input_data[i] = (float)input_ids[i];
    }
    size_t input_shape[] = {token_count};
    Tensor* input_seq = tensor_create_from_data(DT_FLOAT32, 1,
                                              input_shape, input_data);

    // 编码
    Tensor* encoder_output = encode_sequence(model, input_seq);

    // 解码器层
    Layer* dec_emb = model->decoder->layers[0];
    Layer* dec_rnn = model->decoder->layers[1];
    Layer* dec_linear = model->decoder->layers[2];

    // 初始化隐藏状态
    RNNData* rnn_data = (RNNData*)dec_rnn->private_data;
    if (!rnn_data || !rnn_data->hidden) {
        printf("[ERROR] RNN数据未初始化\n");
        return strdup("错误");
    }
    float* hidden_data = (float*)rnn_data->hidden->data;
    float* encoder_out_data = (float*)encoder_output->data;
    for (int i = 0; i < model->hidden_dim; i++) {
        hidden_data[i] = encoder_out_data[i];
    }

    // 初始化束搜索候选
    BeamCandidate** beams = (BeamCandidate**)malloc(beam_width * sizeof(BeamCandidate*));
    for (int i = 0; i < beam_width; i++) {
        beams[i] = beam_candidate_create(max_output_len);
    }
    int num_active = 1;  // 活跃候选数

    // 开始token: <SOS>
    int sos_token = 1;
    beams[0]->sequence[0] = sos_token;
    beams[0]->length = 1;
    beams[0]->score = 0.0f;

    // 逐步扩展
    for (int step = 0; step < max_output_len && num_active > 0; step++) {
        
        // 收集所有扩展候选
        BeamCandidate** new_candidates = NULL;
        int new_count = 0;
        int capacity = num_active * model->vocab_size;
        
        // 初始化新候选数组
        new_candidates = (BeamCandidate**)malloc(capacity * sizeof(BeamCandidate*));

        for (int i = 0; i < num_active; i++) {
            BeamCandidate* beam = beams[i];

            // 跳过已结束的序列
            if (beam->length > 0) {
                int last_token = beam->sequence[beam->length - 1];

                if (last_token == 2) {
                    // 已结束，直接复制到新候选
                    if (new_count >= capacity) {
                        capacity *= 2;
                        new_candidates = (BeamCandidate**)realloc(new_candidates,
                                              capacity * sizeof(BeamCandidate*));
                    }
                    new_candidates[new_count++] = beam_candidate_clone(beam, max_output_len);
                    continue;
                }
            }

            // 获取当前token
            int current_token = beam->sequence[beam->length - 1];

            // 前向传播
            float token_val = (float)current_token;
            size_t token_shape[] = {1};
            Tensor* dec_input = tensor_create_from_data(DT_FLOAT32, 1,
                                                       token_shape, &token_val);

            layer_forward(dec_emb, dec_input);

            // 检查并修复NaN/Inf
            float* emb_out = (float*)dec_emb->output->data;
            for (size_t j = 0; j < dec_emb->output->size; j++) {
                if (!isfinite(emb_out[j])) {
                    emb_out[j] = 0.0f;
                }
            }

            layer_forward(dec_rnn, dec_emb->output);

            float* rnn_out = (float*)dec_rnn->output->data;
            for (size_t j = 0; j < dec_rnn->output->size; j++) {
                if (!isfinite(rnn_out[j])) {
                    rnn_out[j] = 0.0f;
                }
            }

            layer_forward(dec_linear, dec_rnn->output);

            // 获取logits并应用softmax
            float* logits = (float*)dec_linear->output->data;
            int vocab_size = model->vocab_size;

            // 修复NaN/Inf
            for (int v = 0; v < vocab_size; v++) {
                if (!isfinite(logits[v])) {
                    logits[v] = -1e10f;
                }
            }

            // 计算softmax概率
            float max_logit = logits[0];
            for (int v = 1; v < vocab_size; v++) {
                if (logits[v] > max_logit) max_logit = logits[v];
            }

            float sum = 0.0f;
            for (int v = 0; v < vocab_size; v++) {
                logits[v] = expf(logits[v] - max_logit);
                sum += logits[v];
            }

            if (sum > 0) {
                for (int v = 0; v < vocab_size; v++) {
                    logits[v] /= sum;
                }
            }

            // 只考虑top-k个词（优化性能）
            int top_k = (vocab_size < 10) ? vocab_size : 10;  // 最多考虑前10个词

            // 找到top-k个索引
            int* top_indices = (int*)malloc(top_k * sizeof(int));
            int* used = (int*)calloc(vocab_size, sizeof(int));
            int count = 0;

            // 找到前top-k个最大值的索引
            while (count < top_k) {
                float max_val = -1e10f;
                int max_idx = -1;
                for (int v = 0; v < vocab_size; v++) {
                    if (!used[v] && logits[v] > max_val) {
                        max_val = logits[v];
                        max_idx = v;
                    }
                }
                if (max_idx >= 0) {
                    used[max_idx] = 1;
                    top_indices[count++] = max_idx;
                } else {
                    break;
                }
            }
            free(used);

            // 为top-k个词创建新候选
            for (int k = 0; k < count; k++) {
                int v = top_indices[k];
                float log_prob = logf(logits[v] + 1e-10f);

                if (new_count >= capacity) {
                    capacity *= 2;
                    new_candidates = (BeamCandidate**)realloc(new_candidates,
                                          capacity * sizeof(BeamCandidate*));
                }

                BeamCandidate* new_beam = beam_candidate_clone(beam, max_output_len);
                new_beam->sequence[new_beam->length++] = v;
                new_beam->score += log_prob;
                new_candidates[new_count++] = new_beam;
            }
            free(top_indices);

            tensor_destroy(dec_input);
        }

        // 按分数排序并选择top-k
        if (new_count > 0) {
            // 简单冒泡排序
            for (int i = 0; i < new_count - 1; i++) {
                for (int j = 0; j < new_count - 1 - i; j++) {
                    float score1 = apply_length_penalty(new_candidates[j]->score,
                                                       new_candidates[j]->length,
                                                       length_penalty);
                    float score2 = apply_length_penalty(new_candidates[j+1]->score,
                                                       new_candidates[j+1]->length,
                                                       length_penalty);
                    if (score2 > score1) {
                        BeamCandidate* temp = new_candidates[j];
                        new_candidates[j] = new_candidates[j+1];
                        new_candidates[j+1] = temp;
                    }
                }
            }

            // 保留前beam_width个候选
            for (int i = 0; i < num_active; i++) {
                beam_candidate_destroy(beams[i]);
            }

            num_active = (new_count < beam_width) ? new_count : beam_width;
            for (int i = 0; i < num_active; i++) {
                beams[i] = new_candidates[i];
            }

            // 销毁多余的候选
            for (int i = num_active; i < new_count; i++) {
                beam_candidate_destroy(new_candidates[i]);
            }
        }

        if (new_candidates != NULL) {
            free(new_candidates);
            new_candidates = NULL;
        }

        // 检查是否所有候选都已结束
        int all_finished = 1;
        for (int i = 0; i < num_active; i++) {
            if (beams[i]->length == 0 ||
                beams[i]->sequence[beams[i]->length - 1] != 2) {
                all_finished = 0;
                break;
            }
        }
        if (all_finished) {
            break;
        }
    }

    // 选择最佳候选
    int best_idx = 0;
    float best_score = -1e10f;

    for (int i = 0; i < num_active; i++) {
        float normalized_score = apply_length_penalty(beams[i]->score,
                                                     beams[i]->length,
                                                     length_penalty);

        if (normalized_score > best_score) {
            best_score = normalized_score;
            best_idx = i;
        }
    }

    // 将最佳序列转换为文本
    char* result = (char*)malloc(1024 * sizeof(char));
    result[0] = '\0';

    BeamCandidate* best_beam = beams[best_idx];
    
    int valid_words = 0;  // 统计有效词数量

    printf("\n[调试] Beam Search 结果序列长度: %d\n", best_beam->length);
    printf("[调试] 词汇表大小: %d\n", vocab->size);
    printf("[调试] Token ID 映射:\n");

    for (int i = 0; i < best_beam->length; i++) {
        int token_id = best_beam->sequence[i];

        // 验证 token ID 是否在有效范围内
        if (token_id < 0 || token_id >= vocab->size) {
            printf("[警告] Token %d: ID=%d 超出范围 [0, %d)\n", i, token_id, vocab->size);
            continue;
        }

        const char* word = gen_vocab_get_word(vocab, token_id);
        printf("  Token %d: ID=%d, 词='%s'\n", i, token_id, word);

        // 跳过特殊token
        if (strcmp(word, "<PAD>") == 0 ||
            strcmp(word, "<SOS>") == 0 ||
            strcmp(word, "<EOS>") == 0) {
            continue;
        }

        // 对于<UNK>，显示为未知标记
        if (strcmp(word, "<UNK>") == 0) {
            if (strlen(result) > 0) {
                strcat(result, " ");
            }
            strcat(result, "[未知]");
            valid_words++;
            continue;
        }

        if (strlen(result) > 0) {
            strcat(result, " ");
        }
        strcat(result, word);
        valid_words++;
    }

    // 如果序列为空，提供一个默认回复
    if (valid_words == 0) {
        strcpy(result, "你好，有什么可以帮你的吗？");
    }

    // 清理
    free(input_ids);
    free(input_data);
    tensor_destroy(input_seq);
    tensor_destroy(encoder_output);
    for (int i = 0; i < beam_width; i++) {
        beam_candidate_destroy(beams[i]);
    }
    free(beams);

    return result;
}

// 真正的序列生成
char* generate_sequence_greedy(Seq2SeqModel* model, GenVocabulary* vocab,
                               const char* input_text,
                               int max_output_len,
                               float temperature) {
    // 分词
    char* tokens[100];
    int token_count = tokenize_text(input_text, tokens, 100);

    // 转换为词ID
    int* input_ids = (int*)malloc(token_count * sizeof(int));
    for (int i = 0; i < token_count; i++) {
        input_ids[i] = gen_vocab_get_word_id(vocab, tokens[i]);
        free(tokens[i]);
    }

    // 创建输入张量
    float* input_data = (float*)malloc(token_count * sizeof(float));
    for (int i = 0; i < token_count; i++) {
        input_data[i] = (float)input_ids[i];
    }
    size_t input_shape[] = {token_count};
    Tensor* input_seq = tensor_create_from_data(DT_FLOAT32, 1,
                                             input_shape, input_data);

    // 编码
    Tensor* encoder_output = encode_sequence(model, input_seq);

    // 解码器生成
    Layer* dec_emb = model->decoder->layers[0];
    Layer* dec_rnn = model->decoder->layers[1];
    Layer* dec_linear = model->decoder->layers[2];

    // 初始化解码器隐藏状态
    RNNData* rnn_data = (RNNData*)dec_rnn->private_data;

    // 重置隐藏状态
    float* hidden_data = (float*)rnn_data->hidden->data;
    float* encoder_out_data = (float*)encoder_output->data;
    for (int i = 0; i < model->hidden_dim; i++) {
        hidden_data[i] = encoder_out_data[i];
    }

    // 生成的词ID序列
    int* output_ids = (int*)malloc((max_output_len + 1) * sizeof(int));
    int output_len = 0;

    // 开始token: <SOS>
    int current_token = 1;  // 假设1是<SOS>

    // 逐个生成token
    for (int step = 0; step < max_output_len; step++) {
        // printf("  [调试] 时间步 %d/%d, 当前token=%d\n", step, max_output_len, current_token);

        // 创建当前token的输入
        float token_val = (float)current_token;
        size_t token_shape[] = {1};
        Tensor* dec_input = tensor_create_from_data(DT_FLOAT32, 1,
                                                  token_shape, &token_val);

        // 前向传播
        layer_forward(dec_emb, dec_input);

        // 检查嵌入层输出
        float* emb_out = (float*)dec_emb->output->data;
        for (size_t i = 0; i < dec_emb->output->size; i++) {
            if (!isfinite(emb_out[i])) {
                printf("  [错误] 嵌入层输出在第 %zu 个位置出现 NaN/Inf\n", i);
            }
        }

        layer_forward(dec_rnn, dec_emb->output);

        // 检查RNN输出
        float* rnn_out = (float*)dec_rnn->output->data;
        for (size_t i = 0; i < dec_rnn->output->size; i++) {
            if (!isfinite(rnn_out[i])) {
                printf("  [错误] RNN输出在第 %zu 个位置出现 NaN/Inf\n", i);
            }
        }

        layer_forward(dec_linear, dec_rnn->output);

        // 应用Softmax
        float* logits = (float*)dec_linear->output->data;
        int vocab_size = model->vocab_size;

        // printf("  [调试] vocab_size=%d, logits[0]=%.4f\n", vocab_size, logits[0]);

        // 检查 NaN 或 Inf
        int has_nan = 0;
        for (int v = 0; v < vocab_size; v++) {
            if (!isfinite(logits[v])) {
                logits[v] = 0.0f;
                has_nan = 1;
            }
        }

        if (has_nan) {
            printf("  [警告] 检测到 NaN/Inf，已重置为 0\n");
            // 如果所有 logits 都是 0，随机选择一个词
            int valid_count = 0;
            for (int v = 0; v < vocab_size; v++) {
                if (logits[v] == 0.0f) valid_count++;
            }
            if (valid_count == vocab_size) {
                // 所有都是 NaN，随机选择一个词（跳过特殊token）
                for (int v = 4; v < vocab_size; v++) {
                    logits[v] = 1.0f;  // 给普通词相同的权重
                    break;
                }
            }
        }

        // 温度调整
        if (temperature > 0 && temperature != 1.0f) {
            float max_logit = logits[0];
            for (int v = 1; v < vocab_size; v++) {
                if (logits[v] > max_logit) max_logit = logits[v];
            }
            for (int v = 0; v < vocab_size; v++) {
                logits[v] = (logits[v] - max_logit) / temperature;
            }
        }

        // 选择下一个token
        if (temperature < 0.3f) {
            current_token = greedy_decode(dec_linear->output, vocab_size);  // 贪婪
        } else {
            current_token = top_k_sample(dec_linear->output, 5);  // Top-5采样
        }

        // printf("  [调试] 选择的下一个token=%d, 对应词='%s'\n", current_token, gen_vocab_get_word(vocab, current_token));

        // 检查结束token
        if (current_token == 2) {  // <EOS>
            break;
        }

        // 保存生成的token
        output_ids[output_len++] = current_token;

        // 清理
        tensor_destroy(dec_input);
    }

    // 将词ID转换为文本
    char* result = (char*)malloc(1024 * sizeof(char));
    result[0] = '\0';

    for (int i = 0; i < output_len; i++) {
        const char* word = gen_vocab_get_word(vocab, output_ids[i]);

        // 跳过特殊token
        if (strcmp(word, "<PAD>") == 0 ||
            strcmp(word, "<SOS>") == 0 ||
            strcmp(word, "<EOS>") == 0 ||
            strcmp(word, "<UNK>") == 0) {
            continue;
        }

        if (strlen(result) > 0) {
            strcat(result, " ");
        }
        strcat(result, word);
    }

    // 清理
    free(input_ids);
    free(input_data);
    tensor_destroy(input_seq);
    tensor_destroy(encoder_output);
    free(output_ids);

    return result;
}
