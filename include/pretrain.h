#ifndef PRETRAIN_H
#define PRETRAIN_H

#include "vocab.h"
#include "layer.h"
#include <time.h>

// ========== 预训练模型类型 ==========

typedef enum {
    PRETRAIN_MODE_SKIPGRAM = 0,    // Skip-gram: 中心词预测上下文
    PRETRAIN_MODE_Cbow    = 1     // CBOW: 上下文预测中心词
} PretrainMode;

// ========== 预训练配置 ==========

typedef struct {
    int embedding_dim;           // 嵌入维度
    int window_size;             // 上下文窗口大小
    int window_size_max;         // 最大窗口大小（动态窗口）
    int negative_samples;        // 负采样数量
    float learning_rate;         // 初始学习率
    float learning_rate_min;     // 最小学习率
    int epochs;                 // 训练轮数
    int batch_size;             // 批大小（批内词对数）
    float subsample_thresh;      // 高频词下采样阈值
    int min_count;              // 最小词频
    int max_vocab_size;         // 最大词表大小
    int num_workers;            // 工作线程数
    float sample_rate;          // 采样率（0-1）
    int save_every_n_lines;    // 每N行保存一次检查点
    int validate_every_n_lines; // 每N行验证一次
    int verbose;                // 详细输出

    // 新增配置
    PretrainMode mode;          // 训练模式（Skip-gram / CBOW）
    int use_momentum;          // 使用动量优化
    float momentum;            // 动量系数（默认0.9）
    int use_grad_clip;         // 使用梯度裁剪
    float grad_clip_value;     // 梯度裁剪阈值
    int phrase_min_count;      // 短语检测最小共现次数
    float phrase_threshold;    // 短语检测PMI阈值
    int use_position_weight;   // 使用位置权重（越近权重越大）
    float warmup_ratio;        // 学习率预热比例（0-1）
} PretrainConfig;

// ========== 词对结构（用于批处理）==========

typedef struct {
    int center_id;
    int context_id;
    int distance;       // 中心词到上下文词的距离
    float weight;      // 采样权重（用于下采样）
} WordPair;

// ========== 预训练状态 ==========

typedef struct {
    Vocab* vocab;                // 词表
    Layer* embedding_layer;      // 嵌入层（输入向量）
    float* context_weights;      // 上下文权重矩阵（Skip-gram 输出层）
    int total_words;            // 训练词数
    int trained_words;          // 已训练词数
    int epoch;                  // 当前轮次
    float loss;                 // 当前总损失
    float loss_count;           // 损失计数
    float current_lr;           // 当前学习率
    float lr_decay_factor;      // 学习率衰减因子
    int use_hs;                 // 使用分层softmax（替代负采样）
    int use_subsample;          // 使用下采样

    // Unigram table（基于词频的负采样表）
    int* unigram_table;         // 负采样表
    int unigram_table_size;     // 表大小
    float* unigram_cumsum;       // 累积分布

    // 验证相关
    float best_loss;            // 最佳验证损失
    int no_improve_count;       // 无改善次数

    // 统计
    clock_t train_start_time;   // 训练开始时间
    int total_pairs;            // 总词对数
    int subsampled_pairs;       // 被下采样的词对数
    int neg_samples_used;        // 实际负采样数

    // 新增：动量缓冲区
    float* grad_momentum_embed;    // 嵌入层动量
    float* grad_momentum_context;  // 上下文权重动量

    // 新增：训练模式
    PretrainMode mode;            // 当前训练模式

    // 新增：检查点恢复
    int resume_line;              // 恢复训练的起始行号
    int resume_epoch;             // 恢复训练的起始epoch

    // 新增：warmup步数
    int warmup_steps;             // 预热步数
    int global_step;              // 全局训练步数

    // 新增：批处理缓冲区
    WordPair* batch_buffer;       // 批处理词对缓冲区
    int batch_buffer_size;        // 缓冲区大小
    int batch_buffer_count;       // 当前缓冲区中的词对数
} PretrainState;

// ========== 预训练API ==========

/**
 * 创建默认预训练配置
 */
