/* tools/probe_batch_contract.c — 批次完成语义探针（G-T1）
 * 用法: ./probe_batch_contract <workers> <tasks> <iters>
 * 判据: 每次 batch() 返回时 tasks_completed == tasks；否则打印 *** CONTRACT VIOLATED ***
 * 编译: gcc -O1 -g -pthread -Iinclude -o /tmp/probe_batch_contract \
 *          tools/probe_batch_contract.c src/thread_pool.c
 */
#include "thread_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>

static volatile int g_done = 0;                 /* 已完成任务数（探针自己记） */
static void sleep_task(void* arg) {
    (void)arg;
    struct timespec ts = {0, 20 * 1000 * 1000}; /* 20ms：与 uaf-dialog-topoworker.md §4.3 同口径 */
    nanosleep(&ts, NULL);
    __sync_fetch_and_add((int*)&g_done, 1);
}

int main(int argc, char** argv) {
    int nw = (argc > 1) ? atoi(argv[1]) : 20;
    int nt = (argc > 2) ? atoi(argv[2]) : 9;
    int it = (argc > 3) ? atoi(argv[3]) : 3;
    if (nt <= 0 || it <= 0) { fprintf(stderr, "bad args\n"); return 2; }

    ThreadPool* pool = thread_pool_create_with_size(nw);
    if (!pool) { fprintf(stderr, "pool create failed\n"); return 2; }
    printf("[probe] pool_workers=%d tasks=%d task_duration=20ms\n", nw, nt);

    /* 任务数组每次迭代重新 calloc —— 与 dialog_system.c:785 同型（堆数组、随后 free） */
    ThreadTask* th = (ThreadTask*)calloc((size_t)nt, sizeof(ThreadTask));
    if (!th) return 2;
    for (int i = 0; i < nt; i++) { th[i].func = sleep_task; th[i].arg = NULL; }

    int violations = 0;
    for (int k = 0; k < it; k++) {
        g_done = 0;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int rc = thread_pool_batch(pool, th, nt);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        int done = (int)g_done;
        const char* verdict;
        if (rc < 0) { verdict = "BAD RC"; violations++; }
        else if (done != nt) { verdict = "*** CONTRACT VIOLATED ***"; violations++; }
        else verdict = "CONTRACT HOLDS";
        printf("[probe] iter%d rc=%d batch_returned_after=%7.2fms tasks_completed_at_return=%d/%d -> %s\n",
               k, rc, ms, done, nt, verdict);
    }
    printf("[probe] VERDICT: %s (%d violations / %d iters)\n",
           violations ? "FAIL" : "PASS", violations, it);
    free(th);
    thread_pool_destroy(pool);
    return violations ? 1 : 0;
}
