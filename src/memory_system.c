#include "../include/memory_system.h"
#include "../include/causal_reasoning.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

// ==================== 因果规则数据结构（统一声明）====================

typedef struct {
    char cause_key[128];
    char effect_key[128];
    float strength;
    float base_score;
    int observation_count;
    int valid_scenarios;
    int total_scenarios;
    int consistent_count;
    int total_tests;
    time_t first_observed;
    time_t last_confirmed;
} CausalRuleData;

// ==================== 哈希表辅助函数 ====================

// FNV-1a 哈希
static unsigned int hash_key(const char* key, int table_size) {
    unsigned int h = 2166136261U;
    for (const char* p = key; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 16777619U;
    }
    return h % table_size;
}

static int _stm_hash_lookup(ShortTermMemory* stm, const char* key) {
    if (!stm || !key) return -1;
    int idx = hash_key(key, stm->capacity);
    int start = idx;
    do {
        if (stm->hash_table[idx].entry_index >= 0 &&
            strcmp(stm->hash_table[idx].key, key) == 0) {
            return stm->hash_table[idx].entry_index;
        }
        if (stm->hash_table[idx].entry_index < 0) break;
        idx = (idx + 1) % stm->capacity;
    } while (idx != start);
    return -1;
}

static void _stm_hash_insert(ShortTermMemory* stm, const char* key, int entry_idx) {
    int idx = hash_key(key, stm->capacity);
    int start = idx;
    do {
        if (stm->hash_table[idx].entry_index < 0) {
            stm->hash_table[idx].key = stm->entries[entry_idx]->key;
            stm->hash_table[idx].entry_index = entry_idx;
            return;
        }
        idx = (idx + 1) % stm->capacity;
    } while (idx != start);
}

static void _stm_hash_remove(ShortTermMemory* stm, const char* key) {
    int idx = hash_key(key, stm->capacity);
    int start = idx;
    do {
        if (stm->hash_table[idx].entry_index >= 0 &&
            strcmp(stm->hash_table[idx].key, key) == 0) {
            stm->hash_table[idx].entry_index = -1;
            return;
        }
        if (stm->hash_table[idx].entry_index < 0) break;
        idx = (idx + 1) % stm->capacity;
    } while (idx != start);
}

static int _ltm_hash_lookup(LongTermMemory* ltm, const char* key) {
    if (!ltm || !key) return -1;
    int idx = hash_key(key, ltm->max_entries);
    int start = idx;
    do {
        if (ltm->hash_table[idx].entry_index >= 0 &&
            strcmp(ltm->hash_table[idx].key, key) == 0) {
            return ltm->hash_table[idx].entry_index;
        }
        if (ltm->hash_table[idx].entry_index < 0) break;
        idx = (idx + 1) % ltm->max_entries;
    } while (idx != start);
    return -1;
}

static void _ltm_hash_insert(LongTermMemory* ltm, const char* key, int entry_idx) {
    int idx = hash_key(key, ltm->max_entries);
    int start = idx;
    do {
        if (ltm->hash_table[idx].entry_index < 0) {
            ltm->hash_table[idx].key = ltm->entries[entry_idx]->key;
            ltm->hash_table[idx].entry_index = entry_idx;
            return;
        }
        idx = (idx + 1) % ltm->max_entries;
    } while (idx != start);
}

static void _ltm_hash_remove(LongTermMemory* ltm, const char* key) {
    int idx = hash_key(key, ltm->max_entries);
    int start = idx;
    do {
        if (ltm->hash_table[idx].entry_index >= 0 &&
            strcmp(ltm->hash_table[idx].key, key) == 0) {
            ltm->hash_table[idx].entry_index = -1;
            return;
        }
        if (ltm->hash_table[idx].entry_index < 0) break;
        idx = (idx + 1) % ltm->max_entries;
    } while (idx != start);
}

// ==================== 记忆条目实现 ====================

MemoryEntry* create_memory_entry(const char* key, void* data, size_t data_size,
                                MemoryType type, float importance) {
    MemoryEntry* entry = (MemoryEntry*)malloc(sizeof(MemoryEntry));
    if (!entry) return NULL;

    entry->key = strdup(key);
    entry->data_size = data_size;
    entry->type = type;
    entry->importance = importance;
    entry->access_count = 0;
    entry->last_access = time(NULL);
    entry->created_at = time(NULL);
    entry->decay_factor = 0.95f;

    entry->data = malloc(data_size);
    if (data) {
        memcpy(entry->data, data, data_size);
    } else {
        memset(entry->data, 0, data_size);
    }

    return entry;
}

