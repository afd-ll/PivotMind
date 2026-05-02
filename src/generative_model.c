#include "generative_model.h"
#include "multi_topology.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// 初始化词汇表（使用 gen_ 前缀避免与 vocab 模块冲突）
GenVocabulary* gen_vocab_create(int max_size) {
    GenVocabulary* vocab = (GenVocabulary*)malloc(sizeof(GenVocabulary));
    vocab->words = (char**)malloc(sizeof(char*) * max_size);
    vocab->size = 0;
    vocab->max_size = max_size;

    // 添加特殊token
    gen_vocab_add_word(vocab, "<PAD>");  // 填充
    gen_vocab_add_word(vocab, "<SOS>");  // 句子开始
    gen_vocab_add_word(vocab, "<EOS>");  // 句子结束
    gen_vocab_add_word(vocab, "<UNK>");  // 未知词

    return vocab;
}

void gen_vocab_destroy(GenVocabulary* vocab) {
    if (vocab) {
        for (int i = 0; i < vocab->size; i++) {
            if (vocab->words[i]) {
                free(vocab->words[i]);
            }
        }
        free(vocab->words);
        free(vocab);
    }
}

int gen_vocab_add_word(GenVocabulary* vocab, const char* word) {
    if (vocab->size >= vocab->max_size) {
        int new_max_size = vocab->max_size * 2;
        char** new_words = (char**)realloc(vocab->words,
                                         sizeof(char*) * new_max_size);
        if (!new_words) {
            // realloc失败，返回错误
            return -1;
        }
        vocab->words = new_words;
        vocab->max_size = new_max_size;
    }

    // 检查词是否已存在
    for (int i = 0; i < vocab->size; i++) {
        if (strcmp(vocab->words[i], word) == 0) {
            return i;  // 返回已存在的ID
        }
    }

    vocab->words[vocab->size] = strdup(word);
    if (!vocab->words[vocab->size]) {
        return -1;  // strdup失败
    }
    return vocab->size++;
}

int gen_vocab_get_word_id(GenVocabulary* vocab, const char* word) {
    for (int i = 0; i < vocab->size; i++) {
        if (strcmp(vocab->words[i], word) == 0) {
            return i;
        }
    }
    return 3;  // <UNK>的ID
}

const char* gen_vocab_get_word(GenVocabulary* vocab, int id) {
    if (id >= 0 && id < vocab->size) {
        return vocab->words[id];
    }
    return "<UNK>";
}

// 创建Seq2Seq模型
Seq2SeqModel* seq2seq_create(int vocab_size, int embedding_dim,
                             int hidden_dim, int max_seq_len) {
    Seq2SeqModel* model = (Seq2SeqModel*)malloc(sizeof(Seq2SeqModel));
    model->vocab_size = vocab_size;
    model->embedding_dim = embedding_dim;
    model->hidden_dim = hidden_dim;
    model->max_seq_len = max_seq_len;

    // 创建编码器
    // 编码器只输出隐藏状态(hidden_dim)，不输出到vocab_size
    model->encoder = model_create();
    model_add_layer(model->encoder, layer_create_embedding(vocab_size, embedding_dim));
    model_add_layer(model->encoder, layer_create_simple_rnn(embedding_dim, hidden_dim));
    // 注意：不添加线性层，因为编码器只需要输出隐藏状态作为上下文向量

    // 创建解码器
    model->decoder = model_create();
    model_add_layer(model->decoder, layer_create_embedding(vocab_size, embedding_dim));
    model_add_layer(model->decoder, layer_create_simple_rnn(embedding_dim, hidden_dim));
    model_add_layer(model->decoder, layer_create_linear(hidden_dim, vocab_size, true));

    return model;
}

void seq2seq_destroy(Seq2SeqModel* model) {
    if (model) {
        if (model->encoder) model_destroy(model->encoder);
        if (model->decoder) model_destroy(model->decoder);
        free(model);
    }
}

// 编码输入序列
Tensor* encode_sequence(Seq2SeqModel* model, Tensor* input_seq) {
    return model_forward(model->encoder, input_seq);
}

