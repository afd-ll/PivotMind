/**
 * @file brainstem.c
 * @brief 脑干节律控制器实现 — 数字生命的心跳
 *
 * 独立 pthread 线程，维持系统基础节律。
 */

#include "brainstem.h"
#include "huarong_topology.h"
#include "dmn.h"
#include "self_learner.h"
#include "node_cache.h"
#include "thalamus.h"
#include "perception.h"
#include "hippocampus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define msleep(ms) Sleep(ms)
#else
#define msleep(ms) usleep((ms) * 1000)
#endif

static const CognitiveState BASELINE_STATE = {
    .drive_curiosity  = 0.5f,
    .drive_hunger     = 0.3f,
    .drive_social     = 0.5f,
    .drive_comfort    = 0.5f,
    .emotion_pleasure = 0.5f,
    .emotion_pain     = 0.1f,
    .emotion_security = 0.6f,
    .valence          = 0.0f,
    .current_goal     = NULL,
    .goal_strength    = 0.0f,
    .plan_step        = 0,
    .explore_rate     = 0.5f
};

static unsigned int local_rand(unsigned int* seed) {
    *seed = *seed * 1103515245 + 12345;
    return (*seed >> 16) & 0x7FFF;
}

static void drift_cognitive_state(Brainstem* bs) {
    CognitiveState* state = bs->cognitive_state;
    if (!state) return;

    const float keep = 0.995f;
    const float pull = 0.005f;

    state->drive_curiosity = state->drive_curiosity * keep + BASELINE_STATE.drive_curiosity * pull;
    state->drive_hunger    = state->drive_hunger    * keep + BASELINE_STATE.drive_hunger    * pull;
    state->drive_social    = state->drive_social    * keep + BASELINE_STATE.drive_social    * pull;
    state->drive_comfort   = state->drive_comfort   * keep + BASELINE_STATE.drive_comfort   * pull;

    state->emotion_pleasure = state->emotion_pleasure * keep + BASELINE_STATE.emotion_pleasure * pull;
    state->emotion_pain     = state->emotion_pain     * keep + BASELINE_STATE.emotion_pain     * pull;
    state->emotion_security = state->emotion_security * keep + BASELINE_STATE.emotion_security * pull;

    {
        SubTopology* emo = NULL;
        for (int t = 0; t < bs->master->sub_topo_count; t++) {
            SubTopology* s = bs->master->sub_topologies[t];
            if (s && s->type == TOPO_EMOTION) { emo = s; break; }
        }
        if (emo && emo->net && emo->net->node_count > 0) {
            float sample_sum = 0.0f;
            int sample_n = emo->net->node_count < 20 ? emo->net->node_count : 20;
            for (int i = 0; i < sample_n; i++) {
                int idx = local_rand(&bs->_rng_seed) % emo->net->node_count;
                ReasoningNode* n = emo->net->nodes[idx];
                if (n) sample_sum += n->valence;
            }
            float topo_val = sample_sum / sample_n;
            state->valence = state->valence * 0.9f + topo_val * 0.1f;
        }
    }
    state->valence *= 0.998f;
    if (fabsf(state->valence) < 0.001f) {
        state->valence = 0.0f;
    }

    state->explore_rate = 0.5f + state->valence * 0.5f;
    if (state->explore_rate < 0.05f) state->explore_rate = 0.05f;
    if (state->explore_rate > 0.95f) state->explore_rate = 0.95f;
}

static float circadian_activity(int hour) {
    double rad = (hour - 6) * 3.141592653589793 / 12.0;
    double raw = (sin(rad) + 1.0) / 2.0;
    return 0.1f + (float)raw * 0.9f;
}

static int current_hour(time_t t) {
    struct tm* tm_info = localtime(&t);
    return tm_info ? tm_info->tm_hour : 12;
}

