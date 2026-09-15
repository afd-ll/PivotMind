/**
 * @file thalamus.c
 * @brief 丘脑调度器实现 — 资源门控 + 脑区信号总线
 */

#include "thalamus.h"
#include "error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ================================================================
 *  子系统名称（调试用）
 * ================================================================ */

static const char* SYS_NAMES[THAL_SUBSYSTEM_COUNT] __attribute__((unused)) = {
    "前额叶(对话)",
    "海马体(学习)",
    "默认模式网络(梦境)",
    "感知(联网)",
    "布罗卡区(句式)",
    "小脑(微调)",
    "杏仁核(情绪)",
    "前额叶执行器(推理)",
    "下丘脑(动机)"
};

/* ================================================================
 *  创建 / 销毁
 * ================================================================ */

Thalamus* thalamus_create(void) {
    Thalamus* th = (Thalamus*)calloc(1, sizeof(Thalamus));
    if (!th) return NULL;

    /* 默认：所有子系统全速 */
    for (int i = 0; i < THAL_SUBSYSTEM_COUNT; i++) {
        th->throttle[i] = 1.0f;
        th->enabled[i] = 1;   /* 默认全部启用 */
    }

    /* 可配置默认值 */
    th->dialog_window_seconds  = 3600;
    th->idle_threshold_dialogs = 2;
    th->busy_threshold_dialogs = 10;
    th->learner_slow_ms        = 5000.0f;
    th->cpu_high_threshold     = 0.85f;

    th->circadian = 0.5f;
    th->circadian_phase = "active";

    /* 信号队列初始化（head=tail=count=0 已在 calloc 中清零） */
    /* 脑区指针、工具指针已在 calloc 中清零 */

    /* 小脑保护 */
    th->cerebellum_protect = 1.0f;

    /* 自主神经系统默认值（静息态） */
    th->autonomic.sympathetic     = 0.4f;
    th->autonomic.parasympathetic = 0.6f;
    th->autonomic.arousal         = 0.5f;

    /* 初始化决策理由 */
    snprintf(th->last_reason, sizeof(th->last_reason), "init");

    pthread_mutex_init(&th->lock, NULL);
    LOG_INFO("[丘脑] 调度器+信号总线就绪 (子系统=%d, 信号队列=%d)",
           THAL_SUBSYSTEM_COUNT, THAL_SIGNAL_QUEUE_SIZE);
    return th;
}

void thalamus_destroy(Thalamus* th) {
    if (!th) return;
    pthread_mutex_destroy(&th->lock);
    free(th);
}

/* ================================================================
 *  脑区注册 / 查询
 * ================================================================ */

void thalamus_register_region(Thalamus* th, ThalamusSubsystem sys, void* ptr) {
    if (!th || sys < 0 || sys >= THAL_SUBSYSTEM_COUNT) return;
    pthread_mutex_lock(&th->lock);
    th->region_ptrs[sys] = ptr;
    pthread_mutex_unlock(&th->lock);
}

void* thalamus_get_region(Thalamus* th, ThalamusSubsystem sys) {
    if (!th || sys < 0 || sys >= THAL_SUBSYSTEM_COUNT) return NULL;
    void* ptr;
    pthread_mutex_lock(&th->lock);
    ptr = th->region_ptrs[sys];
    pthread_mutex_unlock(&th->lock);
    return ptr;
}

void thalamus_register_utility(Thalamus* th, ThalamusUtilitySlot slot, void* ptr) {
    if (!th || slot < 0 || slot >= THAL_UTIL_COUNT) return;
    pthread_mutex_lock(&th->lock);
    th->utility_ptrs[slot] = ptr;
    pthread_mutex_unlock(&th->lock);
}

void* thalamus_get_utility(Thalamus* th, ThalamusUtilitySlot slot) {
    if (!th || slot < 0 || slot >= THAL_UTIL_COUNT) return NULL;
    void* ptr;
    pthread_mutex_lock(&th->lock);
    ptr = th->utility_ptrs[slot];
    pthread_mutex_unlock(&th->lock);
    return ptr;
}