PretrainConfig* pretrain_config_create_default(void);

/**
 * 创建高质量预训练配置
 */
PretrainConfig* pretrain_config_create_quality(void);

/**
 * 销毁预训练配置
 */
void pretrain_config_destroy(PretrainConfig* cfg);

/**
 * 创建预训练状态
 * @param vocab 词表
 * @param embedding_dim 嵌入维度
 * @return 预训练状态
 */
PretrainState* pretrain_state_create(Vocab* vocab, int embedding_dim);

/**
 * 初始化预训练状态的高级特性
 * @param state 预训练状态
 * @param config 预训练配置
 * @return 0成功，-1失败
 */
int pretrain_state_init_advanced(PretrainState* state, PretrainConfig* config);

/**
 * 销毁预训练状态
 */
void pretrain_state_destroy(PretrainState* state);

/**
 * 保存预训练后的嵌入层权重（完整格式）
 * @param state 预训练状态
 * @param filepath 文件路径
 * @return 0成功，-1失败
 */
int pretrain_save_weights(PretrainState* state, const char* filepath);

/**
 * 保存预训练后的嵌入层权重（兼容格式，仅嵌入向量）
 * @param state 预训练状态
 * @param filepath 文件路径
 * @return 0成功，-1失败
 */
int pretrain_save_embeddings_only(PretrainState* state, const char* filepath);

/**
 * 加载预训练状态（从已保存的嵌入层）
 * @param vocab 词表
 * @param weights_path 权重文件路径
 * @return 预训练状态，失败返回NULL
 */
PretrainState* pretrain_state_load(Vocab* vocab, const char* weights_path);

/**
 * 从文件预训练嵌入层（单轮）
 * @param state 预训练状态
 * @param filepath 文件路径
 * @param config 预训练配置
 * @return 训练的词对数，-1失败
 */
int pretrain_from_file(PretrainState* state, const char* filepath, PretrainConfig* config);

/**
 * 从多个文件预训练（多轮）
 * @param state 预训练状态
 * @param filepaths 文件路径数组
 * @param count 文件数量
 * @param config 预训练配置
 * @return 训练的词对总数
 */
int pretrain_from_files(PretrainState* state, const char** filepaths, int count,
                        PretrainConfig* config);

/**
 * 从字符串预训练（内存数据）
 * @param state 预训练状态
 * @param text 输入文本
 * @param config 预训练配置
 * @return 训练的词对数
 */
int pretrain_from_text(PretrainState* state, const char* text, PretrainConfig* config);

/**
 * 获取词的嵌入向量
 * @param state 预训练状态
 * @param word 词文本
 * @param vec 输出向量（需预先分配，大小为embedding_dim）
 * @return 0成功，-1词不存在
 */
int pretrain_get_embedding(PretrainState* state, const char* word, float* vec);

/**
 * 获取词的嵌入向量（通过ID）
 * @param state 预训练状态
 * @param word_id 词ID
 * @param vec 输出向量
 * @return 0成功，-1失败
 */
int pretrain_get_embedding_by_id(PretrainState* state, int word_id, float* vec);

/**
 * 计算两个向量的余弦相似度
 * @param vec1 向量1
 * @param vec2 向量2
 * @param dim 维度
 * @return 余弦相似度
 */
float pretrain_cosine_similarity(const float* vec1, const float* vec2, int dim);

/**
 * 计算两个向量的欧氏距离
 * @param vec1 向量1
 * @param vec2 向量2
 * @param dim 维度
 * @return 欧氏距离
 */
float pretrain_euclidean_distance(const float* vec1, const float* vec2, int dim);

/**
 * 查找相似词
 * @param state 预训练状态
 * @param word 目标词
 * @param top_k 返回数量
 * @param results 输出词数组（需预先分配）
 * @param scores 输出相似度数组
 * @return 实际返回数量
 */
int pretrain_find_similar(PretrainState* state, const char* word, int top_k,
                          char** results, float* scores);

/**
 * 查找相似词（使用已有向量）
 * @param state 预训练状态
 * @param vec 查询向量
 * @param top_k 返回数量
 * @param results 输出词数组
 * @param scores 输出相似度数组
 * @return 实际返回数量
 */
