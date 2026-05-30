#ifndef THREAD_POOL_H
#define THREAD_POOL_H

/**
 * thread_pool.h — 自动检测CPU核数的轻量线程池
 *
 * 设计原则：
 * - 固定大小线程池（n = CPU核数）
 * - 批量任务提交：batch = N个任务，线程池自动分配
 * - 主线程也参与任务窃取，100%利用CPU
 * - 所有任务完成后再返回（barrier同步）
 * - 跨平台：pthread（Linux/macOS）+ Windows兼容
 *
 * 用法：
 *   ThreadPool* pool = thread_pool_create();
 *   ThreadTask tasks[] = {{func1, arg1}, {func2, arg2}};
 *   thread_pool_batch(pool, tasks, 2);
 *   thread_pool_destroy(pool);
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 线程池任务 */
typedef struct {
    void (*func)(void* arg);   // 任务函数
    void* arg;                 // 任务参数
} ThreadTask;

/** 线程池（不透明结构体） */
typedef struct ThreadPool ThreadPool;

/**
 * 创建线程池
 * 自动检测CPU核心数作为worker数量
 * @return 线程池指针，失败返回NULL
 */
ThreadPool* thread_pool_create(void);

/**
 * 创建指定worker数量的线程池
 * @param num_threads 指定worker数（<=0则自动检测）
 * @return 线程池指针，失败返回NULL
 */
ThreadPool* thread_pool_create_with_size(int num_threads);

/**
 * 销毁线程池
 * 等待所有worker结束后释放资源
 */
void thread_pool_destroy(ThreadPool* pool);

/**
 * 批量提交任务并等待全部完成
 * 主线程也参与任务窃取
 * @param pool 线程池
 * @param tasks 任务数组
 * @param count 任务数量
 * @return 已完成任务数，失败返回-1
 */
int thread_pool_batch(ThreadPool* pool, ThreadTask* tasks, int count);

/**
 * 获取线程池的worker数量
 * @param pool 线程池
 * @return worker数
 */
int thread_pool_num_workers(ThreadPool* pool);

#ifdef __cplusplus
}
#endif

#endif // THREAD_POOL_H