void destroy_memory_entry(MemoryEntry* entry) {
    if (!entry) return;

    free(entry->key);
    free(entry->data);
    free(entry);
}

// ==================== 短期记忆实现 ====================

ShortTermMemory* stm_create(int capacity) {
    ShortTermMemory* stm = (ShortTermMemory*)malloc(sizeof(ShortTermMemory));
    if (!stm) return NULL;

    stm->entries = (MemoryEntry**)calloc(capacity, sizeof(MemoryEntry*));
    stm->hash_table = (MemHashEntry*)calloc(capacity, sizeof(MemHashEntry));
    for (int i = 0; i < capacity; i++) {
        stm->hash_table[i].entry_index = -1;
    }
    stm->capacity = capacity;
    stm->size = 0;
    stm->current_index = 0;

    return stm;
}

void stm_destroy(ShortTermMemory* stm) {
    if (!stm) return;

    for (int i = 0; i < stm->size; i++) {
        destroy_memory_entry(stm->entries[i]);
    }
    free(stm->entries);
    free(stm->hash_table);
    free(stm);
}

int stm_store(ShortTermMemory* stm, const char* key, void* data,
              size_t data_size, MemoryType type, float importance) {
    if (!stm || !key || !data) return -1;

    int exist_idx = _stm_hash_lookup(stm, key);
    if (exist_idx >= 0) {
        // 更新现有条目
        free(stm->entries[exist_idx]->data);
        stm->entries[exist_idx]->data = malloc(data_size);
        memcpy(stm->entries[exist_idx]->data, data, data_size);
        stm->entries[exist_idx]->data_size = data_size;
        stm->entries[exist_idx]->importance = importance;
        stm->entries[exist_idx]->last_access = time(NULL);
        stm->entries[exist_idx]->access_count++;
        return 0;
    }

    // 如果达到容量限制，使用LRU策略替换
    if (stm->size >= stm->capacity) {
        int lru_index = 0;
        time_t oldest_time = stm->entries[0]->last_access;

        for (int i = 1; i < stm->size; i++) {
            if (stm->entries[i]->last_access < oldest_time) {
                oldest_time = stm->entries[i]->last_access;
                lru_index = i;
            }
        }

        // 从哈希表中移除被淘汰的 key
        _stm_hash_remove(stm, stm->entries[lru_index]->key);
        destroy_memory_entry(stm->entries[lru_index]);

        MemoryEntry* new_entry = create_memory_entry(key, data, data_size, type, importance);
        if (!new_entry) return -1;
        stm->entries[lru_index] = new_entry;
        _stm_hash_insert(stm, key, lru_index);
    } else {
        MemoryEntry* new_entry = create_memory_entry(key, data, data_size, type, importance);
        if (!new_entry) return -1;
        stm->entries[stm->size] = new_entry;
        _stm_hash_insert(stm, key, stm->size);
        stm->size++;
    }

    return 0;
}

MemoryEntry* stm_retrieve(ShortTermMemory* stm, const char* key) {
    if (!stm || !key) return NULL;

    int idx = _stm_hash_lookup(stm, key);
    if (idx >= 0) {
        stm->entries[idx]->last_access = time(NULL);
        stm->entries[idx]->access_count++;
        return stm->entries[idx];
    }

    return NULL;
}

// ==================== 长期记忆实现 ====================

LongTermMemory* ltm_create(int max_entries) {
    LongTermMemory* ltm = (LongTermMemory*)malloc(sizeof(LongTermMemory));
    if (!ltm) return NULL;

    ltm->entries = (MemoryEntry**)calloc(max_entries, sizeof(MemoryEntry*));
    ltm->index_map = (MemoryIndex*)calloc(max_entries, sizeof(MemoryIndex));
    ltm->hash_table = (MemHashEntry*)calloc(max_entries, sizeof(MemHashEntry));
    for (int i = 0; i < max_entries; i++) {
        ltm->hash_table[i].entry_index = -1;
    }
    ltm->max_entries = max_entries;
    ltm->size = 0;
    ltm->index_size = 0;

    return ltm;
}