static void* brainstem_loop(void* arg) {
    Brainstem* bs = (Brainstem*)arg;

    if (bs->verbose) printf("[脑干] 心跳启动, tick=%dms\n", bs->tick_interval_ms);

    while (bs->is_running) {
        bs->current_time = time(NULL);

        int hour = current_hour(bs->current_time);
        float circadian = circadian_activity(hour);

        int   actual_tick_ms     = (int)(bs->tick_interval_ms * (1.4f - 0.4f * circadian));
        float actual_decay       = bs->decay_per_tick + (1.0f - circadian) * 0.03f;
        float actual_spontaneous = bs->spontaneous_prob * circadian;
        int   dream_interval     = (int)(60.0f * (1.8f - 0.8f * circadian));
        int   selflearn_interval = (int)(120.0f * (2.0f - 1.0f * circadian));

        {
            static int last_logged_hour = -1;
            if (hour != last_logged_hour && bs->verbose) {
                const char* phase = (hour >= 0 && hour < 6)  ? "沉睡" :
                                    (hour >= 6 && hour < 10) ? "苏醒" :
                                    (hour >= 10 && hour < 14)? "活跃" :
                                    (hour >= 14 && hour < 18)? "午后" :
                                    (hour >= 18 && hour < 22)? "次活跃" : "入眠";
                printf("[脑干] %02d:00 -> %s期 (activity=%.2f)\n", hour, phase, circadian);
                last_logged_hour = hour;
            }
        }

        int slept = 0;
        while (slept < actual_tick_ms && bs->is_running) {
            msleep(100);
            slept += 100;
        }
        if (!bs->is_running) break;

        bs->tick_count++;

        pthread_rwlock_rdlock(&bs->master->rwlock);

        float total_nodes = 0.0f;
        for (int t = 0; t < bs->master->sub_topo_count; t++) {
            SubTopology* sub = bs->master->sub_topologies[t];
            if (sub && sub->net) total_nodes += (float)sub->net->node_count;
        }

        /* 稀疏衰减 */
        {
        int sample_count = (int)(total_nodes * 0.05f * circadian);
        if (sample_count < 100) sample_count = 100;
        for (int t = 0; t < bs->master->sub_topo_count; t++) {
            SubTopology* sub = bs->master->sub_topologies[t];
            if (!sub || !sub->net || sub->net->node_count == 0) continue;
            HuarongTopologyNet* net = sub->net;
            float sub_ratio = (float)net->node_count / (float)total_nodes;
            int sub_samples = (int)(sample_count * sub_ratio) + 1;
            if (sub_samples > net->node_count) sub_samples = net->node_count;

            for (int s = 0; s < sub_samples; s++) {
                int idx = local_rand(&bs->_rng_seed) % net->node_count;
                ReasoningNode* node = net->nodes[idx];
                if (!node || node->is_cooled) continue;

                int lock_idx = node->node_id & (PM_NODE_LOCK_COUNT - 1);
                pthread_mutex_lock(&net->node_locks[lock_idx]);
                if (node->activation <= PM_CLOCK_ACTIVATION_FLOOR) {
                    node->activation = 0.0f;
                } else {
                    node->activation *= actual_decay;
                    if (node->activation < PM_CLOCK_ACTIVATION_FLOOR) {
                        node->activation = 0.0f;
                    }
                }
                pthread_mutex_unlock(&net->node_locks[lock_idx]);
            }
        }
        }

        /* 自发激活 */
        {
        float expected = total_nodes * actual_spontaneous;
        int num_spontaneous = (int)expected;
        if ((float)local_rand(&bs->_rng_seed) / 32767.0f < (expected - num_spontaneous)) {
            num_spontaneous++;
        }
        for (int s = 0; s < num_spontaneous; s++) {
            int topo_idx = local_rand(&bs->_rng_seed) % bs->master->sub_topo_count;
            SubTopology* sub = bs->master->sub_topologies[topo_idx];
            if (!sub || !sub->net || sub->net->node_count == 0) continue;
            int ni = local_rand(&bs->_rng_seed) % sub->net->node_count;
            ReasoningNode* rn = sub->net->nodes[ni];
            if (!rn || rn->is_cooled) continue;

            int lock_idx = rn->node_id & (PM_NODE_LOCK_COUNT - 1);
            pthread_mutex_lock(&sub->net->node_locks[lock_idx]);
            float added = bs->spontaneous_strength * (0.5f + local_rand(&bs->_rng_seed) / 65535.0f);
            rn->activation += added;
            if (rn->activation > 1.0f) rn->activation = 1.0f;
            pthread_mutex_unlock(&sub->net->node_locks[lock_idx]);
        }
        }

        pthread_rwlock_unlock(&bs->master->rwlock);

        drift_cognitive_state(bs);

        /* ── 丘脑调度 ── （每30 tick一次，≈30秒） */
        if (bs->tick_count % 30 == 0 && bs->thalamus) {
            Thalamus* th = (Thalamus*)bs->thalamus;
            thalamus_set_circadian(th, circadian,
                (hour >= 0 && hour < 6) ? "sleep" :
                (hour >= 6 && hour < 10) ? "waking" :
                (hour >= 10 && hour < 14) ? "active" :
                (hour >= 14 && hour < 18) ? "afternoon" :
                (hour >= 18 && hour < 22) ? "evening" : "winding");
            thalamus_tick(th);
        }

        /* ── 感觉皮层 ── （传递丘脑 throttle） */
        if (bs->tick_count % 30 == 0 && bs->perception && bs->thalamus) {
            float p_throttle = thalamus_get_throttle((Thalamus*)bs->thalamus, THAL_PERCEPTION);
            perception_tick((Perception*)bs->perception, p_throttle);
        }

        if (bs->hippocampus && bs->tick_count % bs->consolidate_every_n_ticks == 0) {
            hippocampus_consolidate((Hippocampus*)bs->hippocampus);
        }

        if (bs->tick_count % dream_interval == 0) {
            DmnConfig dmn_cfg = DMN_DEFAULT_CONFIG;
            dmn_cfg.verbose = bs->verbose;
            float dmn_throttle = bs->thalamus ?
                thalamus_get_throttle((Thalamus*)bs->thalamus, THAL_DMN) : 1.0f;
            dmn_cycle(bs->master, bs->memory, &dmn_cfg, dmn_throttle);
        }

        if (bs->tick_count % selflearn_interval == 0 && bs->self_learner) {
            self_learner_cycle((SelfLearner*)bs->self_learner);
        }

        /* 冷节点冻结 */
        {
        int freeze_interval = (int)(600.0f * (2.0f - 1.0f * circadian));
        if (bs->tick_count % freeze_interval == 0 && bs->node_cache) {
            NodeCache* nc = (NodeCache*)bs->node_cache;
            int frozen = 0;
            pthread_rwlock_rdlock(&bs->master->rwlock);
            for (int t = 0; t < bs->master->sub_topo_count && frozen < 10; t++) {
                SubTopology* sub = bs->master->sub_topologies[t];
                if (!sub || !sub->net) continue;
                for (int attempt = 0; attempt < 50 && frozen < 10; attempt++) {
                    int idx = local_rand(&bs->_rng_seed) % sub->net->node_count;
                    ReasoningNode* node = sub->net->nodes[idx];
                    if (!node || node->is_cooled) continue;
                    if (node->connection_count > 0 && node->activation < 0.005f) {
                        node_cache_freeze(nc, sub->net, node);
                        frozen++;
                    }
                }
            }
            pthread_rwlock_unlock(&bs->master->rwlock);
            if (frozen > 0 && bs->verbose) {
                printf("[脑干] 本轮冻结 %d 个冷节点\n", frozen);
            }
        }
        }
    }

    if (bs->verbose) printf("[脑干] 已停止, tick=%d\n", bs->tick_count);
    return NULL;
}