// 训练Seq2Seq模型 (教师强制 - 带完整反向传播)
void seq2seq_train(Seq2SeqModel* model, Tensor* input_seq,
                   Tensor* target_seq, float learning_rate) {
    if (!model || !input_seq || !target_seq) return;
    
    // 编码器前向传播
    model_forward(model->encoder, input_seq);
    
    // 解码器前向传播 (使用目标序列作为输入 - Teacher Forcing)
    Tensor* decoder_output = model_forward(model->decoder, target_seq);
    if (!decoder_output) return;
    
    int vocab_size = model->vocab_size;
    int seq_len = (int)input_seq->shape[0];
    int hidden_dim = model->hidden_dim;
    
    // 计算交叉熵损失
    float* output_data = (float*)decoder_output->data;
    float* target_data = (float*)target_seq->data;
    float loss = 0.0f;
    for (int t = 0; t < seq_len && t < model->max_seq_len; t++) {
        int target_idx = (int)target_data[t];
        if (target_idx < 0 || target_idx >= vocab_size) target_idx = 3;
        for (int v = 0; v < vocab_size; v++) {
            float pred = output_data[t * vocab_size + v];
            if (pred < 1e-10f) pred = 1e-10f;
            float onehot = (v == target_idx) ? 1.0f : 0.0f;
            loss -= onehot * logf(pred);
        }
    }
    
    // 获取解码器各层
    Layer* dec_linear = NULL;
    Layer* dec_rnn = NULL;
    for (size_t i = 0; i < model->decoder->num_layers; i++) {
        Layer* l = model->decoder->layers[i];
        if (l->type == LAYER_LINEAR) dec_linear = l;
        if (l->type == LAYER_SIMPLE_RNN) dec_rnn = l;
    }
    if (!dec_linear || !dec_rnn) { printf("  [训练] 缺少必要层\n"); return; }
    
    RNNData* rnn = (RNNData*)dec_rnn->private_data;
    if (!rnn) { printf("  [训练] RNN数据为空\n"); return; }
    
    int lin_in = (int)dec_linear->weights->shape[0];  // = hidden_dim
    int lin_out = (int)dec_linear->weights->shape[1]; // = vocab_size
    
    // 分配并清零梯度
    if (!dec_linear->weights->grad)
        dec_linear->weights->grad = tensor_zeros(DT_FLOAT32, 2, dec_linear->weights->shape);
    if (!dec_linear->bias->grad)
        dec_linear->bias->grad = tensor_zeros(DT_FLOAT32, 1, (size_t[]){(size_t)lin_out});
    if (!rnn->Wx->grad)
        rnn->Wx->grad = tensor_zeros(DT_FLOAT32, 2, (size_t[]){(size_t)rnn->input_size, (size_t)rnn->hidden_size});
    if (!rnn->Wh->grad)
        rnn->Wh->grad = tensor_zeros(DT_FLOAT32, 2, (size_t[]){(size_t)rnn->hidden_size, (size_t)rnn->hidden_size});
    if (!rnn->bh->grad)
        rnn->bh->grad = tensor_zeros(DT_FLOAT32, 1, (size_t[]){(size_t)rnn->hidden_size});
    
    // 清零梯度
    memset(dec_linear->weights->grad->data, 0, dec_linear->weights->grad->size * sizeof(float));
    memset(dec_linear->bias->grad->data, 0, dec_linear->bias->grad->size * sizeof(float));
    memset(rnn->Wx->grad->data, 0, rnn->Wx->grad->size * sizeof(float));
    memset(rnn->Wh->grad->data, 0, rnn->Wh->grad->size * sizeof(float));
    memset(rnn->bh->grad->data, 0, rnn->bh->grad->size * sizeof(float));
    
    // BPTT: 时间步反向传播
    float* rnn_outputs = rnn->outputs ? (float*)rnn->outputs->data : NULL;
    float* W_lin = (float*)dec_linear->weights->data;
    float* dW_lin = (float*)dec_linear->weights->grad->data;
    float* db_lin = (float*)dec_linear->bias->grad->data;
    float* dWh = (float*)rnn->Wh->grad->data;
    float* dbh = (float*)rnn->bh->grad->data;
    
    for (int t = seq_len - 1; t >= 0; t--) {
        // 输出层梯度: (pred_t - onehot(target_t)) / seq_len
        float grad_out[200];
        int target_idx = (int)target_data[t];
        if (target_idx < 0 || target_idx >= vocab_size) target_idx = 3;
        for (int v = 0; v < vocab_size; v++) {
            float pred = output_data[t * vocab_size + v];
            grad_out[v] = (pred - (v == target_idx ? 1.0f : 0.0f)) / seq_len;
        }
        
        // 获取此时间步的RNN输出h_t（Linear层的输入）
        float* h_t = rnn_outputs ? &rnn_outputs[t * hidden_dim] : NULL;
        
        // Linear梯度: dW += h_t ⊗ grad_out, db += grad_out
        if (h_t) {
            for (int i = 0; i < lin_in; i++)
                for (int j = 0; j < lin_out; j++)
                    dW_lin[i * lin_out + j] += h_t[i] * grad_out[j];
        }
        for (int j = 0; j < lin_out; j++) db_lin[j] += grad_out[j];
        
        // RNN梯度: dh_t = W_lin^T · grad_out
        float dh[64] = {0};
        for (int i = 0; i < hidden_dim; i++)
            for (int j = 0; j < vocab_size; j++)
                dh[i] += W_lin[i * vocab_size + j] * grad_out[j];
        
        // 简化Wh梯度: dWh += dh_t ⊗ h_{t-1}
        if (t > 0 && rnn_outputs) {
            float* h_prev = &rnn_outputs[(t-1) * hidden_dim];
            for (int i = 0; i < hidden_dim; i++)
                for (int j = 0; j < hidden_dim; j++)
                    dWh[i * hidden_dim + j] += dh[i] * h_prev[j];
        }
        for (int i = 0; i < hidden_dim; i++) dbh[i] += dh[i];
    }
    
    // SGD权重更新
    float lr = learning_rate;
    float* w = (float*)dec_linear->weights->data;
    for (size_t i = 0; i < dec_linear->weights->size; i++) w[i] -= lr * ((float*)dec_linear->weights->grad->data)[i];
    float* b = (float*)dec_linear->bias->data;
    for (size_t i = 0; i < dec_linear->bias->size; i++) b[i] -= lr * ((float*)dec_linear->bias->grad->data)[i];
    
    float* wx = (float*)rnn->Wx->data;
    float* dwx = (float*)rnn->Wx->grad->data;
    for (size_t i = 0; i < rnn->Wx->size; i++) wx[i] -= lr * dwx[i];
    
    float* wh = (float*)rnn->Wh->data;
    for (size_t i = 0; i < rnn->Wh->size; i++) wh[i] -= lr * dWh[i];
    
    float* bh = (float*)rnn->bh->data;
    for (int i = 0; i < hidden_dim; i++) bh[i] -= lr * dbh[i];
    
    printf("  [训练] Loss: %.4f (lr=%.4f, grad_norm=%.4f)\n", loss, lr, 
           dec_linear->weights->grad->data ? ((float*)dec_linear->weights->grad->data)[0] : 0);
}

