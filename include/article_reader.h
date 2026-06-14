/**
 * @file article_reader.h
 * @brief 文章阅读模式 — 字符级共现统计 + PMI 词发现
 *
 * 原理：
 *   输入纯文本 → 字符级共现统计 → PMI 计算 → 分层合评分 → 发现词边界
 *   → 创建概念节点 + 相邻词之间建边
 *
 * 设计理念：词性是涌现，不是标签。
 *   不依赖外部词典、不预定义词表、不标注词性。
 *   从字符共现模式中自然涌现出词语边界。
 *   新词默认进入词汇区（脑区归属由后续连接模式决定）。
 */

#ifndef ARTICLE_READER_H
#define ARTICLE_READER_H

#include "multi_topology.h"

// ==================== 配置 ====================

typedef struct {
    int   window_size;        // 共现窗口半径 (默认 2)
    float pmi_threshold;      // PMI 合并阈值 (默认 1.5)
    int   min_freq;           // 最低频率 (默认 2)
    float alpha;              // PMI 权重 (默认 0.4)
    float beta;               // 频次奖励权重 (默认 0.4)
    float gamma;              // 连接奖励权重 (默认 0.2)
    int   batch_size;         // 每处理 N 行执行一次词发现 (默认 200)
    int   verbose;            // 详细输出
} ArticleReaderConfig;

#define ARTICLE_READER_DEFAULT_CONFIG { \
    2, 1.5f, 2, 0.4f, 0.4f, 0.2f, 200, 0 \
}

// ==================== 公共 API ====================

typedef struct ArticleReader ArticleReader;

/**
 * 创建文章阅读器
 * @param master  主拓扑（用于创建节点）
 * @param cfg     配置（NULL=默认）
 */
ArticleReader* article_reader_create(MasterTopology* master,
                                     ArticleReaderConfig* cfg);

/** 销毁 */
void article_reader_destroy(ArticleReader* ar);

/**
 * 处理一行文章文本
 * 内部累积字符级共现统计
 * @return 累积行数达到 batch_size 时进行词发现并返回 >0
 */
int article_process_line(ArticleReader* ar, const char* line);

/**
 * 手动触发词发现 + 建图
 * 将累积的共现统计转换为拓扑节点和边
 * @param ar
 * @param topo 目标拓扑（一般传入词汇拓扑）
 * @return 新增词数量, -1 出错
 */
int article_flush(ArticleReader* ar, SubTopology* topo);

/**
 * 设置进度回调（给 train_mode 用）
 * @param total_added_nodes_ptr  用于累计新增节点数的 long 指针
 * @param total_added_edges_ptr  用于累计新增边数的 long 指针
 */
void article_set_progress_ptr(ArticleReader* ar,
                              long* total_added_nodes_ptr,
                              long* total_added_edges_ptr);

/**
 * 获取当前统计数据
 * @param ar
 * @param out_chars     输出：累计处理的不同字符数
 * @param out_pairs     输出：累计处理的字符对种类数
 * @param out_words     输出：已发现的词数
 */
void article_get_stats(ArticleReader* ar,
                       int* out_chars, int* out_pairs, int* out_words);

#endif // ARTICLE_READER_H
