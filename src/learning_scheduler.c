/**
 * @file learning_scheduler.c
 * @brief 学习调度器实现
 *
 * 闭环流程：
 *   1. SELF_LEARN: 执行 cfg.self_learn_cycles 次 self_learner_cycle()
 *      每次中间休眠 cfg.self_learn_interval_s 秒
 *   2. BATCH_LEARN: 如果有 batch_corpus_path，创建 TrainMode 跑一轮增量训练
 *      否则跳过（自学习闭环本身也产生知识）
 *   3. EVALUATE: 扫描节点置信度/热力分布，标记低效节点为冷冻候选
 *   4. 回到 1
 *
 * 线程安全：通过 scheduler->lock 保护统计数据的读取
 */

#include "learning_scheduler.h"
#include "huarong_topology.h"
#include "node_importance.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* ================================================================
 *  内部：自学习阶段
 * ================================================================ */

static int run_self_learn_phase(LearningScheduler* ls) {
    if (!ls->self_learner) return 0;

    int total_mods = 0;
    int cycles_done = 0;

    for (int i = 0; i < ls->cfg.self_learn_cycles && !ls->should_stop; i++) {
        int mods = self_learner_cycle(ls->self_learner);
        total_mods += mods;
        cycles_done++;

        if (ls->cfg.verbose && mods > 0) {
            printf("[调度] 自学习 cycle#%d: %d 处修正\n", i + 1, mods);
            fflush(stdout);
        }

        // 两次 cycle 间休眠，避免 CPU 100%
        // 如果应停止，提前退出
        for (int s = 0; s < ls->cfg.self_learn_interval_s && !ls->should_stop; s++) {
            sleep(1);
        }
    }

    if (ls->cfg.verbose) {
        printf("[调度] 自学习阶段完成: %d cycles, %d 处修正\n",
               cycles_done, total_mods);
        fflush(stdout);
    }

    ls->last_sel_cycles = cycles_done;
    ls->last_sel_mods   = total_mods;
    return total_mods;
}

/* ================================================================
 *  内部：批量训练阶段
 * ================================================================ */

static int run_batch_learn_phase(LearningScheduler* ls) {
    if (!ls->cfg.batch_corpus_path) {
        if (ls->cfg.verbose) {
            printf("[调度] 跳过批量训练 (未配置语料路径)\n");
            fflush(stdout);
        }
        return 0;
    }

    // 创建 TrainMode，配置为增量训练 (rounds=1, 高速)
    TrainConfig tcfg = {
        .corpus_path = ls->cfg.batch_corpus_path,
        .format      = CORPUS_JSON_QA,
        .rounds      = 1,
        .speed       = 50,
        .batch_learn_interval = 200,
        .save_interval      = 50000,
        .verbose     = ls->cfg.verbose,
    };

    // 自动检测语料格式
    tcfg.format = train_detect_format(tcfg.corpus_path);

    TrainMode* tm = train_mode_create(ls->master, ls->memory, ls->learner, tcfg);
    if (!tm) {
        fprintf(stderr, "[调度] 批量训练创建失败\n");
        return -1;
    }

    ls->train_mode = tm;

    // 记录训练前节点数
    int nodes_before = 0;
    for (int t = 0; t < ls->master->sub_topo_count; t++) {
        if (ls->master->sub_topologies[t] && ls->master->sub_topologies[t]->net)
            nodes_before += ls->master->sub_topologies[t]->net->node_count;
    }
    int edges_before = 0;
    for (int t = 0; t < ls->master->sub_topo_count; t++) {
        SubTopology* sub = ls->master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        for (int n = 0; n < sub->net->node_count; n++) {
            if (sub->net->nodes[n])
                edges_before += sub->net->nodes[n]->connection_count;
        }
    }

    // 启动训练
    if (train_mode_start(tm) != 0) {
        fprintf(stderr, "[调度] 批量训练启动失败\n");
        train_mode_destroy(tm);
        ls->train_mode = NULL;
        return -1;
    }

    if (ls->cfg.verbose) {
        printf("[调度] 批量训练已启动 (语料: %s)...\n", tcfg.corpus_path);
        fflush(stdout);
    }

    // 等待训练完成（定期检查 should_stop）
    while (!ls->should_stop) {
        TrainProgress p = train_mode_get_progress(tm);
        if (p.state == TRAIN_COMPLETED || p.state == TRAIN_ERROR) break;
        sleep(5); // 每 5 秒检查一次
    }

    // 停止并清理
    train_mode_stop(tm);

    // 统计增量
    int nodes_after = 0;
    for (int t = 0; t < ls->master->sub_topo_count; t++) {
        if (ls->master->sub_topologies[t] && ls->master->sub_topologies[t]->net)
            nodes_after += ls->master->sub_topologies[t]->net->node_count;
    }
    int edges_after = 0;
    for (int t = 0; t < ls->master->sub_topo_count; t++) {
        SubTopology* sub = ls->master->sub_topologies[t];
        if (!sub || !sub->net) continue;
        for (int n = 0; n < sub->net->node_count; n++) {
            if (sub->net->nodes[n])
                edges_after += sub->net->nodes[n]->connection_count;
        }
    }

    ls->last_batch_nodes = nodes_after - nodes_before;
    ls->last_batch_edges = edges_after - edges_before;

    TrainProgress final_p = train_mode_get_progress(tm);
    train_mode_destroy(tm);
    ls->train_mode = NULL;

    if (ls->cfg.verbose) {
        printf("[调度] 批量训练完成: +%d 节点, +%d 边, 喂料 %ld 条\n",
               ls->last_batch_nodes, ls->last_batch_edges, final_p.total_fed);
        fflush(stdout);
    }

    return 0;
}

