/**
 * src/thread_pool.c — 轻量线程池实现
 *
 * 原理：
 * - 创建 N 个 worker 线程（N = CPU核数）
 * - batch() 提交一批任务，所有 worker 被唤醒
 * - 主线程 + workers 都从任务数组中原子窃取任务
 * - 所有任务完成后 batch() 返回
 *
 * 线程安全：
 * - next_index 使用 GCC __sync 原子操作
 * - workers_done 在 mutex 保护下递增
 * - 无锁任务窃取，仅同步用简单 mutex+condvar
 */

#include "thread_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#endif

// ==================== 内部结构 ====================

struct ThreadPool {
    int num_workers;              // worker 线程数
    pthread_t* workers;           // worker 线程数组

    // 当前批次
    ThreadTask* tasks;            // 任务数组（外部引用，不拥有）
    int task_count;               // 任务总数
    volatile int next_index;      // 下一个待窃取的任务索引（原子递增）
    volatile int workers_done;    // 已完成窃取的 worker 数

    // 同步
    pthread_mutex_t mutex;
    pthread_cond_t cv_batch;      // 通知 worker 新批次到来
    pthread_cond_t cv_done;       // 通知主线程批次完成
    volatile int running;         // 1 = 批次进行中
    volatile int shutdown;        // 1 = 销毁中

    // 统计
    int total_batches;
    int total_tasks_executed;
};

// ==================== CPU 核数检测 ====================

static int detect_cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    int n = (int)sysinfo.dwNumberOfProcessors;
    return (n > 0) ? n : 4;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (int)n : 4;
#endif
}

// ==================== Worker 线程 ====================

static void* worker_loop(void* arg) {
    ThreadPool* pool = (ThreadPool*)arg;

    while (1) {
        // 等待批次或退出
        pthread_mutex_lock(&pool->mutex);
        while (!pool->running && !pool->shutdown) {
            pthread_cond_wait(&pool->cv_batch, &pool->mutex);
        }
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }
        pthread_mutex_unlock(&pool->mutex);

        // 任务窃取：原子取下一个未分配的任务
        while (1) {
            int idx = __sync_fetch_and_add(&pool->next_index, 1);
            if (idx >= pool->task_count) break;
            pool->tasks[idx].func(pool->tasks[idx].arg);
            __sync_fetch_and_add(&pool->total_tasks_executed, 1);
        }

        // 报告完成
        pthread_mutex_lock(&pool->mutex);
        pool->workers_done++;
        if (pool->workers_done >= pool->num_workers) {
            // 所有 worker 完成，通知主线程
            pthread_cond_signal(&pool->cv_done);
        }
        pthread_mutex_unlock(&pool->mutex);
    }
    return NULL;
}

// ==================== 创建/销毁 ====================

ThreadPool* thread_pool_create(void) {
    return thread_pool_create_with_size(0);
}

ThreadPool* thread_pool_create_with_size(int num_threads) {
    if (num_threads <= 0) {
        num_threads = detect_cpu_count();
    }
    if (num_threads < 1) num_threads = 1;
    if (num_threads > 64) num_threads = 64;  // 上限保护

    ThreadPool* pool = (ThreadPool*)calloc(1, sizeof(ThreadPool));
    if (!pool) return NULL;

    pool->num_workers = num_threads;
    pool->running = 0;
    pool->shutdown = 0;
    pool->next_index = 0;
    pool->workers_done = 0;
    pool->total_batches = 0;
    pool->total_tasks_executed = 0;

    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cv_batch, NULL);
    pthread_cond_init(&pool->cv_done, NULL);

    pool->workers = (pthread_t*)calloc(num_threads, sizeof(pthread_t));
    if (!pool->workers) {
        free(pool);
        return NULL;
    }

    // 启动 worker 线程
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&pool->workers[i], NULL, worker_loop, pool) != 0) {
            // 创建失败，清理已创建的
            pool->shutdown = 1;
            pthread_cond_broadcast(&pool->cv_batch);
            for (int j = 0; j < i; j++) {
                pthread_join(pool->workers[j], NULL);
            }
            free(pool->workers);
            pthread_mutex_destroy(&pool->mutex);
            pthread_cond_destroy(&pool->cv_batch);
            pthread_cond_destroy(&pool->cv_done);
            free(pool);
            return NULL;
        }
    }

    printf("[线程池] 已创建 %d 个 worker (CPU核数: %d)\n", num_threads, detect_cpu_count());
    return pool;
}

void thread_pool_destroy(ThreadPool* pool) {
    if (!pool) return;

    // 通知所有 worker 退出
    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->cv_batch);
    pthread_mutex_unlock(&pool->mutex);

    // 等待所有 worker
    for (int i = 0; i < pool->num_workers; i++) {
        pthread_join(pool->workers[i], NULL);
    }

    printf("[线程池] 销毁 (执行了 %d 批次, %d 任务)\n",
           pool->total_batches, pool->total_tasks_executed);

    free(pool->workers);
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->cv_batch);
    pthread_cond_destroy(&pool->cv_done);
    free(pool);
}

// ==================== 批量提交 ====================

int thread_pool_batch(ThreadPool* pool, ThreadTask* tasks, int count) {
    if (!pool || !tasks || count <= 0) return -1;

    // 设置批次
    pool->tasks = tasks;
    pool->task_count = count;
    pool->next_index = 0;
    pool->workers_done = 0;

    // 保证所有 worker 看到最新数据
    __sync_synchronize();

    // 唤醒所有 worker
    pthread_mutex_lock(&pool->mutex);
    pool->running = 1;
    pool->total_batches++;
    pthread_cond_broadcast(&pool->cv_batch);
    pthread_mutex_unlock(&pool->mutex);

    // 主线程也参与任务窃取
    int local_executed = 0;
    while (1) {
        int idx = __sync_fetch_and_add(&pool->next_index, 1);
        if (idx >= count) break;
        tasks[idx].func(tasks[idx].arg);
        local_executed++;
    }
    __sync_fetch_and_add(&pool->total_tasks_executed, local_executed);

    // 等待所有 worker 完成窃取
    pthread_mutex_lock(&pool->mutex);
    while (pool->workers_done < pool->num_workers) {
        pthread_cond_wait(&pool->cv_done, &pool->mutex);
    }
    pool->running = 0;
    pthread_mutex_unlock(&pool->mutex);

    return count;
}

int thread_pool_num_workers(ThreadPool* pool) {
    return pool ? pool->num_workers : 0;
}
