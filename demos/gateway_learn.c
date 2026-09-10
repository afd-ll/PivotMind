/**
 * @file gateway_learn.c
 * @brief PivotMind HTTP Gateway — 学习队列与 worker
 *
 * 由 demos/pivotmind_gateway.c 拆分而来（v0.5.25 P2-6）。
 * 共享类型与原型见 gateway_internal.h。
 */

#include "gateway_internal.h"

/* 中文预处理：连续 CJK 字符间插入空格，使 strtok 能切出单字 */
static void _cjk_insert_spaces(const char* src, char* dst, int dst_sz) {
    int wi = 0;
    for (const char* p = src; *p && wi < dst_sz - 4; ) {
        unsigned char c = (unsigned char)*p;
        int blen = 1;
        if (c >= 0xE0 && c <= 0xEF) blen = 3;
        else if (c >= 0xC0 && c <= 0xDF) blen = 2;

        /* 多字节字符必须完整写入：检查是否有足够空间放 字符+空格+null */
        if (blen > 1 && wi + blen + 2 >= dst_sz) break;

        if (blen == 3 && wi > 0 && (unsigned char)dst[wi-1] != ' '
            && wi + 1 < dst_sz) {
            dst[wi++] = ' ';
        }
        for (int b = 0; b < blen && p[b] && wi < dst_sz - 1; b++)
            dst[wi++] = p[b];
        dst[wi] = '\0';
        p += blen;
    }
    /* 确保 null 结尾 */
    if (wi < dst_sz) dst[wi] = '\0';
}

/* 分词 + 注册到 vocab + 建相邻边，返回新增节点数
 * v0.4.3: 使用 EmergentPOS 同词类边加权 */
int _learn_tokens(SubTopology* vocab, const char* text,
                         int* p_prev_id, EmergentPOS* ep) {
    if (!vocab || !vocab->net || !text || !text[0]) return 0;

    /* 动态分配缓冲区：CJK spaced 最坏情况 = strlen * 4/3 + 1 */
    int text_len = (int)strlen(text);
    int copy_sz = text_len * 4 / 3 + 32;
    if (copy_sz < 256) copy_sz = 256;
    char* copy = (char*)malloc((size_t)copy_sz);
    if (!copy) return 0;

    _cjk_insert_spaces(text, copy, copy_sz);
    char* tok = strtok(copy, " \t\n\r。，！？、；：\"\"''（）《》…—");
    int added = 0;
    while (tok) {
        if (strlen(tok) >= 2) {
            int nid = huarong_net_find_concept(vocab->net, tok);
            if (nid < 0 && (size_t)vocab->net->node_count < vocab->net->max_nodes) {
                nid = huarong_net_dynamic_add_node(vocab->net, tok, NULL, 0);
                if (nid >= 0) {
                    added++;
                    node_hash_add(vocab->node_hash, vocab->net->nodes[nid]);
                }
            }
            if (nid >= 0) {
                vocab->net->nodes[nid]->activation += 0.1f;
                if (*p_prev_id >= 0 && *p_prev_id != nid) {
                    /* v0.4.3: 涌现词类加权 — 同词类节点边权重更高 */
                    float edge_w = 0.4f;
                    if (ep) {
                        ReasoningNode* prev = vocab->net->nodes[*p_prev_id];
                        ReasoningNode* curr = vocab->net->nodes[nid];
                        if (prev && prev->emergent_class_count > 0 &&
                            curr && curr->emergent_class_count > 0) {
                            for (int pi = 0; pi < prev->emergent_class_count && pi < 4; pi++) {
                                for (int ci = 0; ci < curr->emergent_class_count && ci < 4; ci++) {
                                    if (prev->emergent_class_ids[pi] == curr->emergent_class_ids[ci]) {
                                        edge_w = 0.65f; /* 同词类: 更强的语法关联 */
                                        break;
                                    }
                                }
                                if (edge_w > 0.4f) break;
                            }
                        }
                    }
                    huarong_net_add_connection(vocab->net, *p_prev_id, nid, edge_w);
                }
                *p_prev_id = nid;
            }
        }
        tok = strtok(NULL, " \t\n\r。，！？、；：\"\"''（）《》…—");
    }
    free(copy);
    return added;
}

/* _learn_task — 异步学习参数
 * v0.5.20 B4: 加 flush/done/done_mutex/done_cond 完成信号——flush 型任务由调用方
 * wait 并负责释放（worker 只发 done 信号不 free），fire-and-forget 型由 worker free。 */
struct LearnTask {
    char*  msg;
    char   domain[64];  /* "medical", "legal", "" = default vocab */
    int    flush;       /* 1 = 调用方 wait 并释放；0 = worker 处理后释放 */
    int    done;        /* 1 = worker 已处理完 _learn_tokens */
    pthread_mutex_t done_mutex;
    pthread_cond_t  done_cond;
};