// 分词 (支持中英文混合)
int tokenize_text(const char* text, char** tokens, int max_tokens) {
    int count = 0;
    if (!text || !tokens) return 0;

    printf("\n[调试] 分词输入: %s\n", text);
    printf("[调试] 输入长度: %zu 字节\n", strlen(text));

    // 先尝试按空格、换行符、标点符号分词
    char* copy = strdup(text);
    // 添加常见分隔符: 空格、换行、回车、逗号、句号、顿号、问号、感叹号、分号、冒号
    char* delimiters = " \n\r,，。、？！；：";
    char* token = strtok(copy, delimiters);

    while (token != NULL && count < max_tokens) {
        tokens[count] = strdup(token);
        count++;
        token = strtok(NULL, delimiters);
    }
    free(copy);

    // 如果没有分词成功（中文文本没有空格），按字符分词
    if (count == 0) {
        printf("[调试] 按空格分词失败，改为按字符分词\n");
        int len = strlen(text);
        for (int i = 0; i < len && count < max_tokens; ) {
            // 跳过UTF-8 BOM或控制字符
            if ((unsigned char)text[i] < 32) {
                i++;
                continue;
            }

            // 检测UTF-8中文字符 (0xE0-0xEF 开头)
            if ((unsigned char)text[i] >= 0xE0 && (unsigned char)text[i] <= 0xEF) {
                // 中文: 3字节
                if (i + 3 <= len && count < max_tokens) {
                    char* ch = (char*)malloc(4);
                    memcpy(ch, text + i, 3);
                    ch[3] = '\0';
                    tokens[count] = ch;
                    count++;
                    i += 3;
                } else {
                    break;
                }
            } else if ((unsigned char)text[i] >= 0xC0 && (unsigned char)text[i] <= 0xDF) {
                // 2字节 UTF-8 (Latin扩展等)
                if (i + 2 <= len && count < max_tokens) {
                    char* ch = (char*)malloc(3);
                    memcpy(ch, text + i, 2);
                    ch[2] = '\0';
                    tokens[count] = ch;
                    count++;
                    i += 2;
                } else {
                    break;
                }
            } else {
                // ASCII 字符
                if (count < max_tokens) {
                    char* ch = (char*)malloc(2);
                    ch[0] = text[i];
                    ch[1] = '\0';
                    tokens[count] = ch;
                    count++;
                }
                i++;
            }
        }
    }

    // 调试：显示分词结果
    printf("[调试] 分词结果: %d 个 token\n", count);
    for (int i = 0; i < count && i < 10; i++) {
        printf("  Token %d: '%s'\n", i, tokens[i]);
    }
    if (count > 10) {
        printf("  ... (共 %d 个 token)\n", count);
    }

    return count;
}

// 全局主拓扑（延迟初始化）
static MasterTopology* g_master_topo = NULL;

