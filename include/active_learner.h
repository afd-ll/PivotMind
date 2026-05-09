/**
 * @file active_learner.h
 * @brief 主动学习器头文件 - 后台持续学习模块
 */

#ifndef ACTIVE_LEARNNER_H
#define ACTIVE_LEARNNER_H

#include "multi_topology.h"
#include "memory_system.h"
#include <pthread.h>
#include <stdbool.h>
#include <time.h>

// ==================== 主动学习器结构 ====================

typedef struct {
    MasterTopology* master;    // 多拓扑网络
    MemorySystem* memory;      // 记忆系统
    
    // 线程控制
    pthread_t thread;          // 学习线程
    int is_running;            // 是否运行中
    int learning_interval;     // 学习间隔（秒）
    pthread_mutex_t mutex;     // 线程锁
    
    // 学习统计
    int total_concepts_learned;   // 累计学习概念数
    int total_relations_learned;  // 累计建立关系数
    int total_forgotten;          // 累计遗忘/清理数
    time_t start_time;            // 开始时间
    
    // 对象池（性能优化）
    void* metric_pool;            // ObjectPool* for ImportanceMetrics
} ActiveLearner;

// ==================== API函数 ====================

// 创建/销毁
ActiveLearner* active_learner_create(MasterTopology* master, MemorySystem* memory);
void active_learner_destroy(ActiveLearner* learner);

// 启停控制
void active_learner_start(ActiveLearner* learner);
void active_learner_stop(ActiveLearner* learner);
void active_learner_set_interval(ActiveLearner* learner, int seconds);

// 学习功能
void learn_from_memory(ActiveLearner* learner);
void discover_new_relations(ActiveLearner* learner);
void cleanup_forgotten_knowledge(ActiveLearner* learner);

// 用户反馈学习
void learn_from_feedback(ActiveLearner* learner, const char* question,
                        const char* correct_answer, int is_correct);

// 对话中学习（旧接口，保留兼容）
void learn_from_dialog(ActiveLearner* learner, const char* user_input,
                      const char* ai_response, const char* user_feedback);

/**
 * 非自主纠偏：用户反馈后的置信度修正
 * @param learner   主动学习器
 * @param user_input   用户输入
 * @param ai_response  AI回复
 * @param user_feedback  用户反馈（"correct"/"wrong"/"更好的回答:..."）
 * 
 * 这层只做一件事：用户说不对时压下置信度
 * 学习本身由 autonomic_learn_from_dialog() 负责
 */
void feedback_correct(ActiveLearner* learner, const char* user_input,
                     const char* ai_response, const char* user_feedback);

// 统计
void print_learning_stats(ActiveLearner* learner);

// 线程函数（内部使用）
void* learning_cycle(void* arg);

#endif // ACTIVE_LEARNNER_H