/* v0.5.9: 学习队列——固定 worker 消费，防每请求一线程堆积（08-07 35线程实锤） */
#define LEARN_QUEUE_CAP 256
typedef struct {
    LearnTask* tasks[LEARN_QUEUE_CAP];
    int head, tail, count;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    int stop;
} LearnQueue;
static LearnQueue g_learn_q;
static pthread_t g_learn_workers[LEARN_WORKER_COUNT];
/* v0.5.25 fix: 队列/worker 是否已初始化——learn_queue_shutdown 据此守卫，
 * 防止未 init 就 shutdown（join 未创建的 pthread_t）或重复 shutdown（重复 join）。 */
static int g_learn_inited = 0;

/* B4 (v0.5.20): 标记任务完成并唤醒等待者（flush 型调用方在此继续）。 */
static void learn_task_complete(LearnTask* task) {
    if (!task) return;
    pthread_mutex_lock(&task->done_mutex);
    task->done = 1;
    pthread_cond_broadcast(&task->done_cond);
    pthread_mutex_unlock(&task->done_mutex);
}

/* B4 (v0.5.20): 丢弃任务（队列满丢最旧）——flush 型唤醒等待者由其释放，
 * 非 flush 型直接释放。 */
static void learn_task_abandon(LearnTask* task) {
    if (!task) return;
    if (task->flush) {
        learn_task_complete(task);   /* 唤醒等待者，由等待者 free */
    } else {
        free(task->msg);
        free(task);
    }
}

/* B4 (v0.5.20): 可复用入队助手——原 handle_learn 内联入队逻辑抽出，供 handle_chat 复用。
 * flush=1 时返回 task 供调用方 wait；OOM 返回 NULL。队列满丢最旧。 */
LearnTask* learn_queue_push(const char* msg, const char* domain, int flush) {
    LearnTask* task = (LearnTask*)calloc(1, sizeof(LearnTask));
    if (!task) return NULL;
    task->msg = strdup(msg);
    if (!task->msg) { free(task); return NULL; }
    if (domain)
        snprintf(task->domain, sizeof(task->domain), "%s", domain);
    task->flush = flush;
    task->done = 0;
    pthread_mutex_init(&task->done_mutex, NULL);
    pthread_cond_init(&task->done_cond, NULL);

    pthread_mutex_lock(&g_learn_q.mutex);
    if (g_learn_q.count >= LEARN_QUEUE_CAP) {
        /* 队列满：丢最旧（工作记忆丢弃策略——最近的学习请求优先） */
        LearnTask* old = g_learn_q.tasks[g_learn_q.head];
        g_learn_q.head = (g_learn_q.head + 1) % LEARN_QUEUE_CAP;
        g_learn_q.count--;
        learn_task_abandon(old);   /* flush 型唤醒等待者；非 flush 型 free */
    }
    g_learn_q.tasks[g_learn_q.tail] = task;
    g_learn_q.tail = (g_learn_q.tail + 1) % LEARN_QUEUE_CAP;
    g_learn_q.count++;
    pthread_cond_signal(&g_learn_q.cond);
    pthread_mutex_unlock(&g_learn_q.mutex);

    return task;
}

/* B4 (v0.5.20): flush 等待——等 worker 处理完 _learn_tokens（done==1）后释放 task。
 * 本线程独占 task 生命周期：worker 对 flush 型任务只发 done 不 free。 */
void learn_task_wait(LearnTask* task) {
    if (!task) return;
    pthread_mutex_lock(&task->done_mutex);
    while (!task->done)
        pthread_cond_wait(&task->done_cond, &task->done_mutex);
    pthread_mutex_unlock(&task->done_mutex);
    /* task 已完成：本线程负责释放 */
    free(task->msg);
    pthread_mutex_destroy(&task->done_mutex);
    pthread_cond_destroy(&task->done_cond);
    free(task);
}