void ltm_destroy(LongTermMemory* ltm) {
    if (!ltm) return;

    for (int i = 0; i < ltm->size; i++) {
        destroy_memory_entry(ltm->entries[i]);
    }
    free(ltm->entries);
    free(ltm->index_map);
    free(ltm->hash_table);
    free(ltm);
}

int ltm_store(LongTermMemory* ltm, const char* key, void* data,
              size_t data_size, MemoryType type, float importance) {
    if (!ltm || !key || !data || ltm->size >= ltm->max_entries) return -1;

    int exist_idx = _ltm_hash_lookup(ltm, key);
    if (exist_idx >= 0) {
        free(ltm->entries[exist_idx]->data);
        ltm->entries[exist_idx]->data = malloc(data_size);
        memcpy(ltm->entries[exist_idx]->data, data, data_size);
        ltm->entries[exist_idx]->importance = importance;
        return 0;
    }

    // 添加新条目
    ltm->entries[ltm->size] = create_memory_entry(key, data, data_size, type, importance);
    _ltm_hash_insert(ltm, key, ltm->size);

    // 更新 index_map（保留兼容）
    ltm->index_map[ltm->index_size].key = strdup(key);
    ltm->index_map[ltm->index_size].entry_index = ltm->size;
    ltm->index_size++;

    ltm->size++;
    return 0;
}

MemoryEntry* ltm_retrieve(LongTermMemory* ltm, const char* key) {
    if (!ltm || !key) return NULL;

    int idx = _ltm_hash_lookup(ltm, key);
    if (idx >= 0) {
        ltm->entries[idx]->last_access = time(NULL);
        ltm->entries[idx]->access_count++;
        return ltm->entries[idx];
    }

    return NULL;
}

// ==================== 记忆检索算法 ====================

// qsort 比较函数（降序）
static int _result_cmp(const void* a, const void* b) {
    float diff = ((MemorySearchResult*)b)->similarity - ((MemorySearchResult*)a)->similarity;
    if (diff > 0) return 1;
    if (diff < 0) return -1;
    return 0;
}

MemorySearchResult* memory_search(MemorySystem* memory, const char* query,
                                 int* result_count, float threshold) {
    if (!memory || !query || !result_count) return NULL;

    MemorySearchResult* temp_results = (MemorySearchResult*)calloc(100, sizeof(MemorySearchResult));
    int temp_count = 0;

    for (int i = 0; i < memory->short_term->size && temp_count < 100; i++) {
        MemoryEntry* entry = memory->short_term->entries[i];
        float similarity = calculate_similarity(query, entry->key);

        if (similarity >= threshold) {
            temp_results[temp_count].entry = entry;
            temp_results[temp_count].similarity = similarity;
            temp_results[temp_count].source = MEMORY_SOURCE_SHORT_TERM;
            temp_count++;
        }
    }

    for (int i = 0; i < memory->permanent_memory->size && temp_count < 100; i++) {
        MemoryEntry* entry = memory->permanent_memory->entries[i];
        float similarity = calculate_similarity(query, entry->key);

        if (similarity >= threshold) {
            temp_results[temp_count].entry = entry;
            temp_results[temp_count].similarity = similarity;
            temp_results[temp_count].source = MEMORY_SOURCE_LONG_TERM;
            temp_count++;
        }
    }

    if (temp_count > 1) {
        qsort(temp_results, temp_count, sizeof(MemorySearchResult), _result_cmp);
    }

    *result_count = temp_count;
    return temp_results;
}

// ==================== 记忆级别判断 ====================

MemoryLevel get_memory_level(float confidence) {
    if (confidence < 0.3f) {
        return MEMORY_LEVEL_CONTEXT;
    } else if (confidence < 0.6f) {
        return MEMORY_LEVEL_SHORT_TERM;
    } else {
        return MEMORY_LEVEL_PERMANENT;
    }
}

// ==================== 记忆系统主实现 ====================