void thalamus_set_partition(Thalamus* th, ThalamusSubsystem sys,
                             const int* topo_ids, int count) {
    if (!th || sys < 0 || sys >= THAL_SUBSYSTEM_COUNT) return;
    if (count > THAL_MAX_OWNED_TOPOS) count = THAL_MAX_OWNED_TOPOS;
    pthread_mutex_lock(&th->lock);
    th->partitions[sys].count = count;
    for (int i = 0; i < count; i++) {
        th->partitions[sys].topo_ids[i] = topo_ids[i];
    }
    pthread_mutex_unlock(&th->lock);
}

int thalamus_owns_topo(Thalamus* th, ThalamusSubsystem sys, int topo_type) {
    if (!th || sys < 0 || sys >= THAL_SUBSYSTEM_COUNT) return 0;
    pthread_mutex_lock(&th->lock);
    int owned = 0;
    BrainTopoPartition* p = &th->partitions[sys];
    for (int i = 0; i < p->count; i++) {
        if (p->topo_ids[i] == topo_type) { owned = 1; break; }
    }
    pthread_mutex_unlock(&th->lock);
    return owned;
}

/* ================================================================
 *  信号总线
 * ================================================================ */

int thalamus_send_signal(Thalamus* th, int target, const BrainSignal* sig) {
    if (!th || !sig) return 0;

    pthread_mutex_lock(&th->lock);
    int sent = 0;

    if (target >= 0 && target <= THAL_SELF_QUEUE) {
        /* 定向发送（含丘脑自用槽 THAL_SELF_QUEUE） */
        if (th->signal_queues[target].count < THAL_SIGNAL_QUEUE_SIZE) {
            int tail = th->signal_queues[target].tail;
            th->signal_queues[target].slots[tail] = *sig;
            th->signal_queues[target].tail = (tail + 1) % THAL_SIGNAL_QUEUE_SIZE;
            th->signal_queues[target].count++;
            sent = 1;
        }
    } else {
        /* 广播模式 */
        for (int r = 0; r < THAL_SUBSYSTEM_COUNT; r++) {
            if (th->signal_queues[r].count < THAL_SIGNAL_QUEUE_SIZE) {
                int tail = th->signal_queues[r].tail;
                th->signal_queues[r].slots[tail] = *sig;
                th->signal_queues[r].tail = (tail + 1) % THAL_SIGNAL_QUEUE_SIZE;
                th->signal_queues[r].count++;
                sent++;
            }
        }
    }

    pthread_mutex_unlock(&th->lock);
    return sent;
}

int thalamus_recv_signal(Thalamus* th, int region, BrainSignal* out, int max) {
    /* 边界与 thalamus_send_signal 对称：允许 region == THAL_SELF_QUEUE（丘脑自用槽） */
    if (!th || !out || max <= 0 || region < 0 || region > THAL_SELF_QUEUE) return 0;

    pthread_mutex_lock(&th->lock);

    int received = 0;
    while (received < max && th->signal_queues[region].count > 0) {
        int head = th->signal_queues[region].head;
        out[received] = th->signal_queues[region].slots[head];
        th->signal_queues[region].head = (head + 1) % THAL_SIGNAL_QUEUE_SIZE;
        th->signal_queues[region].count--;
        received++;
    }

    pthread_mutex_unlock(&th->lock);
    return received;
}

int thalamus_has_signal(Thalamus* th, int region) {
    /* 同上：has 也必须认自用槽，否则「发得进、收不出」 */
    if (!th || region < 0 || region > THAL_SELF_QUEUE) return 0;
    int count;
    pthread_mutex_lock(&th->lock);
    count = th->signal_queues[region].count;
    pthread_mutex_unlock(&th->lock);
    return count;
}