/* _learn_worker — 学习 worker 线程：循环消费队列（单 worker，B4 后 _learn_tokens 单消费者） */
static void* _learn_worker(void* arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&g_learn_q.mutex);
        while (g_learn_q.count == 0 && !g_learn_q.stop)
            pthread_cond_wait(&g_learn_q.cond, &g_learn_q.mutex);
        if (g_learn_q.stop && g_learn_q.count == 0) {
            pthread_mutex_unlock(&g_learn_q.mutex);
            break;
        }
        LearnTask* task = g_learn_q.tasks[g_learn_q.head];
        g_learn_q.head = (g_learn_q.head + 1) % LEARN_QUEUE_CAP;
        g_learn_q.count--;
        pthread_mutex_unlock(&g_learn_q.mutex);

        if (!task) continue;
        if (!task->msg) {   /* 理论不可达：push 保证 msg 非 NULL，防御性处理 */
            if (task->flush) learn_task_complete(task);
            else free(task);
            continue;
        }
        int is_flush = task->flush;   /* 提前读取：发 done 后 task 可能已被等待者释放 */
        GatewaySystem* gw = g_gw;
        if (gw && gw->topology) {
            /* 按域名选择目标拓扑 */
            int target_topo = TOPO_VOCABULARY;
            if (task->domain[0]) {
                target_topo = TOPO_DOMAIN;
                printf("[gateway] [领域] '%s' → 领域拓扑\n", task->domain);
            }
            SubTopology* topo = NULL;
            for (int t = 0; t < gw->topology->sub_topo_count; t++) {
                if (gw->topology->sub_topologies[t] && (int)gw->topology->sub_topologies[t]->type == target_topo)
                    { topo = gw->topology->sub_topologies[t]; break; }
            }
            if (topo && topo->net) {
                int prev_id = -1;
                EmergentPOS* ep = (gw->prefrontal && gw->prefrontal->controller)
                                  ? gw->prefrontal->controller->emergent_pos : NULL;
                _learn_tokens(topo, task->msg, &prev_id, ep);
                auto_learn_concepts(gw->topology, task->msg, NULL);
                if (gw->perception) perception_feed_learn_text(gw->perception, task->msg);
                __sync_fetch_and_add(&gw->total_learning_cycles, 1);
                /* 语法种子注入 */
                SubTopology* tpl = NULL;
                for (int t = 0; t < gw->topology->sub_topo_count; t++)
                    if (gw->topology->sub_topologies[t] && gw->topology->sub_topologies[t]->type == TOPO_TEMPLATE)
                        { tpl = gw->topology->sub_topologies[t]; break; }
                if (tpl && tpl->net && tpl->net->node_count < 8) broca_seed_grammar(gw->topology);
            }
        }
        /* B4: _learn_tokens 等全部处理完再发 done（flush 型等待者据此唤醒并释放）。
         * 发完 done 后 worker 不得再触碰 task。 */
        if (is_flush) {
            learn_task_complete(task);
        } else {
            free(task->msg);
            free(task);
        }
    }
    return NULL;
}

/* v0.5.9: 初始化学习队列 + 启动 worker（gateway 启动时调用一次）
 * v0.5.20 B4: worker 数由 LEARN_WORKER_COUNT 决定（2→1，单消费者消除并发写）。 */
void learn_queue_init(void) {
    if (g_learn_inited) return;
    memset(&g_learn_q, 0, sizeof(g_learn_q));
    pthread_mutex_init(&g_learn_q.mutex, NULL);
    pthread_cond_init(&g_learn_q.cond, NULL);
    g_learn_q.stop = 0;
    g_learn_inited = 1;   /* 先置位：create 失败的 worker 槽不会被误 join */
    for (int i = 0; i < LEARN_WORKER_COUNT; i++) {
        pthread_create(&g_learn_workers[i], NULL, _learn_worker, NULL);
    }
    fprintf(stderr, "[gateway]   学习队列就绪 (%d worker, 容量 %d)\n", LEARN_WORKER_COUNT, LEARN_QUEUE_CAP);
}

/* v0.5.25 fix: 学习 worker 退出机制——置 stop + 广播唤醒，join 全部 worker。
 * 旧实现从无此函数：gw_system_shutdown 只销毁 brainstem/topology/memory，
 * worker 线程却仍在跑循环，且消费任务时访问 gw->topology / gw->perception
 * → 关闭时 use-after-free（后台线程踩已释放对象）。
 * 必须在 gw_system_shutdown 拆解任何 gw 资源之前调用。 */
void learn_queue_shutdown(void) {
    if (!g_learn_inited) return;

    /* 1. 置停止位并广播：worker 从 cond_wait 唤醒；队列尚有任务时 worker 会
     *    先排空再退出（worker_loop 的"stop && count==0"判定），保证已入队的
     *    学习任务不丢。 */
    pthread_mutex_lock(&g_learn_q.mutex);
    g_learn_q.stop = 1;
    pthread_cond_broadcast(&g_learn_q.cond);
    pthread_mutex_unlock(&g_learn_q.mutex);

    /* 2. join 所有 worker：返回即代表无 worker 再触碰 gw 资源 */
    for (int i = 0; i < LEARN_WORKER_COUNT; i++) {
        pthread_join(g_learn_workers[i], NULL);
    }

    /* 3. 排空残留（防御性：worker 退出前已排空队列，此处理论不可达）：
     *    flush 型唤醒等待者由其自行释放，非 flush 型直接释放。 */
    pthread_mutex_lock(&g_learn_q.mutex);
    while (g_learn_q.count > 0) {
        LearnTask* task = g_learn_q.tasks[g_learn_q.head];
        g_learn_q.head = (g_learn_q.head + 1) % LEARN_QUEUE_CAP;
        g_learn_q.count--;
        pthread_mutex_unlock(&g_learn_q.mutex);
        learn_task_abandon(task);
        pthread_mutex_lock(&g_learn_q.mutex);
    }
    g_learn_q.head = g_learn_q.tail = 0;
    pthread_mutex_unlock(&g_learn_q.mutex);

    g_learn_inited = 0;
    fprintf(stderr, "[gateway]   学习队列已停止 (%d worker 已退出)\n", LEARN_WORKER_COUNT);
}