MemorySystem* memory_system_create(int context_capacity, int stm_capacity, int permanent_capacity) {
    MemorySystem* memory = (MemorySystem*)malloc(sizeof(MemorySystem));
    if (!memory) return NULL;

    memory->context_memory = stm_create(context_capacity);
    memory->short_term = stm_create(stm_capacity);
    memory->permanent_memory = ltm_create(permanent_capacity);
    memory->total_operations = 0;
    memory->last_consolidation = time(NULL);
    memory->confidence_threshold_1 = 0.3f;
    memory->confidence_threshold_2 = 0.6f;

    memory->pending_updates = 0;
    memory->batch_threshold = 100;
    memory->last_save_time = time(NULL);
    memory->save_interval = 60;
    memory->save_enabled = 0;

    return memory;
}

void memory_system_destroy(MemorySystem* memory) {
    if (!memory) return;

    stm_destroy(memory->context_memory);
    stm_destroy(memory->short_term);
    ltm_destroy(memory->permanent_memory);
    free(memory);
}

MemoryEntry* memory_retrieve(MemorySystem* memory, const char* key) {
    if (!memory || !key) return NULL;

    MemoryEntry* entry = stm_retrieve(memory->context_memory, key);
    if (entry) return entry;

    entry = stm_retrieve(memory->short_term, key);
    if (entry) return entry;

    return ltm_retrieve(memory->permanent_memory, key);
}

// ==================== 记忆巩固实现 ====================

void memory_consolidate(MemorySystem* memory) {
    if (!memory) return;

    time_t current_time = time(NULL);

    if (memory->total_operations < 1000 &&
        (current_time - memory->last_consolidation) < 3600) {
        return;
    }

    printf("开始记忆巩固过程...\n");

    // 检查上下文记忆，提升到短期记忆
    int write_idx = 0;
    for (int i = 0; i < memory->context_memory->size; i++) {
        MemoryEntry* entry = memory->context_memory->entries[i];

        if (entry->importance >= memory->confidence_threshold_1) {
            if (stm_store(memory->short_term, entry->key, entry->data,
                         entry->data_size, entry->type, entry->importance) == 0) {
                printf("记忆提升：'%s' 从上下文记忆升级到短期记忆 (置信度: %.2f)\n",
                       entry->key, entry->importance);
            }
            destroy_memory_entry(entry);
        } else {
            if (write_idx != i) {
                memory->context_memory->entries[write_idx] = entry;
            }
            write_idx++;
        }
    }
    for (int i = write_idx; i < memory->context_memory->size; i++) {
        memory->context_memory->entries[i] = NULL;
    }
    memory->context_memory->size = write_idx;

    // 检查短期记忆，提升到永久记忆
    write_idx = 0;
    for (int i = 0; i < memory->short_term->size; i++) {
        MemoryEntry* entry = memory->short_term->entries[i];

        if (entry->importance >= memory->confidence_threshold_2) {
            if (ltm_store(memory->permanent_memory, entry->key, entry->data,
                         entry->data_size, entry->type, entry->importance) == 0) {
                printf("记忆提升：'%s' 从短期记忆升级到永久记忆 (置信度: %.2f)\n",
                       entry->key, entry->importance);
            }
            destroy_memory_entry(entry);
        } else {
            if (write_idx != i) {
                memory->short_term->entries[write_idx] = entry;
            }
            write_idx++;
        }
    }
    for (int i = write_idx; i < memory->short_term->size; i++) {
        memory->short_term->entries[i] = NULL;
    }
    memory->short_term->size = write_idx;

    memory->last_consolidation = current_time;
    printf("记忆巩固完成\n");
}

// ==================== 记忆更新策略 ====================