Brainstem* brainstem_create(MasterTopology* master, MemorySystem* memory, CognitiveState* state) {
    if (!master) return NULL;

    Brainstem* bs = (Brainstem*)calloc(1, sizeof(Brainstem));
    if (!bs) return NULL;

    bs->master          = master;
    bs->memory          = memory;
    bs->cognitive_state = state;

    bs->tick_interval_ms       = PM_CLOCK_TICK_INTERVAL_MS;
    bs->decay_per_tick         = PM_CLOCK_DECAY_PER_TICK;
    bs->spontaneous_prob       = PM_CLOCK_SPONTANEOUS_PROB;
    bs->spontaneous_strength   = PM_CLOCK_SPONTANEOUS_STRENGTH;
    bs->consolidate_every_n_ticks = PM_CLOCK_CONSOLIDATE_INTERVAL;

    bs->is_running    = 0;
    bs->tick_count    = 0;
    bs->start_time    = time(NULL);
    bs->current_time  = bs->start_time;
    bs->self_learner  = self_learner_create(master, NULL);
    bs->_rng_seed     = (unsigned int)time(NULL) ^ (unsigned int)(uintptr_t)bs;

    return bs;
}

void brainstem_start(Brainstem* bs) {
    if (!bs || bs->is_running) return;
    bs->is_running = 1;
    int ret = pthread_create(&bs->thread, NULL, brainstem_loop, (void*)bs);
    if (ret != 0) {
        bs->is_running = 0;
        fprintf(stderr, "[脑干] 线程创建失败 (errno=%d)\n", ret);
        return;
    }
    if (bs->verbose) printf("[脑干] 线程已启动\n");
}