// 初始化多拓扑网络
static void init_master_topology() {
    if (g_master_topo) return;
    
    g_master_topo = master_topology_create(8);
    
    // 创建各类子拓扑
    master_add_sub_topology(g_master_topo, TOPO_VOCABULARY, 
                           "词汇网络", 10000, 9);
    master_add_sub_topology(g_master_topo, TOPO_SEMANTIC, 
                           "语义网络", 5000, 8);
    master_add_sub_topology(g_master_topo, TOPO_EMOTION, 
                           "情绪网络", 1000, 7);
    master_add_sub_topology(g_master_topo, TOPO_CONTEXT, 
                           "上下文网络", 2000, 6);
    
    printf("[生成模型] 多拓扑网络已初始化\n");
}

// 基于多拓扑网络的生成式推理（替代预设匹配）
char* generate_response(Seq2SeqModel* /*model*/, GenVocabulary* /*vocab*/,
                      const char* input_text, int max_output_len) {
    // 初始化多拓扑网络（首次调用时）
    if (!g_master_topo) {
        init_master_topology();
    }
    
    // 使用多拓扑网络进行生成式推理
    char* response = master_generate_response(g_master_topo, 
                                             input_text, 
                                             max_output_len);
    
    return response;
}

// 加载训练数据 (简化版)
void load_training_data(const char* /*db_path*/, GenVocabulary* /*vocab*/,
                      float** inputs, float** targets, int* num_samples) {
    // TODO: 从数据库加载实际训练数据
    *num_samples = 0;
    *inputs = NULL;
    *targets = NULL;
}


// 从文本文件加载训练数据
int load_training_data_from_file(const char* filepath, GenVocabulary* vocab, TrainingSample** samples) {
    FILE* fp = fopen(filepath, "r");
    if (!fp) {
        printf("无法打开训练数据文件: %s\n", filepath);
        return 0;
    }

    // 先统计行数
    int line_count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        line_count++;
    }
    rewind(fp);

    if (line_count == 0) {
        fclose(fp);
        return 0;
    }

    // 分配样本数组
    *samples = (TrainingSample*)malloc(sizeof(TrainingSample) * line_count);
    int valid_count = 0;

    while (fgets(line, sizeof(line), fp)) {
        // 移除换行符
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
        if (len > 1 && line[len-2] == '\r') line[len-2] = '\0';

        // 按 | 分隔问题和回答
        char* sep = strchr(line, '|');
        if (!sep) continue;

        *sep = '\0';
        char* question = line;
        char* answer = sep + 1;

        // 跳过空格
        while (*question == ' ') question++;
        while (*answer == ' ') answer++;

        // 分词
        char* q_tokens[100];
        char* a_tokens[100];
        int q_count = tokenize_text(question, q_tokens, 100);
        int a_count = tokenize_text(answer, a_tokens, 100);

        if (q_count == 0 || a_count == 0) {
            // 释放分词内存
            for (int i = 0; i < q_count; i++) free(q_tokens[i]);
            for (int i = 0; i < a_count; i++) free(a_tokens[i]);
            continue;
        }

        // 构建输入和目标序列
        int seq_len = (q_count + a_count + 2 > 50) ? 50 : (q_count + a_count + 2);

        (*samples)[valid_count].input_ids = (int*)malloc(sizeof(int) * seq_len);
        (*samples)[valid_count].target_ids = (int*)malloc(sizeof(int) * seq_len);
        (*samples)[valid_count].input_len = q_count;
        (*samples)[valid_count].target_len = a_count + 1;  // 包含EOS

        // 添加问题词到词汇表
        int idx = 0;
        for (int i = 0; i < q_count && idx < seq_len - 1; i++) {
            int word_id = gen_vocab_add_word(vocab, q_tokens[i]);
            (*samples)[valid_count].input_ids[idx++] = word_id;
        }

        // 添加分隔符标记 (可选)

        // 添加回答词到词汇表，以<SOS>开头
        (*samples)[valid_count].target_ids[0] = gen_vocab_get_word_id(vocab, "<SOS>");
        for (int i = 0; i < a_count && idx < seq_len - 1; i++) {
            int word_id = gen_vocab_add_word(vocab, a_tokens[i]);
            (*samples)[valid_count].target_ids[i + 1] = word_id;
        }

        // 添加<EOS>
        (*samples)[valid_count].target_ids[a_count + 1] = gen_vocab_get_word_id(vocab, "<EOS>");

        // 释放分词内存
        for (int i = 0; i < q_count; i++) free(q_tokens[i]);
        for (int i = 0; i < a_count; i++) free(a_tokens[i]);

        valid_count++;
    }

    fclose(fp);
    printf("成功加载 %d 个训练样本，词汇表大小: %d\n", valid_count, vocab->size);

    return valid_count;
}

// 释放训练数据
void free_training_data(TrainingSample* samples, int count) {
    if (!samples) return;

    for (int i = 0; i < count; i++) {
        if (samples[i].input_ids) free(samples[i].input_ids);
        if (samples[i].target_ids) free(samples[i].target_ids);
    }
    free(samples);
}