/* ================================================================
 *  内部：评估阶段
 * ================================================================ */

/**
 * 简单评估：扫描所有子拓扑的节点，按置信度+连接数排序，
 * 标记底部 (1 - eval_keep_ratio) 比例的节点为冷冻候选。
 *
 * 实际冻结不在调度器内做——只统计候选数，各拓扑自行决定何时 freeze。
 */
static int run_evaluate_phase(LearningScheduler* ls) {
    if (!ls->master) return 0;

    int candidate_count = 0;
    int total_nodes = 0;
    float keep_ratio = ls->cfg.eval_keep_ratio;
    if (keep_ratio <= 0.0f || keep_ratio > 1.0f) keep_ratio = 0.9f;

    for (int t = 0; t < ls->master->sub_topo_count && !ls->should_stop; t++) {
        SubTopology* sub = ls->master->sub_topologies[t];
        if (!sub || !sub->net || sub->net->node_count == 0) continue;

        HuarongTopologyNet* net = sub->net;

        // 收集节点置信度
        typedef struct {
            int node_id;
            float confidence;
            int conn_count;
        } NodeEval;

        int n_count = 0;
        for (int i = 0; i < net->node_count; i++) {
            if (net->nodes[i]) n_count++;
        }

        if (n_count == 0) continue;

        // 动态分配
        NodeEval* evals = (NodeEval*)malloc(n_count * sizeof(NodeEval));
        if (!evals) continue;

        int idx = 0;
        for (int i = 0; i < net->node_count; i++) {
            ReasoningNode* node = net->nodes[i];
            if (!node) continue;
            // 置信度：取连接平均置信度，若无连接则 = 0.1
            float avg_conf = 0.1f;
            if (node->connection_count > 0 && node->connection_confidences) {
                float sum = 0;
                for (int c = 0; c < node->connection_count; c++)
                    sum += node->connection_confidences[c];
                avg_conf = sum / node->connection_count;
            }
            evals[idx].node_id    = node->node_id;
            evals[idx].confidence = avg_conf;
            evals[idx].conn_count = node->connection_count;
            idx++;
        }

        // 按 (置信度升序, 连接数降序) 排序 — 低置信 + 少连接 = 冷冻候选
        for (int i = 0; i < idx - 1; i++) {
            for (int j = i + 1; j < idx; j++) {
                int swap = 0;
                if (evals[j].confidence < evals[i].confidence) swap = 1;
                else if (evals[j].confidence == evals[i].confidence &&
                         evals[j].conn_count > evals[i].conn_count) swap = 1;
                if (swap) {
                    NodeEval tmp = evals[i];
                    evals[i] = evals[j];
                    evals[j] = tmp;
                }
            }
        }

        // 底部 (1 - keep_ratio) 为候选
        int can_freeze = (int)(idx * (1.0f - keep_ratio));
        if (can_freeze < 0) can_freeze = 0;

        // 只标记，不真正冻结
        if (ls->cfg.verbose && can_freeze > 0) {
            printf("[调度] 拓扑%d(%s): %d节点, %d冷冻候选 (最低置信度: %.3f)\n",
                   t, sub->name ? sub->name : "?", idx, can_freeze,
                   can_freeze > 0 ? evals[0].confidence : 0);
            fflush(stdout);
        }

        candidate_count += can_freeze;
        total_nodes += idx;
        free(evals);
    }

    ls->last_eval_candidates = candidate_count;

    if (ls->cfg.verbose) {
        printf("[调度] 评估完成: %d/%d 节点为冷冻候选 (保留率 %.0f%%)\n",
               candidate_count, total_nodes, keep_ratio * 100);
        fflush(stdout);
    }

    return candidate_count;
}

/* ================================================================
 *  后台线程主循环
 * ================================================================ */

