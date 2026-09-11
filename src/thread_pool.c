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

/* C4: batch() 的“池忙”返回码 THREAD_POOL_BUSY 的正式定义已收口到
 * include/thread_pool.h（对外契约，值 -2：池忙时未执行任何任务，
 * 调用方须把手上的 tasks 串行跑掉）。本文件已 #include "thread_pool.h"，
 * 故此处不再重复定义。 */

// ==================== 内部结构 ====================

struct ThreadPool {
    int num_workers;              // worker 线程数
    pthread_t* workers;           // worker 线程数组

    // 当前批次
    ThreadTask* tasks;            // 任务数组（外部引用，不拥有）
    int task_count;               // 任务总数
    volatile int next_index;      // 下一个待窃取的任务索引（**只在 mutex 内读写**，见下）
    /* R3-1: 本批尚未执行完的任务数（原子递减；0 == 本批所有 func 已返回）。
     * 取代旧的 workers_done——后者记的是"worker 跑完的趟数"：同一 worker 出列后
     * 因 running 仍为 1 会立刻再进一轮"空转趟"并再记一笔，于是 num_workers 次
     * 记数能由远少于 num_workers 个 worker 攒齐 → batch() 提前返回。 */
    volatile int tasks_left;
    int batch_epoch;              // R3-1: 批次代次，submit 时 +1（受 mutex 保护）

    // 同步
    pthread_mutex_t mutex;
    pthread_cond_t cv_batch;      // 通知 worker 新批次到来
    pthread_cond_t cv_done;       // 通知主线程批次完成
    volatile int running;         // 1 = 批次进行中
    volatile int in_batch;        // C4: 1 = 有批次在池内执行（受 mutex 保护，批次闸门）
    volatile int shutdown;        // 1 = 销毁中

    // 统计
    int total_batches;
    int total_tasks_executed;
};

// ==================== CPU 核数检测 ====================