int thalamus_send_feedback(Thalamus* th, int source,
                            int consolidated, int searched, int dreamed) {
    BrainSignal sig;
    memset(&sig, 0, sizeof(sig));
    sig.type   = THAL_SIG_FEEDBACK_REPORT;
    sig.source = source;
    /* v0.5.39: 由「广播(-1)」改为「定向投给丘脑自用信箱」—— 反馈本就是
     * 上报给丘脑的，广播到各脑区既无意义、又会挤占其仅 16 格的队列。 */
    sig.target = THAL_SELF_QUEUE;
    sig.data.feedback.consolidated = consolidated;
    sig.data.feedback.searched     = searched;
    sig.data.feedback.dreamed      = dreamed;
    return thalamus_send_signal(th, THAL_SELF_QUEUE, &sig);
}

/* ================================================================
 *  感知更新
 * ================================================================ */

void thalamus_update_sensors(Thalamus* th, int dialogs_1h, float learner_ms,
                              int hot_nodes, float cpu) {
    if (!th) return;
    pthread_mutex_lock(&th->lock);
    if (dialogs_1h >= 0) th->dialogs_per_hour = dialogs_1h;
    if (learner_ms  >= 0) th->learner_load_ms = learner_ms;
    if (hot_nodes   >= 0) th->hot_node_count = hot_nodes;
    if (cpu         >= 0) th->cpu_usage = cpu;
    pthread_mutex_unlock(&th->lock);
}

void thalamus_set_circadian(Thalamus* th, float circadian, const char* phase) {
    if (!th) return;
    pthread_mutex_lock(&th->lock);
    th->circadian = circadian;
    if (phase) th->circadian_phase = phase;
    pthread_mutex_unlock(&th->lock);
}

void thalamus_record_dialog(Thalamus* th) {
    if (!th) return;
    pthread_mutex_lock(&th->lock);
    th->dialogs_total++;
    th->dialogs_per_hour++;
    pthread_mutex_unlock(&th->lock);
}

/* ================================================================
 *  决策引擎
 * ================================================================ */

static float compute_busyness(Thalamus* th) {
    if (th->dialogs_per_hour <= th->idle_threshold_dialogs) return 0.0f;
    if (th->dialogs_per_hour >= th->busy_threshold_dialogs) return 1.0f;
    float range = (float)(th->busy_threshold_dialogs - th->idle_threshold_dialogs);
    return (float)(th->dialogs_per_hour - th->idle_threshold_dialogs) / range;
}

/* g_last_reason 已移至 Thalamus.last_reason 缓冲区，线程安全 */

/* ================================================================
 *  工具函数
 * ================================================================ */

/**
 * sigmoid 反馈曲线 — 阈上响应平滑饱和
 * @param work_count 实际工作量
 * @param threshold  阈值（低于此值几乎无抑制）
 * @param steepness  曲线陡度（越大越"开关"）
 * @param max_reduction  最大抑制比例 (0.0~1.0)
 * @return 抑制比例
 */
static float sigmoid_feedback(float work_count, float threshold,
                               float steepness, float max_reduction) {
    if (work_count <= 0) return 0.0f;
    float x = work_count - threshold;
    if (x < 0) x = 0;
    return max_reduction / (1.0f + expf(-steepness * x));
}

/* ================================================================
 *  主调度 tick
 * ================================================================ */