static void* scheduler_thread_func(void* arg) {
    LearningScheduler* ls = (LearningScheduler*)arg;

    while (!ls->should_stop) {
        // === 阶段 1: 自学习 ===
        ls->phase = LS_SELF_LEARN;
        ls->phase_start_time = time(NULL);
        run_self_learn_phase(ls);

        if (ls->should_stop) break;

        // === 阶段 2: 批量训练 ===
        ls->phase = LS_BATCH_LEARN;
        ls->phase_start_time = time(NULL);
        run_batch_learn_phase(ls);

        if (ls->should_stop) break;

        // === 阶段 3: 评估 ===
        ls->phase = LS_EVALUATE;
        ls->phase_start_time = time(NULL);
        run_evaluate_phase(ls);

        // 完成一轮闭环
        pthread_mutex_lock(&ls->lock);
        ls->total_loops++;
        pthread_mutex_unlock(&ls->lock);

        if (ls->cfg.verbose) {
            printf("[调度] 闭环#%d 完成 (自学习:%d件 | 批量:+%d节点,+%d边 | 评估:%d候选)\n",
                   ls->total_loops,
                   ls->last_sel_mods,
                   ls->last_batch_nodes, ls->last_batch_edges,
                   ls->last_eval_candidates);
            fflush(stdout);
        }
    }

    ls->phase = LS_IDLE;
    return NULL;
}

/* ================================================================
 *  API 实现
 * ================================================================ */

LearningScheduler* learning_scheduler_create(MasterTopology* master,
                                             MemorySystem* memory,
                                             ActiveLearner* learner,
                                             SchedulerConfig* cfg) {
    if (!master) return NULL;

    LearningScheduler* ls = (LearningScheduler*)calloc(1, sizeof(LearningScheduler));
    if (!ls) return NULL;

    ls->master  = master;
    ls->memory  = memory;
    ls->learner = learner;

    if (cfg) {
        ls->cfg = *cfg;
    } else {
        SchedulerConfig d = SCHEDULER_DEFAULT_CONFIG;
        ls->cfg = d;
    }

    // 如果未提供自学习cycle数，设置默认
    if (ls->cfg.self_learn_cycles <= 0) ls->cfg.self_learn_cycles = 60;
    if (ls->cfg.self_learn_interval_s <= 0) ls->cfg.self_learn_interval_s = 60;

    // 创建自学习器
    SelfLearnerConfig sl_cfg = SELF_LEARNER_DEFAULT_CONFIG;
    sl_cfg.verbose = ls->cfg.verbose;
    ls->self_learner = self_learner_create(master, &sl_cfg);

    pthread_mutex_init(&ls->lock, NULL);
    ls->phase = LS_IDLE;

    return ls;
}

void learning_scheduler_destroy(LearningScheduler* ls) {
    if (!ls) return;

    learning_scheduler_stop(ls);

    if (ls->self_learner) {
        self_learner_destroy(ls->self_learner);
        ls->self_learner = NULL;
    }

    if (ls->train_mode) {
        train_mode_destroy(ls->train_mode);
        ls->train_mode = NULL;
    }

    pthread_mutex_destroy(&ls->lock);
    free(ls);
}

int learning_scheduler_start(LearningScheduler* ls) {
    if (!ls) return -2;

    if (ls->phase != LS_IDLE) return -1; // 已在运行

    ls->should_stop = 0;
    ls->total_loops = 0;

    if (pthread_create(&ls->thread, NULL, scheduler_thread_func, ls) != 0) {
        return -2;
    }

    return 0;
}

void learning_scheduler_stop(LearningScheduler* ls) {
    if (!ls || ls->phase == LS_IDLE) return;

    ls->should_stop = 1;

    // 如果正在批量训练，也通知它停止
    if (ls->train_mode) {
        train_mode_stop(ls->train_mode);
    }

    pthread_join(ls->thread, NULL);
}

LearningPhase learning_scheduler_get_phase(LearningScheduler* ls) {
    if (!ls) return LS_IDLE;
    return ls->phase;
}

void learning_scheduler_get_stats(LearningScheduler* ls,
                                  int* total_loops,
                                  int* last_sel_cycles,
                                  int* last_sel_mods,
                                  int* last_batch_nodes,
                                  int* last_batch_edges,
                                  int* last_eval_candidates,
                                  const char** phase_name,
                                  long* phase_elapsed_s) {
    if (!ls) return;

    pthread_mutex_lock(&ls->lock);
    if (total_loops)         *total_loops         = ls->total_loops;
    if (last_sel_cycles)     *last_sel_cycles     = ls->last_sel_cycles;
    if (last_sel_mods)       *last_sel_mods       = ls->last_sel_mods;
    if (last_batch_nodes)    *last_batch_nodes    = ls->last_batch_nodes;
    if (last_batch_edges)    *last_batch_edges    = ls->last_batch_edges;
    if (last_eval_candidates) *last_eval_candidates = ls->last_eval_candidates;
    if (phase_name)          *phase_name          = learning_phase_name(ls->phase);
    if (phase_elapsed_s)     *phase_elapsed_s     = (long)(time(NULL) - ls->phase_start_time);
    pthread_mutex_unlock(&ls->lock);
}