/* 带超时的条件等待，timeout_sec 秒后返回 ETIMEDOUT */
static int cond_timedwait_sec(pthread_cond_t* cond, pthread_mutex_t* mtx, int timeout_sec) {
    struct timespec ts;
    /* P2-3 附带：用 CLOCK_MONOTONIC——CLOCK_REALTIME 在树莓派上会被 RTC/NTP 校时
     * 跳变，10s 超时可能瞬间触发或几乎不触发。cv_done 必须以
     * pthread_condattr_setclock(CLOCK_MONOTONIC) 创建，否则 pthread_cond_timedwait
     * 会返回 EINVAL。 */
    clock_gettime(CLOCK_MONOTONIC, &ts);
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
    /* R3-1: 本 worker 已认领的批次代次。0 = 尚未认领任何批次
     * （batch_epoch 初值 0，故创建后 worker 一律先睡在 cv_batch 上）。 */
    int my_epoch = 0;

    while (1) {
        /* R3-1: 代次握手 —— 一个 worker 对同一代**只进窃取循环一次**。
         * 旧闸门只判 running，而批内 running 恒为 1，worker 出列后立刻再进一轮
         * "空转趟"（一件活不干）并给 workers_done 多记一笔——这就是屏障被提前
         * 凑满、batch() 提前返回、调用方 free 与 worker 解引用重叠的根源。
         * 现在：只有池里确实推进了代次（有新批次）才准进；无活可干就睡在真实
         * 批次信号上（不是休眠/退避，只是不再空转白烧 CPU）。 */
        pthread_mutex_lock(&pool->mutex);
        while (pool->batch_epoch == my_epoch && !pool->shutdown) {
            pthread_cond_wait(&pool->cv_batch, &pool->mutex);
        }
        if (pool->shutdown) {
            /* 本 worker 退出前，它对已认领代次的任务要么已执行完、要么已在上方
             * 递减 tasks_left；且它此刻不在窃取循环里（不会再去碰任务数组），
             * 因此 batch() 的完成账不受影响，destroy() 可安全 join。 */
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }
        my_epoch = pool->batch_epoch;
        pthread_mutex_unlock(&pool->mutex);

        /* R3-1: 任务窃取。索引分配、数组快照、代次校验**同处一个临界区**：
         *  worker 因此绝不可能拿"上一代的数组"去执行"下一代的索引"
         *  （旧版 TSan 报的 thread_pool.c:270↔:121 与跨代串批，源头即此）。
         *  临界区只有几条指令，不含任务体；并行度一点没减（红线：不加麻药）。 */
        while (1) {
            pthread_mutex_lock(&pool->mutex);
            if (pool->batch_epoch != my_epoch) {   /* batch() 已收尾/换批 → 退出本代 */
                pthread_mutex_unlock(&pool->mutex);
                break;
            }
            int idx = pool->next_index++;
            ThreadTask* bt = pool->tasks;
            int bc = pool->task_count;
            pthread_mutex_unlock(&pool->mutex);

            if (idx >= bc) break;                  /* 本代无活了 */
            bt[idx].func(bt[idx].arg);
            __sync_fetch_and_add(&pool->total_tasks_executed, 1);

            /* R3-1: 本任务已执行完（func 已返回）→ 递减未完成账；
             * 减到 0 的那个线程负责唤醒 batch() 的等待者。
             * 注意：此递减发生在 func 返回**之后**，所以 tasks_left==0
             *  ⟹ 所有 func 都已返回 ⟹ 调用方 free 安全。 */
            if (__sync_sub_and_fetch(&pool->tasks_left, 1) == 0) {
                pthread_mutex_lock(&pool->mutex);
                pthread_cond_broadcast(&pool->cv_done);
                pthread_mutex_unlock(&pool->mutex);
            }
        }
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
    pool->in_batch = 0;      /* C4: 批次闸门初始空闲 */
    pool->shutdown = 0;
    pool->next_index = 0;
    pool->tasks_left = 0;    /* R3-1: 完成账（无在飞批次） */
    pool->batch_epoch = 0;   /* R3-1: 代次 0 = 尚无批次（worker 的 my_epoch 初值同为 0） */
    pool->total_batches = 0;
    pool->total_tasks_executed = 0;

    pthread_mutex_init(&pool->mutex, NULL);
    /* C5/P2-3: 两个条件变量都用 CLOCK_MONOTONIC（配合 cond_timedwait_sec） */
    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&pool->cv_batch, &cattr);
    pthread_cond_init(&pool->cv_done, &cattr);
    pthread_condattr_destroy(&cattr);

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
    /* C5: 等正在跑的批次收尾。旧版 5s 超时置 destroy_timeout 后**继续**往下走
     * 到 free(pool)——但没有任何手段中止 worker 手上的任务，随后的 join/free
     * 与仍在跑的任务访问的内存重叠 → use-after-free；若某任务真卡死，销毁还会
     * 永久阻塞在 :199 的无超时 join（watchdog 反复 SIGKILL）。
     * 现在：超时只告警并继续等（有上限）；上限到了就"放弃释放"——宁可泄漏一个
     * 池，也不制造 UAF（此时进程已在退出路径，泄漏不影响后续运行）。 */
    int waited = 0;
    while (pool->in_batch) {
        int rc = cond_timedwait_sec(&pool->cv_done, &pool->mutex, 5);
        if (rc == ETIMEDOUT) {
            waited++;
            fprintf(stderr, "[线程池] 警告：等待批次完成超时 %ds (第 %d 次，不强制释放)\n",
                    waited * 5, waited);
            if (waited >= 3) {   /* 累计 15s 仍未收尾 → 判定卡死 */
                pthread_mutex_unlock(&pool->mutex);
                fprintf(stderr, "[线程池] 错误：批次卡死，放弃销毁"
                                "（泄漏线程池以避免 use-after-free）\n");
                return;          /* 不 join、不 free：池与 mutex 保持有效 */
            }
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

    /* 设置批次（必须在锁内完成：与 worker 的"代次握手/窃取"临界区互斥）。
     * C4: 批次闸门 —— 池内同一时刻只允许一个批次。
     * 旧版无此守卫：两个连接线程并发进入后，后进者会把 tasks/task_count/
     * next_index 全改成自己的批次 → 先进者的 worker 执行错批次任务、
     * 先进者可能提前返回并 free(tasks)，而慢 worker 仍在写该数组（UAF）。
     * 这里不做阻塞等待：忙时立即返回 THREAD_POOL_BUSY，由调用方串行降级
     * （不引入新的阻塞点，关闭路径不会因此挂住）。 */
    pthread_mutex_lock(&pool->mutex);
    if (pool->shutdown) {                 /* C5: 关闭中/已销毁，拒绝提交 */
        pthread_mutex_unlock(&pool->mutex);
        return -1;
    }
    if (pool->in_batch) {                 /* C4: 池正忙，本批不执行 */
        pthread_mutex_unlock(&pool->mutex);
        return THREAD_POOL_BUSY;
    }
    pool->in_batch = 1;
    pool->tasks = tasks;
    pool->task_count = count;
    pool->next_index = 0;
    /* R3-1: 完成账 = 任务数（不是 worker 趟数）。谁跑完一个任务谁减一，
     * 减到 0 即"本批全部执行完"。 */
    pool->tasks_left = count;
    pool->batch_epoch++;                    /* R3-1: 推进代次 → 唤醒 worker 认领本批 */
    pool->running = 1;
    pool->total_batches++;
    pthread_cond_broadcast(&pool->cv_batch);
    pthread_mutex_unlock(&pool->mutex);

    // 主线程也参与任务窃取（与 worker 同一把锁分配索引：索引复位与所有窃取严格有序）
    int local_executed = 0;
    while (1) {
        pthread_mutex_lock(&pool->mutex);
        int idx = pool->next_index++;
        ThreadTask* bt = pool->tasks;
        int bc = pool->task_count;
        pthread_mutex_unlock(&pool->mutex);
        if (idx >= bc) break;
        bt[idx].func(bt[idx].arg);
        local_executed++;
        if (__sync_sub_and_fetch(&pool->tasks_left, 1) == 0) {
            pthread_mutex_lock(&pool->mutex);
            pthread_cond_broadcast(&pool->cv_done);
            pthread_mutex_unlock(&pool->mutex);
        }
    }
    __sync_fetch_and_add(&pool->total_tasks_executed, local_executed);

    /* P1-1 + R3-1: 屏障。旧判据是 workers_done（趟数，可被同一 worker 重复记账），
     * 于是 batch() 会在任务刚开跑时返回 —— 调用方随即 free(tasks)（dialog_system.c:814、
     * multi_topology.c:1275 均为堆数组），而仍在 dialog_topo_worker 里的 worker 继续
     * 按任务指针解引用 → use-after-free（ASan: dialog_system.c:149）。
     * 现在唯一判据是 tasks_left：它只在 func **返回之后** 递减，故
     * "batch 返回 == 本批全部执行完"成为真契约。超时仍只告警、绝不提前返回。 */
    pthread_mutex_lock(&pool->mutex);
    int warn_count = 0;
    /* R3-3: 读侧也必须是原子访问。tasks_left 在 worker 侧（:142）与本函数的主线程
     * 窃取侧（:305）都由 __sync_sub_and_fetch 原子递减；此处若做普通读，就是
     * "原子 RMW vs 非原子读"的**混杂访问**（TSan 原始报：thread_pool.c:142
     * "Atomic write of size 4" ↔ thread_pool.c:321 "Previous read of size 4"，
     * 且 mutex 只在本侧 → 与 worker 的原子 RMW 不构成 happens-before）。
     * __atomic_load_n(..., __ATOMIC_ACQUIRE) 是**纯原子读**（带 acquire 栅栏），
     * 与 __sync_sub_and_fetch 的 release-RMW 在同一原子对象上同步；
     * 语义完全不变（判据仍是 tasks_left 归零 = 本批 func 全返回）；
     * 不加锁、不休眠/退避（红线：不加麻药）。 */
    while (__atomic_load_n(&pool->tasks_left, __ATOMIC_ACQUIRE) > 0) {
        int rc = cond_timedwait_sec(&pool->cv_done, &pool->mutex, 10);
        if (rc == ETIMEDOUT) {
            warn_count++;
            /* P2-2: 旧版 "%d0s" 把 warn_count 拼成分钟数（1 → "10s"，10 → "100s"）。 */
            fprintf(stderr, "[线程池] 警告：等待 worker 完成超过 %ds (第 %d 次告警，继续等待)\n",
                    warn_count * 10, warn_count);
        }
    }
    pool->running = 0;
    pool->in_batch = 0;      /* C4: 释放闸门 —— 之后才允许下一批进入 */
    /* 唤醒可能在 destroy() 中等待批次结束的线程（旧版 batch 收尾不广播）
     * 以及所有已出列、正睡在 cv_batch 上的 worker（它们会看到代次未变而继续睡，
     * 这是预期的：下一批进来时 cv_batch 的广播才会把它们唤醒）。 */
    pthread_cond_broadcast(&pool->cv_done);
    pthread_mutex_unlock(&pool->mutex);

    return count;
}

int thread_pool_num_workers(ThreadPool* pool) {
    return pool ? pool->num_workers : 0;
}