void memory_update_confidence(MemorySystem* memory, const char* key, float new_confidence) {
    if (!memory || !key) return;

    // 上下文记忆（容量小，线性查找仍可接受，但利用哈希表）
    int idx = _stm_hash_lookup(memory->context_memory, key);
    if (idx >= 0) {
        MemoryEntry* entry = memory->context_memory->entries[idx];
        entry->importance = new_confidence;

        if (new_confidence >= memory->confidence_threshold_1) {
            stm_store(memory->short_term, entry->key, entry->data,
                     entry->data_size, entry->type, entry->importance);

            _stm_hash_remove(memory->context_memory, key);
            // 压缩上下文数组
            for (int j = idx; j < memory->context_memory->size - 1; j++) {
                memory->context_memory->entries[j] = memory->context_memory->entries[j + 1];
            }
            memory->context_memory->size--;

            printf("记忆升级：'%s' 从上下文记忆升级到短期记忆\n", key);
        }
        return;
    }

    // 短期记忆
    idx = _stm_hash_lookup(memory->short_term, key);
    if (idx >= 0) {
        MemoryEntry* entry = memory->short_term->entries[idx];
        entry->importance = new_confidence;

        if (new_confidence >= memory->confidence_threshold_2) {
            ltm_store(memory->permanent_memory, entry->key, entry->data,
                     entry->data_size, entry->type, entry->importance);

            _stm_hash_remove(memory->short_term, key);
            for (int j = idx; j < memory->short_term->size - 1; j++) {
                memory->short_term->entries[j] = memory->short_term->entries[j + 1];
            }
            memory->short_term->size--;

            printf("记忆升级：'%s' 从短期记忆升级到永久记忆\n", key);
        } else if (new_confidence < memory->confidence_threshold_1) {
            stm_store(memory->context_memory, entry->key, entry->data,
                     entry->data_size, entry->type, entry->importance);

            _stm_hash_remove(memory->short_term, key);
            for (int j = idx; j < memory->short_term->size - 1; j++) {
                memory->short_term->entries[j] = memory->short_term->entries[j + 1];
            }
            memory->short_term->size--;

            printf("记忆降级：'%s' 从短期记忆降级到上下文记忆\n", key);
        }
        return;
    }

    // 永久记忆
    idx = _ltm_hash_lookup(memory->permanent_memory, key);
    if (idx >= 0) {
        MemoryEntry* entry = memory->permanent_memory->entries[idx];
        entry->importance = new_confidence;

        if (new_confidence < memory->confidence_threshold_2) {
            stm_store(memory->short_term, entry->key, entry->data,
                     entry->data_size, entry->type, entry->importance);

            _ltm_hash_remove(memory->permanent_memory, key);
            for (int j = idx; j < memory->permanent_memory->size - 1; j++) {
                memory->permanent_memory->entries[j] = memory->permanent_memory->entries[j + 1];
            }
            memory->permanent_memory->size--;

            printf("记忆降级：'%s' 从永久记忆降级到短期记忆\n", key);
        }
    }
}

// ==================== 记忆测验学习 ====================

int memory_test_and_learn(MemorySystem* memory, const char* key, int test_result) {
    if (!memory || !key) return -1;

    MemoryEntry* entry = memory_retrieve(memory, key);
    if (!entry) return -1;

    float confidence_change = 0.0f;
    if (test_result > 0) {
        confidence_change = 0.1f * test_result;
        printf("测验成功：'%s' 置信度提升 %.2f\n", key, confidence_change);
    } else if (test_result < 0) {
        confidence_change = -0.05f * abs(test_result);
        printf("测验失败：'%s' 置信度降低 %.2f\n", key, confidence_change);
    }

    float new_confidence = entry->importance + confidence_change;
    new_confidence = fmax(0.0f, fmin(1.0f, new_confidence));

    memory_update_confidence(memory, key, new_confidence);

    return 0;
}

// ==================== 批量延迟保存实现 ====================

void memory_set_batch_save(MemorySystem* memory, int enabled, int threshold, int interval_seconds) {
    if (!memory) return;

    memory->save_enabled = enabled;
    memory->batch_threshold = (threshold > 0) ? threshold : 100;
    memory->save_interval = (interval_seconds > 0) ? interval_seconds : 60;
    memory->last_save_time = time(NULL);

    printf("[批量保存] %s (阈值=%d次, 间隔=%ld秒)\n",
           enabled ? "已启用" : "已禁用",
           memory->batch_threshold,
           memory->save_interval);
}

int memory_flush_pending(MemorySystem* memory) {
    if (!memory) return 0;

    int saved_count = memory->pending_updates;
    if (saved_count == 0) return 0;

    printf("[批量保存] 已刷新 %d 个待保存操作\n", saved_count);
    memory->pending_updates = 0;
    memory->last_save_time = time(NULL);

    return saved_count;
}

