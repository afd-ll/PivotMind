/**
 * @file text_trainer.h
 * @brief 通用文本训练器 - 自动训练任意文本到多拓扑神经网络
 * 
 * 设计理念：
 * - 自动扫描 data/classics/ 目录的所有文本文件
 * - 自动识别格式（【标题】作者\n内容）
 * - 统一训练流程，无需为每种文本写专门代码
 * - 增量训练，避免重复
 * 
 * 使用方法：
 *   TextTrainer* trainer = trainer_create();
 *   trainer_scan_and_train(trainer);  // 自动训练所有文本
 *   trainer_save(trainer, "data/topology_memory.bin");
 *   trainer_free(trainer);
 */

#ifndef TEXT_TRAINER_H
#define TEXT_TRAINER_H

#include "multi_topology.h"
#include "memory_system.h"
#include <stdbool.h>

/**
 * 训练配置
 */
typedef struct {
    char scan_dir[512];        // 扫描目录（默认 data/classics）
    char output_path[512];     // 输出路径（默认 data/topology_memory.bin）
    
    // 拓扑训练开关
    bool train_vocab;          // 训练词汇拓扑
    bool train_semantic;       // 训练语义拓扑
    bool train_culture;        // 训练文化拓扑
    bool train_syntax;         // 训练语法拓扑
    bool train_context;        // 训练上下文拓扑
    
    // 训练参数
    int min_text_length;       // 最小文本长度（过滤短文本）
    float confidence_threshold; // 置信度阈值
    bool verbose;              // 详细输出
} TrainingConfig;

/**
 * 训练统计
 */
typedef struct {
    int total_files;           // 总文件数
    int total_texts;           // 总文本数（诗词、篇章等）
    int total_chars;           // 总字符数
    
    // 各拓扑节点数
    int vocab_nodes;
    int semantic_nodes;
    int culture_nodes;
    int syntax_nodes;
    int context_nodes;
    
    // 跨拓扑连接数
    int cross_links;
    
    // 训练时间
    double training_time_ms;
} TrainingStats;

/**
 * 文本信息（解析后的结构）
 */
typedef struct {
    char title[256];           // 标题
    char author[256];          // 作者
    char content[10000];       // 内容
    char source[256];          // 来源文件
    int line_count;            // 行数
} TextInfo;

/**
 * 训练器结构
 */
typedef struct TextTrainer TextTrainer;

// ========== 核心API ==========

/**
 * 创建训练器
 */
TextTrainer* trainer_create(void);

/**
 * 创建训练器（带配置）
 */
TextTrainer* trainer_create_with_config(const TrainingConfig* config);

/**
 * 释放训练器
 */
void trainer_free(TextTrainer* trainer);

// ========== 训练流程 ==========

/**
 * 扫描并训练所有文本（一键训练）
 * @return 成功训练的文本数量
 */
int trainer_scan_and_train(TextTrainer* trainer);

/**
 * 训练单个文件
 */
bool trainer_train_file(TextTrainer* trainer, const char* filepath);

/**
 * 训练单个文本
 */
bool trainer_train_text(TextTrainer* trainer, const TextInfo* text);

// ========== 输入输出 ==========

/**
 * 保存训练结果
 */
bool trainer_save(TextTrainer* trainer, const char* filepath);

/**
 * 加载已有训练结果
 */
bool trainer_load(TextTrainer* trainer, const char* filepath);

/**
 * 获取训练统计
 */
TrainingStats trainer_get_stats(const TextTrainer* trainer);

/**
 * 打印训练报告
 */
void trainer_print_report(const TextTrainer* trainer);

// ========== 工具函数 ==========

/**
 * 解析文本文件
 * @param filepath 文件路径
 * @param texts 输出文本数组
 * @param max_texts 最大文本数
 * @return 实际解析的文本数
 */
int parse_text_file(const char* filepath, TextInfo* texts, int max_texts);

/**
 * 获取默认配置
 */
TrainingConfig trainer_get_default_config(void);

#endif // TEXT_TRAINER_H
