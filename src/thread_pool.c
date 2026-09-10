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
#include <time.h>
#include <errno.h>

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

/* 带超时的条件等待，timeout_sec 秒后返回 ETIMEDOUT */
static int cond_timedwait_sec(pthread_cond_t* cond, pthread_mutex_t* mtx, int timeout_sec) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_sec;
    return pthread_cond_timedwait(cond, mtx, &ts);
}

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
        /* 仅在「非批次进行中」时才响应 shutdown：若 destroy 恰逢批次进行
         * (running=1)，worker 应完成本批任务并上报 done，让 batch() 正常收尾，
         * 而不是中途退出导致 workers_done 永远凑不齐、destroy 只能等超时。 */
        if (pool->shutdown && !pool->running) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }
        /* 锁内快照本批任务数：防止本批超时强退后，下一批重置 next_index/
         * task_count 导致慢 worker 串批执行新批次任务（跨代干扰）。
         * ⚠ 安全前提（P1-1 修复后新增，勿改回超时逻辑）：thread_pool_batch()
         * 已废弃"10s 超时强制结束并返回成功"，超时只告警并继续等待——即
         * "batch 返回 == 本批任务全部执行完"是 API 契约。本快照只能防"串批"，
         * 挡不住"提前返回"：一旦有人恢复超时提前返回，调用方会按 batch 已返回
         * 释放栈上 tasks（dialog_system/multi_topology 均为栈数组），而慢 worker
         * 仍在按本快照窃取执行 → 立刻复现 use-after-free。 */
        int batch_count = pool->task_count;
        pthread_mutex_unlock(&pool->mutex);

        // 任务窃取：原子取下一个未分配的任务（用本地快照计数）
        while (1) {
            int idx = __sync_fetch_and_add(&pool->next_index, 1);
            if (idx >= batch_count) break;
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

    /* 标记正在销毁，防止新 batch 提交 */
    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->cv_batch);
    /* 等待进行中的 batch 完成（最多等5秒，防止worker崩溃导致的永久死锁） */
    int destroy_timeout = 0;
    while (pool->running && !destroy_timeout) {
        int rc = cond_timedwait_sec(&pool->cv_done, &pool->mutex, 5);
        if (rc == ETIMEDOUT) {
            fprintf(stderr, "[线程池] 警告：等待批次完成超时，强制销毁\n");
            destroy_timeout = 1;
        }
    }
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

    // 设置批次（必须在锁内完成：与 worker 的"上报完成"临界区互斥，
    // 确保上一批 worker 全部退出窃取后才能重置批次状态。旧版无锁重置 +
    // volatile 不足以建立 happens-before，属 C11 数据竞争 UB）
    pthread_mutex_lock(&pool->mutex);
    pool->tasks = tasks;
    pool->task_count = count;
    pool->next_index = 0;
    pool->workers_done = 0;
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
    // P1-1: 旧版等待 10s 超时后"强制结束"并返回成功——若 worker 仍在执行
    // 剩余任务，调用方按"batch 已返回"释放 tasks（dialog_system/multi_topology
    // 均为栈数组）→ use-after-free。batch 返回 == 本批全部执行完是 API 契约，
    // 因此超时只能告警、不能提前返回。worker 若真死循环属任务自身 bug，会让
    // 本调用卡住，但不会把崩溃转成更隐蔽的 UAF。
    pthread_mutex_lock(&pool->mutex);
    int warn_count = 0;
    while (pool->workers_done < pool->num_workers) {
        int rc = cond_timedwait_sec(&pool->cv_done, &pool->mutex, 10);
        if (rc == ETIMEDOUT) {
            warn_count++;
            fprintf(stderr, "[线程池] 警告：等待 worker 完成超过 %d0s (第 %d 次告警，继续等待)\n",
                    warn_count, warn_count);
        }
    }
    pool->running = 0;
    /* 唤醒可能在 destroy() 中等待批次结束的线程（旧版 batch 收尾不广播
     * cv_done，destroy 只能干等 5s 超时兜底） */
    pthread_cond_broadcast(&pool->cv_done);
    pthread_mutex_unlock(&pool->mutex);

    return count;
}

int thread_pool_num_workers(ThreadPool* pool) {
    return pool ? pool->num_workers : 0;
}