void brainstem_stop(Brainstem* bs) {
    if (!bs) return;
    int was_running = __sync_lock_test_and_set(&bs->is_running, 0);
    if (!was_running) return;
    pthread_join(bs->thread, NULL);
    if (bs->verbose) printf("[脑干] 线程已停止 (tick=%d)\n", bs->tick_count);
}

void brainstem_destroy(Brainstem* bs) {
    if (!bs) return;
    if (bs->is_running) brainstem_stop(bs);
    if (bs->self_learner) {
        self_learner_destroy((SelfLearner*)bs->self_learner);
        bs->self_learner = NULL;
    }
    free(bs);
}

int brainstem_tick_count(Brainstem* bs) {
    return bs ? bs->tick_count : 0;
}

void brainstem_set_verbose(Brainstem* bs, int verbose) {
    if (bs) bs->verbose = verbose;
}

const char* brainstem_get_real_time(Brainstem* bs, char* buf, int size) {
    if (!bs || !buf || size < 16) return NULL;
    struct tm* tm_info = localtime(&bs->current_time);
    if (!tm_info) return NULL;
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", tm_info);
    return buf;
}

long brainstem_get_uptime(Brainstem* bs) {
    if (!bs) return 0;
    return (long)(bs->current_time - bs->start_time);
}

float brainstem_get_circadian(Brainstem* bs) {
    if (!bs) return 0.5f;
    return circadian_activity(current_hour(bs->current_time));
}

const char* brainstem_get_circadian_phase(Brainstem* bs) {
    if (!bs) return "unknown";
    int hour = current_hour(bs->current_time);
    if (hour >= 0  && hour < 6)  return "sleep";
    if (hour >= 6  && hour < 10) return "waking";
    if (hour >= 10 && hour < 14) return "active";
    if (hour >= 14 && hour < 18) return "afternoon";
    if (hour >= 18 && hour < 22) return "evening";
    return "winding";
}

void brainstem_set_node_cache(Brainstem* bs, void* node_cache) {
    if (bs) bs->node_cache = node_cache;
}

void brainstem_set_thalamus(Brainstem* bs, void* thalamus) {
    if (bs) bs->thalamus = thalamus;
}

void brainstem_set_perception(Brainstem* bs, void* perception) {
    if (bs) bs->perception = perception;
}

void brainstem_set_hippocampus(Brainstem* bs, void* hippocampus) {
    if (bs) bs->hippocampus = hippocampus;
}