void memory_check_auto_save(MemorySystem* memory) {
    if (!memory || !memory->save_enabled) return;

    time_t now = time(NULL);

    if (memory->pending_updates >= memory->batch_threshold) {
        printf("[批量保存] 达到阈值 %d，触发保存\n", memory->pending_updates);
        memory_flush_pending(memory);
        return;
    }

    if ((now - memory->last_save_time) >= memory->save_interval && memory->pending_updates > 0) {
        printf("[批量保存] 定时触发（间隔 %ld 秒），保存 %d 个操作\n",
               (long)(now - memory->last_save_time), memory->pending_updates);
        memory_flush_pending(memory);
    }
}

static int memory_store_internal(MemorySystem* memory, const char* key, void* data,
                                 size_t data_size, MemoryType type, float confidence, int count_update) {
    if (!memory || !key || !data) return -1;

    memory->total_operations++;

    MemoryLevel level = get_memory_level(confidence);
    int result;

    switch (level) {
        case MEMORY_LEVEL_CONTEXT:
            result = stm_store(memory->context_memory, key, data, data_size, type, confidence);
            break;
        case MEMORY_LEVEL_SHORT_TERM:
            result = stm_store(memory->short_term, key, data, data_size, type, confidence);
            break;
        case MEMORY_LEVEL_PERMANENT:
            result = ltm_store(memory->permanent_memory, key, data, data_size, type, confidence);
            break;
        default:
            return -1;
    }

    if (result == 0 && count_update && memory->save_enabled) {
        memory->pending_updates++;
        memory_check_auto_save(memory);
    }

    return result;
}

int memory_store(MemorySystem* memory, const char* key, void* data,
                size_t data_size, MemoryType type, float confidence) {
    return memory_store_internal(memory, key, data, data_size, type, confidence, 1);
}

// ==================== 辅助函数 ====================

float calculate_similarity(const char* query, const char* key) {
    if (!query || !key) return 0.0f;

    int query_len = strlen(query);
    int key_len = strlen(key);

    if (strstr(key, query) != NULL) {
        return 0.8f + 0.2f * (query_len / (float)key_len);
    }

    int match_count = 0;
    for (int i = 0; i < query_len; i++) {
        for (int j = 0; j < key_len; j++) {
            if (query[i] == key[j]) {
                match_count++;
                break;
            }
        }
    }

    if (query_len + key_len - match_count == 0) return 0.0f;
    return (float)match_count / (query_len + key_len - match_count);
}

// ==================== 溯智网络集成 ====================

int memory_integrate_with_huarong(MemorySystem* memory, HuarongTopologyNet* net) {
    if (!memory || !net) return -1;

    for (int i = 0; i < net->node_count; i++) {
        ReasoningNode* node = net->nodes[i];

        size_t data_size = node->feature_dim * sizeof(float) + sizeof(int);
        char* data_buffer = (char*)malloc(data_size);

        if (data_buffer) {
            memcpy(data_buffer, &node->feature_dim, sizeof(int));
            memcpy(data_buffer + sizeof(int), node->features,
                   node->feature_dim * sizeof(float));

            char key[256];
            snprintf(key, sizeof(key), "node_%d_%s", node->node_id, node->concept);

            memory_store(memory, key, data_buffer, data_size,
                        MEMORY_TYPE_FLOAT_ARRAY, node->activation);

            free(data_buffer);
        }
    }

    return 0;
}

// ==================== 因果规则记忆存储 ====================

int memory_store_causal_rule(MemorySystem* memory, const char* cause_key,
                            const char* effect_key, float rule_strength,
                            CausalConfidence* rule) {
    if (!memory || !cause_key || !effect_key) return -1;

    char key[512];
    snprintf(key, sizeof(key), "causal:%s->%s", cause_key, effect_key);

    CausalRuleData rule_data;
    strncpy(rule_data.cause_key, cause_key, sizeof(rule_data.cause_key) - 1);
    rule_data.cause_key[sizeof(rule_data.cause_key) - 1] = '\0';
    strncpy(rule_data.effect_key, effect_key, sizeof(rule_data.effect_key) - 1);
    rule_data.effect_key[sizeof(rule_data.effect_key) - 1] = '\0';
    rule_data.strength = rule_strength;

    if (rule) {
        rule_data.base_score = rule->base_score;
        rule_data.observation_count = rule->observation_count;
        rule_data.valid_scenarios = rule->valid_scenarios;
        rule_data.total_scenarios = rule->total_scenarios;
        rule_data.consistent_count = rule->consistent_count;
        rule_data.total_tests = rule->total_tests;
        rule_data.first_observed = rule->first_observed;
        rule_data.last_confirmed = rule->last_confirmed;
    } else {
        rule_data.base_score = rule_strength;
        rule_data.observation_count = 1;
        rule_data.valid_scenarios = 1;
        rule_data.total_scenarios = 1;
        rule_data.consistent_count = 1;
        rule_data.total_tests = 1;
        rule_data.first_observed = time(NULL);
        rule_data.last_confirmed = time(NULL);
    }

    float importance = rule ? compute_causal_confidence(rule) : rule_strength;

    return memory_store(memory, key, &rule_data, sizeof(CausalRuleData),
                       MEMORY_TYPE_CAUSAL_RULE, importance);
}

