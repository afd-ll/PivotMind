/**
 * @file transition_model.c
 * @brief [PROB-ASSIST v0.6.11] 词转移概率模型实现 —— 见 transition_model.h
 *
 * 结构：开放寻址哈希（keys/counts 并行数组，capacity 2 的幂，线性探测，
 * 空槽 = counts[i]==0），整表一把互斥锁。观测路径（learn worker）与
 * 读取路径（生成端竞争）并发 ⇒ 锁内只放 O(1) 查找/插入，采样/打分持锁
 * 全表扫（容量万级，仅竞争分支触发，毫秒以下）。
 */

#include "transition_model.h"
#include "pivotmind_paths.h"
#include "error.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <pthread.h>

#define TM_MAGIC     0x504D544Du   /* "PMTM" */
#define TM_VERSION   1u
#define TM_LAMBDA    0.1f          /* add-λ 平滑 */
#define TM_K         64.0f         /* 平滑分母里的假定候选池大小（常数） */
#define TM_INIT_CAP  1024          /* 初始桶数（2 的幂） */
#define TM_LOAD_NUM  7
#define TM_LOAD_DEN  10            /* load factor 0.7 扩容 */

struct TransitionModel {
    uint64_t* keys;       /* key = (uint64)(uint32)prev << 32 | (uint32)next */
    uint32_t* counts;
    int       capacity;   /* 2 的幂 */
    int       size;
    pthread_mutex_t lock;
};

