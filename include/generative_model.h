#ifndef GENERATIVE_MODEL_H
#define GENERATIVE_MODEL_H

#include "tensor.h"
#include "model.h"
#include <stdbool.h>

// 词汇表结构（生成模型专用）
typedef struct {
    char** words;       // 词列表
    int size;          // 词汇表大小
    int max_size;      // 最大容量
} GenVocabulary;

// 训练样本结构
typedef struct {
    int* input_ids;
    int input_len;
    int* target_ids;
    int target_len;
} TrainingSample;

// 序列到序列(Seq2Seq)模型
typedef struct {
    Model* encoder;     // 编码器
    Model* decoder;     // 解码器
    int embedding_dim;  // 词嵌入维度
    int hidden_dim;     // 隐藏层维度
    int vocab_size;     // 词汇表大小
    int max_seq_len;    // 最大序列长度
} Seq2SeqModel;

// 初始化词汇表（使用 gen_ 前缀避免与 vocab 模块冲突）
GenVocabulary* gen_vocab_create(int max_size);
void gen_vocab_destroy(GenVocabulary* vocab);
int gen_vocab_add_word(GenVocabulary* vocab, const char* word);
int gen_vocab_get_word_id(GenVocabulary* vocab, const char* word);
const char* gen_vocab_get_word(GenVocabulary* vocab, int id);

// 创建Seq2Seq模型
Seq2SeqModel* seq2seq_create(int vocab_size, int embedding_dim,
                             int hidden_dim, int max_seq_len);
void seq2seq_destroy(Seq2SeqModel* model);

// 编码输入序列
Tensor* encode_sequence(Seq2SeqModel* model, Tensor* input_seq);

// 解码生成回复
Tensor* decode_sequence(Seq2SeqModel* model, Tensor* encoder_output,
                       int target_start_id);

// 训练Seq2Seq模型
void seq2seq_train(Seq2SeqModel* model, Tensor* input_seq,
                   Tensor* target_seq, float learning_rate);

// Teacher Forcing训练
float seq2seq_train_with_teacher_forcing(Seq2SeqModel* model,
                                      Tensor* input_seq,
                                      Tensor* target_seq,
                                      float learning_rate,
                                      int use_teacher_forcing);

// 从编码器状态生成回复
char* generate_response(Seq2SeqModel* model, GenVocabulary* vocab,
                      const char* input_text, int max_output_len);

// 贪婪解码生成序列
char* generate_sequence_greedy(Seq2SeqModel* model, GenVocabulary* vocab,
                              const char* input_text, int max_output_len,
                              float temperature);

// 束搜索解码生成序列
char* beam_search_decode(Seq2SeqModel* model, GenVocabulary* vocab,
                         const char* input_text, int max_output_len,
                         int beam_width, float length_penalty);

// 加载训练数据
void load_training_data(const char* db_path, GenVocabulary* vocab,
                      float** inputs, float** targets, int* num_samples);

// 分词
int tokenize_text(const char* text, char** tokens, int max_tokens);

// 置信度联想推理（基于多拓扑网络）
char* confidence_associative_inference(const char* input_text, int max_output_len);

// 从文件加载训练数据
int load_training_data_from_file(const char* filepath, GenVocabulary* vocab, TrainingSample** samples);

// 释放训练数据
void free_training_data(TrainingSample* samples, int count);

#endif