float memory_get_causal_rule(MemorySystem* memory, const char* cause_key,
                            const char* effect_key, CausalConfidence* out_rule) {
    if (!memory || !cause_key || !effect_key) return -1.0f;

    char key[512];
    snprintf(key, sizeof(key), "causal:%s->%s", cause_key, effect_key);

    MemoryEntry* entry = memory_retrieve(memory, key);
    if (!entry) return -1.0f;

    if (out_rule && entry->type == MEMORY_TYPE_CAUSAL_RULE) {
        CausalRuleData* rule_data = (CausalRuleData*)entry->data;
        out_rule->base_score = rule_data->base_score;
        out_rule->observation_count = rule_data->observation_count;
        out_rule->valid_scenarios = rule_data->valid_scenarios;
        out_rule->total_scenarios = rule_data->total_scenarios;
        out_rule->consistent_count = rule_data->consistent_count;
        out_rule->total_tests = rule_data->total_tests;
        out_rule->first_observed = rule_data->first_observed;
        out_rule->last_confirmed = rule_data->last_confirmed;
    }

    CausalRuleData* rule_data = (CausalRuleData*)entry->data;
    return rule_data->strength;
}

int memory_search_causal_rules(MemorySystem* memory, const char* concept,
                              int max_results, char results[][512]) {
    if (!memory || !concept || max_results <= 0) return 0;

    int count = 0;

    for (int i = 0; i < memory->short_term->size && count < max_results; i++) {
        MemoryEntry* entry = memory->short_term->entries[i];
        if (entry && entry->type == MEMORY_TYPE_CAUSAL_RULE) {
            CausalRuleData* key = (CausalRuleData*)entry->data;

            if (strstr(key->cause_key, concept) || strstr(key->effect_key, concept)) {
                strncpy(results[count], entry->key, 511);
                results[count][511] = '\0';
                count++;
            }
        }
    }

    for (int i = 0; i < memory->permanent_memory->size && count < max_results; i++) {
        MemoryEntry* entry = memory->permanent_memory->entries[i];
        if (entry && entry->type == MEMORY_TYPE_CAUSAL_RULE) {
            CausalRuleData* key = (CausalRuleData*)entry->data;

            if (strstr(key->cause_key, concept) || strstr(key->effect_key, concept)) {
                strncpy(results[count], entry->key, 511);
                results[count][511] = '\0';
                count++;
            }
        }
    }

    return count;
}

MemoryLevel memory_get_causal_rule_level(MemorySystem* memory,
                                        const char* cause_key,
                                        const char* effect_key) {
    if (!memory || !cause_key || !effect_key) return MEMORY_LEVEL_CONTEXT;

    char key[512];
    snprintf(key, sizeof(key), "causal:%s->%s", cause_key, effect_key);

    int idx = _stm_hash_lookup(memory->short_term, key);
    if (idx >= 0) {
        MemoryEntry* entry = memory->short_term->entries[idx];
        if (entry->importance >= 0.6f) return MEMORY_LEVEL_PERMANENT;
        if (entry->importance >= 0.3f) return MEMORY_LEVEL_SHORT_TERM;
        return MEMORY_LEVEL_CONTEXT;
    }

    idx = _ltm_hash_lookup(memory->permanent_memory, key);
    if (idx >= 0) {
        return MEMORY_LEVEL_PERMANENT;
    }

    return MEMORY_LEVEL_CONTEXT;
}
