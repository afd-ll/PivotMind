/**
 * @file train_mode.h
 * @brief 内置训练模式 — 不停机喂料
 *
 * 解决痛点：以前要停主进程 → 外部脚本喂 → 重启，一套下来两周。
 * 现在一条命令 ./pivotmind_gateway --train-mode 进入训练模式，
 * 引擎不停，HTTP服务不断，后台线程读语料自动喂。
 *
 * 语料格式支持：
 *   1. JSON QA对: [["问","答"], ...]
 *   2. 管道分隔文本: 问|答 (每行一对)
 *   3. 纯文本: 每行一段，分词后建共现边
 *
 * 用法：
 *   ./pivotmind_gateway --train-mode
 *   ./pivotmind_gateway --train-mode --corpus data/hermes_knowledge_base.json --rounds 3
 *   ./pivotmind_gateway --train-mode --corpus data/corpus/wiki_sample.txt --format text --speed 50
 *
 * 运行时 API：
 *   GET  /train/status    → 训练进度
 *   POST /train/pause     → 暂停
 *   POST /train/resume    → 继续
 *   POST /train/stop      → 停止训练（引擎继续跑）
 */

#ifndef TRAIN_MODE_H
#define TRAIN_MODE_H

#include "multi_topology.h"
#include "memory_system.h"
#include "active_learner.h"
#include <pthread.h>
#include <stdbool.h>

// 语料格式
typedef enum {
    CORPUS_JSON_QA,      // [["问","答"], ...]
    CORPUS_PIPE_QA,      // 问|答
    CORPUS_PLAIN_TEXT,   // 纯文本，分词建边
} CorpusFormat;

// 训练配置
typedef struct {
    const char* corpus_path;    // 语料文件路径
    CorpusFormat format;        // 语料格式
    int rounds;                  // 训练轮数 (默认1)
    int speed;                   // 每秒喂多少条 (默认20)
    int batch_learn_interval;   // 每N条触发一次主动学习 (默认100)
    int save_interval;          // 每N条自动存盘 (默认5000)
    int verbose;                // 详细输出
} TrainConfig;

// 训练状态
typedef enum {
    TRAIN_IDLE,        // 未开始
    TRAIN_RUNNING,     // 训练中
    TRAIN_PAUSED,      // 暂停
    TRAIN_COMPLETED,   // 已完成
    TRAIN_ERROR,       // 出错
} TrainState;

// 训练进度
typedef struct {
    TrainState state;
    int current_round;           // 当前第几轮
    int total_rounds;           // 总轮数
    long current_line;          // 当前处理到第几行
    long total_lines;           // 语料总行数
    long total_fed;             // 本轮已喂条数
    long total_learned;         // 累计学习次数
    long total_added_nodes;     // 累计新增节点
    long total_added_edges;     // 累计新建边
    time_t start_time;          // 训练开始时间
    time_t eta;                 // 预计完成时间
    char error_msg[256];        // 错误信息
} TrainProgress;

// 训练模式主结构
typedef struct {
    TrainConfig config;
    TrainProgress progress;

    MasterTopology* topology;
    MemorySystem* memory;
    ActiveLearner* learner;

    pthread_t thread;
    volatile int should_stop;
    volatile int should_pause;
    volatile int is_running;

    // 内部缓冲（避免反复malloc）
    char line_buf[4096];
} TrainMode;

// ==================== API ====================

/**
 * 创建训练模式实例
 * @param topology  多拓扑网络
 * @param memory    记忆系统
 * @param learner   主动学习器
 * @param config    训练配置
 * @return 训练模式实例，失败返回NULL
 */
TrainMode* train_mode_create(MasterTopology* topology,
                              MemorySystem* memory,
                              ActiveLearner* learner,
                              TrainConfig config);

/** 销毁 */
void train_mode_destroy(TrainMode* tm);

/** 启动训练（后台线程） */
int train_mode_start(TrainMode* tm);

/** 暂停 */
void train_mode_pause(TrainMode* tm);

/** 继续 */
void train_mode_resume(TrainMode* tm);

/** 停止（不可恢复） */
void train_mode_stop(TrainMode* tm);

/** 获取进度（线程安全，可用于HTTP接口） */
TrainProgress train_mode_get_progress(TrainMode* tm);

/** 自动检测语料格式 */
CorpusFormat train_detect_format(const char* path);

/** 打印默认配置 */
void train_config_print_defaults(void);

/** 从命令行参数解析训练配置 */
TrainConfig train_config_from_args(int argc, char* argv[]);

#endif // TRAIN_MODE_H