int pretrain_find_similar_by_vec(PretrainState* state, const float* vec, int top_k,
                                  char** results, float* scores);

/**
 * 打印预训练进度
 */
void pretrain_print_progress(PretrainState* state);

/**
 * 打印详细训练统计
 */
void pretrain_print_stats(PretrainState* state);

/**
 * 获取当前训练速度（词/秒）
 * @param state 预训练状态
 * @return 词/秒
 */
float pretrain_get_speed(PretrainState* state);

/**
 * 计算词表覆盖率
 * @param state 预训练状态
 * @param text 测试文本
 * @return 覆盖率（0-1）
 */
float pretrain_compute_coverage(PretrainState* state, const char* text);

/**
 * 评估预训练质量（使用类比推理）
 * @param state 预训练状态
 * @param test_pairs 测试词对数组（格式: "word1 word2 word3 expected"）
 * @param count 测试对数量
 * @return 准确率
 */
float pretrain_evaluate_analogy(PretrainState* state, const char** test_pairs, int count);

/**
 * 对嵌入进行归一化
 * @param state 预训练状态
 * @return 0成功
 */
int pretrain_normalize_embeddings(PretrainState* state);

/**
 * 打印最常见的词（用于检查）
 */
void pretrain_print_top_words(PretrainState* state, int top_n);

// ========== 新增API ==========

/**
 * 使用指定配置创建预训练状态
 * @param vocab 词表
 * @param config 预训练配置
 * @return 预训练状态
 */
PretrainState* pretrain_state_create_with_config(Vocab* vocab, PretrainConfig* config);

/**
 * 使用CBOW模式训练上下文词预测中心词
 * @param state 预训练状态
 * @param context_ids 上下文词ID数组
 * @param context_count 上下文词数量
 * @param center_id 中心词ID
 * @param lr 学习率
 * @param neg_count 负采样数量
 * @return 损失值
 */
float pretrain_train_cbow(PretrainState* state, const int* context_ids, int context_count,
                          int center_id, float lr, int neg_count);

/**
 * 批量累积梯度更新（支持动量和梯度裁剪）
 * @param state 预训练状态
 * @param config 预训练配置
 */
void pretrain_flush_batch(PretrainState* state, PretrainConfig* config);

/**
 * 从检查点恢复训练
 * @param vocab 词表
 * @param ckpt_path 检查点路径
 * @return 预训练状态
 */
PretrainState* pretrain_state_resume(Vocab* vocab, const char* ckpt_path);

/**
 * 检测高频共现词对作为短语
 * @param vocab 词表
 * @param texts 文本数组
 * @param text_count 文本数量
 * @param min_count 最小共现次数
 * @param pmi_threshold PMI阈值
 * @param out_phrases 输出短语数组（需预先分配，大小为max_phrases）
 * @param max_phrases 最大短语数量
 * @return 实际检测到的短语数量
 */
int pretrain_detect_phrases(Vocab* vocab, const char** texts, int text_count,
                            int min_count, float pmi_threshold,
                            char** out_phrases, int max_phrases);

/**
 * 向现有预训练状态添加新词
 * @param state 预训练状态
 * @param new_vocab 新词表（包含新增词）
 * @param init_strategy 初始化策略（0=随机，1=平均附近词）
 * @return 0成功，-1失败
 */
int pretrain_expand_vocab(PretrainState* state, Vocab* new_vocab, int init_strategy);

/**
 * 评估嵌入质量的多种指标
 * @param state 预训练状态
 * @param test_texts 测试文本数组
 * @param count 测试文本数量
 * @param out_coherence 输出相干性分数数组（可为空）
 * @return 综合质量分数（0-1）
 */
float pretrain_evaluate_quality(PretrainState* state, const char** test_texts,
                                 int count, float* out_coherence);

/**
 * 获取训练进度百分比
 * @param state 预训练状态
 * @param total_lines 总行数
 * @return 进度百分比（0-100）
 */
float pretrain_get_progress(PretrainState* state, int total_lines);

#endif // PRETRAIN_H