void thalamus_tick(Thalamus* th) {
    if (!th) return;

    pthread_mutex_lock(&th->lock);
    th->tick_count++;

    float busy = compute_busyness(th);

    /* ── 自主神经系统更新 ── */
    {
        /* 交感 = 昼夜活跃度 + 忙碌度加成 */
        th->autonomic.sympathetic = th->circadian * 0.7f + busy * 0.3f;
        if (th->autonomic.sympathetic > 1.0f) th->autonomic.sympathetic = 1.0f;

        /* 副交感 = 休息期占主导 */
        th->autonomic.parasympathetic = (1.0f - th->circadian) * 0.8f
                                        + (1.0f - busy) * 0.2f;
        if (th->autonomic.parasympathetic > 1.0f) th->autonomic.parasympathetic = 1.0f;

        /* 唤醒水平 = 交感主导时高，副交感主导时低 */
        th->autonomic.arousal = (th->autonomic.sympathetic
                                 + 1.0f - th->autonomic.parasympathetic) * 0.5f;
        if (th->autonomic.arousal < 0.0f) th->autonomic.arousal = 0.0f;
        if (th->autonomic.arousal > 1.0f) th->autonomic.arousal = 1.0f;
    }

    /* ── 决策逻辑（基准调度） ── */

    if (busy > 0.6f) {
        th->throttle[THAL_PREFRONTAL]  = 1.0f;
        th->throttle[THAL_HIPPOCAMPUS] = 0.3f - 0.2f * busy;
        th->throttle[THAL_DMN]         = 0.0f;
        th->throttle[THAL_PERCEPTION]  = 0.0f;
        th->throttle[THAL_BROCA]       = 1.0f;
        th->throttle[THAL_CEREBELLUM]  = 0.2f;
        th->throttle[THAL_AMYGDALA]    = 0.5f;
        th->active_subsystem = THAL_PREFRONTAL;
        snprintf(th->last_reason, sizeof(th->last_reason), "对话繁忙，抑制非必要子系统");

    } else if (busy > 0.2f) {
        th->throttle[THAL_PREFRONTAL]  = 1.0f;
        th->throttle[THAL_HIPPOCAMPUS] = 0.7f;
        th->throttle[THAL_DMN]         = 0.5f;
        th->throttle[THAL_PERCEPTION]  = 0.3f;
        th->throttle[THAL_BROCA]       = 0.8f;
        th->throttle[THAL_CEREBELLUM]  = 0.5f;
        th->throttle[THAL_AMYGDALA]    = 0.7f;
        th->active_subsystem = THAL_PREFRONTAL;
        snprintf(th->last_reason, sizeof(th->last_reason), "正常负载，各系统降速运行");

    } else {
        if (th->cooled_node_count > 100) {
            th->throttle[THAL_HIPPOCAMPUS] = 0.9f;
            th->throttle[THAL_PERCEPTION]  = 0.6f;
            th->throttle[THAL_DMN]         = 0.2f;
            th->throttle[THAL_PREFRONTAL]  = 0.1f;
            th->throttle[THAL_AMYGDALA]    = 0.4f;
            th->active_subsystem = THAL_HIPPOCAMPUS;
            snprintf(th->last_reason, sizeof(th->last_reason), "空闲+冷却节点积压，优先巩固+联网查证");

        } else if (th->learner_load_ms > th->learner_slow_ms) {
            th->throttle[THAL_HIPPOCAMPUS] = 1.0f;
            th->throttle[THAL_DMN]         = 0.1f;
            th->throttle[THAL_PERCEPTION]  = 0.3f;
            th->throttle[THAL_PREFRONTAL]  = 0.1f;
            th->throttle[THAL_AMYGDALA]    = 0.4f;
            th->active_subsystem = THAL_HIPPOCAMPUS;
            snprintf(th->last_reason, sizeof(th->last_reason), "空闲+训练积压，专注学习");

        } else {
            th->throttle[THAL_DMN]         = 0.8f;
            th->throttle[THAL_PERCEPTION]  = 0.7f;
            th->throttle[THAL_HIPPOCAMPUS] = 0.4f;
            th->throttle[THAL_PREFRONTAL]  = 0.1f;
            th->throttle[THAL_BROCA]       = 0.3f;
            th->throttle[THAL_CEREBELLUM]  = 0.3f;
            th->throttle[THAL_AMYGDALA]    = 0.6f;
            th->active_subsystem = THAL_DMN;
            snprintf(th->last_reason, sizeof(th->last_reason), "完全空闲，梦境+好奇探索");
        }
    }

    /* ── 消费「丘脑自用信箱」中的反馈，更新 throttle（sigmoid 曲线） ──
     * v0.5.39: 原实现 `for (r < THAL_SUBSYSTEM_COUNT)` 遍历全部脑区队列并把
     * 它们**清空**，导致各脑区的定向信号（如海马体→感知区的 CONS_NODE）在
     * 收件方读取之前即被丢弃 —— 这也是 recv_signal 全仓零调用的原因之一。
     * 现只消费 THAL_SELF_QUEUE，各脑区队列原样保留给对应脑区自行收取。 */
    while (th->signal_queues[THAL_SELF_QUEUE].count > 0) {
        int h = th->signal_queues[THAL_SELF_QUEUE].head;
        BrainSignal sig = th->signal_queues[THAL_SELF_QUEUE].slots[h];
        th->signal_queues[THAL_SELF_QUEUE].head = (h + 1) % THAL_SIGNAL_QUEUE_SIZE;
        th->signal_queues[THAL_SELF_QUEUE].count--;

        if (sig.type == THAL_SIG_FEEDBACK_REPORT) {
            if (sig.data.feedback.consolidated > 0)
                th->fb_hippo_consolidated += sig.data.feedback.consolidated;
            if (sig.data.feedback.searched > 0)
                th->fb_percept_searched += sig.data.feedback.searched;
            if (sig.data.feedback.dreamed > 0)
                th->fb_dmn_dreamed += sig.data.feedback.dreamed;
        }
        /* ── A12 步2：前额叶 / 视觉媒体事件（步1 已把它们从「无主广播」改为投本信箱） ──
         * 显式列举而非范围判断：将来在枚举中间插值也不会误伤。 */
        else if (sig.type == THAL_SIG_REASONING_START || sig.type == THAL_SIG_REASONING_END ||
                 sig.type == THAL_SIG_SUBGOAL_START   || sig.type == THAL_SIG_SUBGOAL_RESULT ||
                 sig.type == THAL_SIG_IDEA_PROPOSED   || sig.type == THAL_SIG_IDEA_SELECTED) {
            th->fb_pfe_reasoning++;
        }
        else if (sig.type == THAL_SIG_VISUAL_FRAME || sig.type == THAL_SIG_CROSS_MODAL_EDGE ||
                 sig.type == THAL_SIG_MEDIA_FILE_DONE) {
            th->fb_vc_media++;
        }
    }

    /* ── 反馈闭环（sigmoid 比例调节替代固定阈值） ── */
    /* ⚠️ A12 步2 修的既有 bug：原「正反馈恢复」段的 any_feedback 判定写在**计数清零之后**
     * ⇒ 恒为 0 ⇒ idle_ticks 每 tick 都涨、「连续无产出才恢复」退化成「每 tick 都恢复」
     * ⇒ 抑制几 tick 就被抹平。改为在闭环块内标记、块外使用。 */
    int had_feedback = 0;
    {
        /* 海马体巩固：阈值5，每多5个巩固 sigmoid 上升，最大抑制50% */
        if (th->fb_hippo_consolidated > 0) {
            float reduction = sigmoid_feedback(
                (float)th->fb_hippo_consolidated, 5.0f, 0.3f, 0.50f);
            th->throttle[THAL_HIPPOCAMPUS] *= (1.0f - reduction);
            if (th->throttle[THAL_HIPPOCAMPUS] < 0.10f)
                th->throttle[THAL_HIPPOCAMPUS] = 0.10f;
            th->fb_hippo_consolidated = 0;
            had_feedback = 1;
        }

        /* 感知皮层搜索：阈值3，最大抑制35% */
        if (th->fb_percept_searched > 0) {
            float reduction = sigmoid_feedback(
                (float)th->fb_percept_searched, 3.0f, 0.4f, 0.35f);
            th->throttle[THAL_PERCEPTION] *= (1.0f - reduction);
            if (th->throttle[THAL_PERCEPTION] < 0.05f)
                th->throttle[THAL_PERCEPTION] = 0.05f;
            th->fb_percept_searched = 0;
            had_feedback = 1;
        }

        /* DMN 梦境：阈值10，最大抑制55% */
        if (th->fb_dmn_dreamed > 0) {
            float reduction = sigmoid_feedback(
                (float)th->fb_dmn_dreamed, 10.0f, 0.25f, 0.55f);
            th->throttle[THAL_DMN] *= (1.0f - reduction);
            if (th->throttle[THAL_DMN] < 0.05f)
                th->throttle[THAL_DMN] = 0.05f;
            th->fb_dmn_dreamed = 0;
            had_feedback = 1;
        }

        /* ── A12 步2 新增：前额叶推理活跃 ⇒ 抑制 DMN + 感知 ──
         * 与「对话繁忙」同向：推理时不该做梦、也不该闲逛（真实脑里任务正激活与 DMN 相拮抗）。
         * 阈值3：一轮内 ≥3 个推理事件才起效，避免单次噪声触发。 */
        if (th->fb_pfe_reasoning > 0) {
            float reduction = sigmoid_feedback(
                (float)th->fb_pfe_reasoning, 3.0f, 0.3f, 0.35f);
            th->throttle[THAL_DMN] *= (1.0f - reduction);
            if (th->throttle[THAL_DMN] < 0.05f)
                th->throttle[THAL_DMN] = 0.05f;
            th->throttle[THAL_PERCEPTION] *= (1.0f - reduction);
            if (th->throttle[THAL_PERCEPTION] < 0.05f)
                th->throttle[THAL_PERCEPTION] = 0.05f;
            th->fb_pfe_reasoning = 0;
            had_feedback = 1;
        }

        /* ── A12 步2 新增：视觉/媒体处理活跃 ⇒ 抑制 DMN ──
         * 媒体任务在跑就别做梦（资源让给处理链路）；阈值2（媒体事件本就更稀）。 */
        if (th->fb_vc_media > 0) {
            float reduction = sigmoid_feedback(
                (float)th->fb_vc_media, 2.0f, 0.35f, 0.25f);
            th->throttle[THAL_DMN] *= (1.0f - reduction);
            if (th->throttle[THAL_DMN] < 0.05f)
                th->throttle[THAL_DMN] = 0.05f;
            th->fb_vc_media = 0;
            had_feedback = 1;
        }
    }

    /* ── 正反馈恢复：连续无产出→缓慢恢复 throttle ── */
    {
        int any_feedback = had_feedback;
        if (any_feedback == 0) {
            th->idle_ticks++;
            float restore_rate = 0.005f * (1.0f + th->idle_ticks * 0.01f);
            if (restore_rate > 0.05f) restore_rate = 0.05f;
            for (int i = 0; i < THAL_SUBSYSTEM_COUNT; i++) {
                if (th->throttle[i] < 1.0f) {
                    th->throttle[i] += restore_rate;
                    if (th->throttle[i] > 1.0f)
                        th->throttle[i] = 1.0f;
                }
            }
        } else {
            th->idle_ticks = 0;
        }
    }

    /* ── 小脑保护回灌（与调度 throttle 取 min，硬件级限速） ── */
    if (th->cerebellum_protect < 1.0f) {
        float cb_safety = th->cerebellum_protect;
        for (int i = 0; i < THAL_SUBSYSTEM_COUNT; i++) {
            if (th->throttle[i] > cb_safety)
                th->throttle[i] = cb_safety;
        }
        snprintf(th->last_reason, sizeof(th->last_reason), "小脑保护激活，全局限速");
    }

    /* ── 自主神经调制 ── */
    if (th->autonomic.sympathetic > th->autonomic.parasympathetic + 0.1f) {
        /* 交感主导：加强探索（感知/DMN），减弱巩固（海马体） */
        float boost = th->autonomic.sympathetic * 0.25f;
        th->throttle[THAL_PERCEPTION] *= (1.0f + boost);
        if (th->throttle[THAL_PERCEPTION] > 1.0f)
            th->throttle[THAL_PERCEPTION] = 1.0f;
        th->throttle[THAL_DMN] *= (1.0f + boost * 0.5f);
        if (th->throttle[THAL_DMN] > 1.0f)
            th->throttle[THAL_DMN] = 1.0f;
        th->throttle[THAL_HIPPOCAMPUS] *= (1.0f - boost * 0.5f);
        if (th->throttle[THAL_HIPPOCAMPUS] < 0.05f)
            th->throttle[THAL_HIPPOCAMPUS] = 0.05f;

    } else if (th->autonomic.parasympathetic > th->autonomic.sympathetic + 0.1f) {
        /* 副交感主导：加强巩固（海马体），减弱探索 */
        float boost = th->autonomic.parasympathetic * 0.25f;
        th->throttle[THAL_HIPPOCAMPUS] *= (1.0f + boost);
        if (th->throttle[THAL_HIPPOCAMPUS] > 1.0f)
            th->throttle[THAL_HIPPOCAMPUS] = 1.0f;
        th->throttle[THAL_PERCEPTION] *= (1.0f - boost * 0.4f);
        th->throttle[THAL_DMN] *= (1.0f - boost * 0.3f);
    }

    /* ── 昼夜调制（睡眠期全局降速，保留前额叶最小活性） ── */
    if (th->circadian < 0.3f) {
        float sleep_factor = th->circadian * 3.0f;
        for (int i = 0; i < THAL_SUBSYSTEM_COUNT; i++) {
            if (i == THAL_PREFRONTAL) continue;
            th->throttle[i] *= sleep_factor;
        }
    }

    /* ── 记录历史 ── */
    if (th->throttle_history_pos < 32) {
        for (int i = 0; i < THAL_SUBSYSTEM_COUNT; i++) {
            th->throttle_history[i][th->throttle_history_pos] = th->throttle[i];
        }
        th->throttle_history_pos++;
    } else {
        for (int i = 0; i < THAL_SUBSYSTEM_COUNT; i++) {
            th->throttle_history[i][th->tick_count % 32] = th->throttle[i];
        }
    }

    /* 各脑区通过 thalamus_get_throttle() 自行查询更新后的 throttle 值 */
    pthread_mutex_unlock(&th->lock);
}