/* 64 位乘散列（splitmix64 终混），取高 log2(cap) 位 */
static uint64_t tm_hash_key(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

static uint64_t tm_make_key(int32_t prev_id, int32_t next_id) {
    return ((uint64_t)(uint32_t)prev_id << 32) | (uint64_t)(uint32_t)next_id;
}

/* 查找：返回槽位下标（counts>0 为命中）；not_found 时返回应插入的空槽（或 -1 表满）。
 * 调用方必须已持锁。 */
static int tm_find_slot(const TransitionModel* tm, uint64_t key, int* found) {
    uint64_t mask = (uint64_t)(tm->capacity - 1);
    int idx = (int)(tm_hash_key(key) & mask);
    int first_empty = -1;
    for (int probe = 0; probe < tm->capacity; probe++) {
        int i = (int)(((uint64_t)idx + (uint64_t)probe) & mask);
        if (tm->counts[i] == 0) {
            if (first_empty < 0) first_empty = i;
            /* 线性探测遇到空槽即说明 key 不在表中（无删除操作 ⇒ 无墓碑） */
            *found = 0;
            return first_empty;
        }
        if (tm->keys[i] == key) {
            *found = 1;
            return i;
        }
    }
    *found = 0;
    return first_empty;  /* 全表无空槽（load factor 有界，理论不达） */
}

/* 扩容 2 倍并全量 rehash。调用方必须已持锁。 */
static int tm_grow(TransitionModel* tm) {
    int new_cap = tm->capacity * 2;
    uint64_t* new_keys = (uint64_t*)malloc(sizeof(uint64_t) * (size_t)new_cap);
    uint32_t* new_counts = (uint32_t*)calloc((size_t)new_cap, sizeof(uint32_t));
    if (!new_keys || !new_counts) {
        free(new_keys);
        free(new_counts);
        return -1;
    }
    uint64_t mask = (uint64_t)(new_cap - 1);
    for (int i = 0; i < tm->capacity; i++) {
        if (tm->counts[i] == 0) continue;
        uint64_t k = tm->keys[i];
        int idx = (int)(tm_hash_key(k) & mask);
        for (int probe = 0; probe < new_cap; probe++) {
            int j = (int)(((uint64_t)idx + (uint64_t)probe) & mask);
            if (new_counts[j] == 0) {
                new_keys[j] = k;
                new_counts[j] = tm->counts[i];
                break;
            }
        }
    }
    free(tm->keys);
    free(tm->counts);
    tm->keys = new_keys;
    tm->counts = new_counts;
    tm->capacity = new_cap;
    return 0;
}

TransitionModel* tm_create(void) {
    TransitionModel* tm = (TransitionModel*)calloc(1, sizeof(TransitionModel));
    if (!tm) return NULL;
    tm->keys = (uint64_t*)malloc(sizeof(uint64_t) * TM_INIT_CAP);
    tm->counts = (uint32_t*)calloc(TM_INIT_CAP, sizeof(uint32_t));
    if (!tm->keys || !tm->counts) {
        free(tm->keys);
        free(tm->counts);
        free(tm);
        return NULL;
    }
    tm->capacity = TM_INIT_CAP;
    tm->size = 0;
    pthread_mutex_init(&tm->lock, NULL);
    return tm;
}

void tm_destroy(TransitionModel* tm) {
    if (!tm) return;
    free(tm->keys);
    free(tm->counts);
    pthread_mutex_destroy(&tm->lock);
    free(tm);
}

int tm_observe_pair(TransitionModel* tm, int32_t prev_id, int32_t next_id) {
    if (!tm || prev_id < 0 || next_id < 0 || prev_id == next_id) return 0;
    uint64_t key = tm_make_key(prev_id, next_id);

    pthread_mutex_lock(&tm->lock);
    if ((tm->size + 1) * TM_LOAD_DEN >= tm->capacity * TM_LOAD_NUM) {
        if (tm_grow(tm) != 0) {
            pthread_mutex_unlock(&tm->lock);
            return -1;
        }
    }
    int found = 0;
    int slot = tm_find_slot(tm, key, &found);
    if (found) {
        if (tm->counts[slot] < 0xFFFFFFFFu) tm->counts[slot]++;
    } else if (slot >= 0) {
        tm->keys[slot] = key;
        tm->counts[slot] = 1;
        tm->size++;
    } else {
        pthread_mutex_unlock(&tm->lock);
        return -1;
    }
    pthread_mutex_unlock(&tm->lock);
    return 0;
}

int tm_observe_run(TransitionModel* tm, const int32_t* ids, int n) {
    if (!tm || !ids || n < 2) return 0;
    for (int i = 0; i < n - 1; i++) {
        if (tm_observe_pair(tm, ids[i], ids[i + 1]) != 0) return -1;
    }
    return 0;
}

uint32_t tm_out_count(const TransitionModel* tm, int32_t prev_id) {
    if (!tm || prev_id < 0) return 0;
    uint64_t hi = (uint64_t)(uint32_t)prev_id << 32;
    uint32_t total = 0;
    pthread_mutex_lock((pthread_mutex_t*)&tm->lock);
    for (int i = 0; i < tm->capacity; i++) {
        if (tm->counts[i] != 0 && (tm->keys[i] & 0xFFFFFFFF00000000ULL) == hi) {
            total += tm->counts[i];
        }
    }
    pthread_mutex_unlock((pthread_mutex_t*)&tm->lock);
    return total;
}

float tm_prob(const TransitionModel* tm, int32_t prev_id, int32_t next_id) {
    if (!tm || prev_id < 0 || next_id < 0) return 0.0f;
    uint64_t key = tm_make_key(prev_id, next_id);
    float p = 0.0f;
    pthread_mutex_lock((pthread_mutex_t*)&tm->lock);
    int found = 0;
    int slot = tm_find_slot(tm, key, &found);
    if (found) {
        uint64_t hi = (uint64_t)(uint32_t)prev_id << 32;
        uint32_t out = 0;
        for (int i = 0; i < tm->capacity; i++) {
            if (tm->counts[i] != 0 && (tm->keys[i] & 0xFFFFFFFF00000000ULL) == hi) {
                out += tm->counts[i];
            }
        }
        if (out > 0) {
            p = ((float)tm->counts[slot] + TM_LAMBDA) /
                ((float)out + TM_LAMBDA * TM_K);
        }
    }
    pthread_mutex_unlock((pthread_mutex_t*)&tm->lock);
    return p;
}

int32_t tm_sample_next(const TransitionModel* tm, int32_t prev_id, uint32_t* rng_state) {
    if (!tm || prev_id < 0) return -1;
    uint64_t hi = (uint64_t)(uint32_t)prev_id << 32;

    pthread_mutex_lock((pthread_mutex_t*)&tm->lock);
    /* 第一遍：求权重总和 Σ(c+λ) */
    double total = 0.0;
    for (int i = 0; i < tm->capacity; i++) {
        if (tm->counts[i] != 0 && (tm->keys[i] & 0xFFFFFFFF00000000ULL) == hi) {
            total += (double)tm->counts[i] + TM_LAMBDA;
        }
    }
    if (total <= 0.0) {
        pthread_mutex_unlock((pthread_mutex_t*)&tm->lock);
        return -1;
    }
    /* 采样 */
    uint32_t r;
    if (rng_state) {
        /* xorshift32 —— 与 self_learner.c 同款的轻量 RNG，避免 rand() 全局锁 */
        uint32_t x = *rng_state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        *rng_state = x;
        r = x;
    } else {
        r = (uint32_t)rand();
    }
    double pick = ((double)r / 4294967296.0) * total;
    double acc = 0.0;
    int32_t result = -1;
    for (int i = 0; i < tm->capacity && result < 0; i++) {
        if (tm->counts[i] != 0 && (tm->keys[i] & 0xFFFFFFFF00000000ULL) == hi) {
            acc += (double)tm->counts[i] + TM_LAMBDA;
            if (pick < acc) result = (int32_t)(uint32_t)(tm->keys[i] & 0xFFFFFFFFULL);
        }
    }
    pthread_mutex_unlock((pthread_mutex_t*)&tm->lock);
    return result;
}

int tm_top_next(const TransitionModel* tm, int32_t prev_id, int32_t* out_ids, int k) {
    if (!tm || !out_ids || k <= 0 || prev_id < 0) return 0;
    uint64_t hi = (uint64_t)(uint32_t)prev_id << 32;

    /* 锁内只收集 (id,count)，排序在锁外做（tm_prob 也要加锁，锁内调会自死锁；
     * 且 DEFAULT 互斥锁非递归 —— 项目哲学「递归锁优先」但此处结构体已建，
     * 保持最小改动，靠「锁内零外部调用」纪律规避）。 */
    /* ð´ ä¿®ï¼åå®ç° `if (n < 16)` åªæ¶å 16 ä¸ªå¹éé¡¹å°±æªæ­ï¼
     * èåå¸è¡¨éåé¡ºåºæ¯**ä»»æç** â æ¶å°çæ¯ãéæº 16 ä¸ªåç»§」ã
     * åå¨è¿ 16 ä¸ªéæåºæ k ä¸ª â è¿åçä¸æ¯ãæå¯è½ç k ä¸ªåç»§」ã
     * åç»§æ° > 16 çè¯ï¼æ / ç / æ¯ / å­¦ä¹  â¦ï¼å¨é½è¢«æ±¡æï¼
     * CX-39 çã bigram å°é¡¶äºãå°±æ¯å¨è¿ä¸ª bug ä¸æµåºæ¥çã
     *
     * ç°æ¹ä¸ºæ å top-k éæ©ï¼éåå¨è¡¨ï¼ç»´æ¤å¤§å°ä¸º k çåééï¼
     * ä¸åä¸¢å¼ç¬¬ k+1 ä¸ªä¹åçå¹éã */
    #define TM_TOPK_MAX 32
    int32_t ids[TM_TOPK_MAX];
    uint32_t cnts[TM_TOPK_MAX];
    int kk = (k < TM_TOPK_MAX) ? k : TM_TOPK_MAX;
    int n = 0;
    pthread_mutex_lock((pthread_mutex_t*)&tm->lock);
    for (int i = 0; i < tm->capacity; i++) {
        if (tm->counts[i] != 0 && (tm->keys[i] & 0xFFFFFFFF00000000ULL) == hi) {
            int32_t id = (int32_t)(uint32_t)(tm->keys[i] & 0xFFFFFFFFULL);
            uint32_t c = tm->counts[i];
            if (n < kk) {
                ids[n] = id;
                cnts[n] = c;
                n++;
            } else {
                /* æ¾å½å k ä¸ªéæå°çï¼èè¿å®å°±é¡¶æ */
                int mi = 0;
                for (int j = 1; j < kk; j++) if (cnts[j] < cnts[mi]) mi = j;
                if (c > cnts[mi]) { ids[mi] = id; cnts[mi] = c; }
            }
        }
    }
    pthread_mutex_unlock((pthread_mutex_t*)&tm->lock);
    /* n å·²ç»ä¸è¶è¿ kkï¼ä¸é¢ç `if (n > k) n = k;` ä¿çä½åéä¿é© */
    if (n == 0) return 0;
    if (n > k) n = k;

    /* 按 count 降序（计数 + λ 与纯计数排序等价，直接比 count） */
    for (int a = 0; a < n; a++) {
        for (int b = a + 1; b < n; b++) {
            if (cnts[b] > cnts[a]) {
                uint32_t tc = cnts[a]; cnts[a] = cnts[b]; cnts[b] = tc;
                int32_t ti = ids[a];  ids[a] = ids[b];  ids[b] = ti;
            }
        }
    }
    memcpy(out_ids, ids, sizeof(int32_t) * (size_t)n);
    return n;
}

float tm_chain_logprob(const TransitionModel* tm, const int32_t* ids, int n) {
    if (!tm || !ids || n < 2) return 0.0f;
    float sum = 0.0f;
    for (int i = 1; i < n; i++) {
        float p = tm_prob(tm, ids[i - 1], ids[i]);
        if (p <= 0.0f) p = 1e-6f;
        sum += logf(p);
    }
    return sum;
}

int tm_pair_count(const TransitionModel* tm) {
    if (!tm) return 0;
    pthread_mutex_lock((pthread_mutex_t*)&tm->lock);
    int n = tm->size;
    pthread_mutex_unlock((pthread_mutex_t*)&tm->lock);
    return n;
}

int tm_save(const TransitionModel* tm, const char* path) {
    if (!tm || !path) return -1;
    FILE* fp = fopen(path, "wb");
    if (!fp) return -1;

    pthread_mutex_lock((pthread_mutex_t*)&tm->lock);
    uint32_t header[4];
    header[0] = TM_MAGIC;
    header[1] = TM_VERSION;
    header[2] = (uint32_t)tm->size;
    header[3] = 0;  /* reserved */
    if (fwrite(header, sizeof(uint32_t), 4, fp) != 4) {
        pthread_mutex_unlock((pthread_mutex_t*)&tm->lock);
        fclose(fp);
        return -1;
    }
    int written = 0;
    for (int i = 0; i < tm->capacity && written < tm->size; i++) {
        if (tm->counts[i] == 0) continue;
        uint64_t k = tm->keys[i];
        uint32_t c = tm->counts[i];
        if (fwrite(&k, sizeof(uint64_t), 1, fp) != 1 ||
            fwrite(&c, sizeof(uint32_t), 1, fp) != 1) {
            pthread_mutex_unlock((pthread_mutex_t*)&tm->lock);
            fclose(fp);
            return -1;
        }
        written++;
    }
    pthread_mutex_unlock((pthread_mutex_t*)&tm->lock);
    fclose(fp);
    return 0;
}

int tm_load(TransitionModel* tm, const char* path) {
    if (!tm || !path) return -1;
    FILE* fp = fopen(path, "rb");
    if (!fp) return -1;  /* 首次运行文件不存在：合法，空表起步 */

    uint32_t header[4];
    if (fread(header, sizeof(uint32_t), 4, fp) != 4 ||
        header[0] != TM_MAGIC || header[1] != TM_VERSION) {
        LOG_WARNING("[transition_model] %s 头无效（magic/version 不符），忽略", path);
        fclose(fp);
        return -1;
    }
    uint32_t n = header[2];
    for (uint32_t i = 0; i < n; i++) {
        uint64_t k;
        uint32_t c;
        if (fread(&k, sizeof(uint64_t), 1, fp) != 1 ||
            fread(&c, sizeof(uint32_t), 1, fp) != 1) {
            LOG_WARNING("[transition_model] %s 截断于第 %u/%u 条，已读部分保留", path, i, n);
            break;
        }
        int32_t prev = (int32_t)(uint32_t)(k >> 32);
        int32_t next = (int32_t)(uint32_t)(k & 0xFFFFFFFFULL);
        /* 绕过 observe 的互斥（此处单线程加载），直接灌表 */
        pthread_mutex_lock(&tm->lock);
        if ((tm->size + 1) * TM_LOAD_DEN >= tm->capacity * TM_LOAD_NUM) {
            if (tm_grow(tm) != 0) {
                pthread_mutex_unlock(&tm->lock);
                fclose(fp);
                return -1;
            }
        }
        int found = 0;
        int slot = tm_find_slot(tm, k, &found);
        if (!found && slot >= 0) {
            tm->keys[slot] = k;
            tm->counts[slot] = c;
            tm->size++;
        } else if (found) {
            tm->counts[slot] = c;
        }
        pthread_mutex_unlock(&tm->lock);
        (void)prev; (void)next;
    }
    fclose(fp);
    LOG_INFO("[transition_model] 已加载 %d 条转移计数（%s）", tm->size, path);
    return 0;
}

/* ==================== 全局单例 ==================== */

static TransitionModel* g_tm = NULL;
static pthread_mutex_t g_tm_lock = PTHREAD_MUTEX_INITIALIZER;

/* 每次直接 getenv 而不缓存：environ 是进程内只读段，开销纳秒级；
 * 调用点（每轮对话的 RETRY_FROM_POOL 分支、每次喂料）频率完全可承受。
 * 不缓存顺带让运行期改环境变量即时生效，单测也能覆盖开关切换。 */
int tm_global_enabled(void) {
    const char* e = getenv("PIVOTMIND_PROB_ASSIST");
    return !(e && e[0] == '0');
}

TransitionModel* tm_global(void) {
    if (!tm_global_enabled()) return NULL;
    pthread_mutex_lock(&g_tm_lock);
    if (!g_tm) {
        g_tm = tm_create();
        if (g_tm) {
            const char* path = pm_file(PM_FILE_TRANSITIONS);
            if (path) tm_load(g_tm, path);  /* 文件不存在/损坏：空表起步 */
        }
    }
    TransitionModel* ret = g_tm;
    pthread_mutex_unlock(&g_tm_lock);
    return ret;
}

void tm_global_shutdown_save(void) {
    pthread_mutex_lock(&g_tm_lock);
    if (g_tm) {
        const char* path = pm_file(PM_FILE_TRANSITIONS);
        if (path) {
            if (tm_save(g_tm, path) == 0) {
                LOG_INFO("[transition_model] 已保存 %d 条转移计数（%s）",
                         tm_pair_count(g_tm), path);
            } else {
                LOG_WARNING("[transition_model] 存盘失败：%s", path);
            }
        }
    }
    pthread_mutex_unlock(&g_tm_lock);
}
