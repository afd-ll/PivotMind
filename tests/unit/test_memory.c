/**
 * @file test_memory.c
 * @brief Unit tests for memory_system.c — STM/LTM/permanent storage
 */

#include "common.h"
#include "memory_system.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0, tests_passed = 0, tests_failed = 0;

#define TEST_START(name) printf("Running: %s...", name); tests_run++;
#define TEST_END() tests_passed++; printf(" PASSED\n")
#define TEST_FAIL(msg) tests_failed++; printf(" FAILED: %s\n", msg);
#define ASSERT_TRUE(cond, msg) do { if (!(cond)) { TEST_FAIL(msg); return; } } while (0)
#define ASSERT_NOT_NULL(ptr, msg) ASSERT_TRUE((ptr) != NULL, msg)
#define ASSERT_NULL(ptr, msg) ASSERT_TRUE((ptr) == NULL, msg)
#define ASSERT_EQUAL(a, b, msg) ASSERT_TRUE((a) == (b), msg)

void test_memory_create_destroy(void) {
    TEST_START("memory_system_create/destroy");
    MemorySystem* m = memory_system_create(100, 200, 500);
    ASSERT_NOT_NULL(m, "create failed");
    memory_system_destroy(m);
    TEST_END();
}

void test_memory_store_and_retrieve(void) {
    TEST_START("memory_system store/retrieve");
    MemorySystem* m = memory_system_create(100, 200, 500);
    ASSERT_NOT_NULL(m, "create failed");

    /* Store a string value.
     * memory_store 内部 create_memory_entry 会 malloc+memcpy 自持副本，
     * 故该 strdup 的所有权仍在测试侧，用完立即释放（含失败提前 return 路径）。 */
    char* val = strdup("value_one");
    int rc = memory_store(m, "key1", val, strlen(val)+1, MEMORY_TYPE_STRING, 0.9f);
    free(val);
    val = NULL;
    ASSERT_TRUE(rc >= 0, "store returned error");

    /* Retrieve */
    MemoryEntry* entry = memory_retrieve(m, "key1");
    ASSERT_NOT_NULL(entry, "retrieve returned NULL");
    if (entry) {
        ASSERT_NOT_NULL(entry->data, "entry->data is NULL");
    }

    memory_system_destroy(m);
    TEST_END();
}

void test_memory_retrieve_missing(void) {
    TEST_START("memory_system retrieve missing");
    MemorySystem* m = memory_system_create(100, 200, 500);
    ASSERT_NOT_NULL(m, "create failed");

    MemoryEntry* entry = memory_retrieve(m, "nonexistent_key_12345");
    ASSERT_NULL(entry, "retrieve should return NULL for missing key");

    memory_system_destroy(m);
    TEST_END();
}

void test_memory_store_multiple(void) {
    TEST_START("memory_system store multiple");
    MemorySystem* m = memory_system_create(100, 200, 500);
    ASSERT_NOT_NULL(m, "create failed");

    /* Store several items。
     * 每个 val 由测试自己 strdup，memory_store 内部自持副本，
     * 因此每轮 store 后立即 free；strdup 移入循环内，失败提前 return 也不残留。 */
    const char* seeds[] = {"a", "b", "c", "d", "e"};
    for (int i = 0; i < 5; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        char* val = strdup(seeds[i]);
        int rc = memory_store(m, key, val, strlen(val)+1, MEMORY_TYPE_STRING, 0.9f);
        free(val);
        val = NULL;
        ASSERT_TRUE(rc >= 0, "store failed");
    }

    /* Verify all retrievable */
    for (int i = 0; i < 5; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        MemoryEntry* e = memory_retrieve(m, key);
        ASSERT_NOT_NULL(e, "retrieve failed");
    }

    memory_system_destroy(m);
    TEST_END();
}

/* v0.5.25: 记忆种子原子写 + 完整性 footer 的 round-trip 验证。
 * 核心验证 P0 修复：保存端哈希范围 = 加载端哈希范围（footer 魔数不参与哈希），
 * 否则新格式种子保存后加载必报哈希校验失败。 */
void test_seed_save_load_roundtrip(void) {
    TEST_START("seed save/load roundtrip (v0.5.25 footer)");
    const char* path = "/tmp/test_pmseed_roundtrip.dat";
    remove(path);
    remove("/tmp/test_pmseed_roundtrip.dat.tmp");

    MemorySystem* m = memory_system_create(100, 200, 500);
    ASSERT_NOT_NULL(m, "create failed");

    /* 写几条种子（含中文 key、二进制 data，覆盖哈希边界）。
     * 注意：confidence >= 0.6 才进永久记忆(permanent_memory)，save_seed
     * 只序列化永久记忆，所以这里统一用 >= 0.6。 */
    memory_store(m, "你好世界", "data_cn", 8, MEMORY_TYPE_STRING, 0.8f);
    memory_store(m, "key2", "data_b", 7, MEMORY_TYPE_STRING, 0.7f);
    unsigned char bin[4] = {0x00, 0xFF, 0x12, 0x34};
    memory_store(m, "bin_key", bin, 4, MEMORY_TYPE_BINARY, 0.9f);

    int saved = memory_save_seed(m, path);
    ASSERT_TRUE(saved == 3, "save should return 3 entries");

    /* 换一个空系统加载，验证能读回 */
    MemorySystem* m2 = memory_system_create(100, 200, 500);
    ASSERT_NOT_NULL(m2, "create m2 failed");
    int loaded = memory_load_seed(m2, path);
    ASSERT_TRUE(loaded == 3, "load should return 3 entries (hash must match)");

    /* 验证内容一致 */
    MemoryEntry* e1 = memory_retrieve(m2, "你好世界");
    ASSERT_NOT_NULL(e1, "中文 key 未读回");
    if (e1) ASSERT_TRUE(strcmp((char*)e1->data, "data_cn") == 0, "中文数据不一致");
    MemoryEntry* e2 = memory_retrieve(m2, "bin_key");
    ASSERT_NOT_NULL(e2, "二进制 key 未读回");
    if (e2 && e2->data_size == 4) {
        ASSERT_TRUE(memcmp(e2->data, bin, 4) == 0, "二进制数据不一致");
    }

    memory_system_destroy(m);
    memory_system_destroy(m2);
    remove(path);
    TEST_END();
}

void test_seed_save_load_empty(void) {
    TEST_START("seed save/load empty");
    const char* path = "/tmp/test_pmseed_empty.dat";
    remove(path);
    remove("/tmp/test_pmseed_empty.dat.tmp");

    MemorySystem* m = memory_system_create(100, 200, 500);
    ASSERT_NOT_NULL(m, "create failed");
    int saved = memory_save_seed(m, path);
    ASSERT_TRUE(saved == 0, "empty save should return 0");

    MemorySystem* m2 = memory_system_create(100, 200, 500);
    int loaded = memory_load_seed(m2, path);
    ASSERT_TRUE(loaded == 0, "empty load should return 0 (only footer)");

    memory_system_destroy(m);
    memory_system_destroy(m2);
    remove(path);
    TEST_END();
}

int main(void) {
    printf("\n=== PivotMind Memory System Unit Tests ===\n\n");

    test_memory_create_destroy();
    test_memory_store_and_retrieve();
    test_memory_retrieve_missing();
    test_memory_store_multiple();
    test_seed_save_load_roundtrip();
    test_seed_save_load_empty();

    printf("\n=== Results: %d run, %d passed, %d failed ===\n",
           tests_run, tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