/* ================================================================
 *  查询
 * ================================================================ */

float thalamus_get_throttle(Thalamus* th, ThalamusSubsystem subsystem) {
    if (!th || subsystem < 0 || subsystem >= THAL_SUBSYSTEM_COUNT) return 1.0f;
    float val;
    pthread_mutex_lock(&th->lock);
    val = th->throttle[subsystem];
    pthread_mutex_unlock(&th->lock);
    return val;
}

void thalamus_enable_region(Thalamus* th, ThalamusSubsystem subsystem, int enabled) {
    if (!th || subsystem < 0 || subsystem >= THAL_SUBSYSTEM_COUNT) return;
    pthread_mutex_lock(&th->lock);
    th->enabled[subsystem] = enabled ? 1 : 0;
    if (!enabled) th->throttle[subsystem] = 0.0f;  /* 禁用时 throttle 归零 */
    pthread_mutex_unlock(&th->lock);
}

int thalamus_is_region_enabled(Thalamus* th, ThalamusSubsystem subsystem) {
    if (!th || subsystem < 0 || subsystem >= THAL_SUBSYSTEM_COUNT) return 0;
    int val;
    pthread_mutex_lock(&th->lock);
    val = th->enabled[subsystem];
    pthread_mutex_unlock(&th->lock);
    return val;
}

const char* thalamus_phase_description(Thalamus* th) {
    if (!th) return "unknown";
    float busy;
    pthread_mutex_lock(&th->lock);
    busy = compute_busyness(th);
    pthread_mutex_unlock(&th->lock);

    if (busy > 0.6f) return "对话繁忙";
    if (busy > 0.2f) return "正常运行";
    return "空闲探索";
}

const char* thalamus_decision_reason(Thalamus* th) {
    if (!th) return "unknown";
    pthread_mutex_lock(&th->lock);
    const char* ret = th->last_reason;
    pthread_mutex_unlock(&th->lock);
    return ret;
}

/* ── 反馈上报（旧兼容） ── */
void thalamus_report(Thalamus* th, int consolidated, int searched, int dreamed) {
    thalamus_send_feedback(th, -1, consolidated, searched, dreamed);
}

/* ── 小脑保护回灌 ── */
void thalamus_set_cerebellum_protect(Thalamus* th, float protect) {
    if (!th) return;
    pthread_mutex_lock(&th->lock);
    th->cerebellum_protect = protect;
    pthread_mutex_unlock(&th->lock);
}
