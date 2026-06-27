/**
 * @file test_memory.c
 * @brief Unit tests for memory_system.c — STM/LTM/permanent storage
 */

#include "../include/common.h"
#include "../include/memory_system.h"
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

    /* Store a string value */
    char* val = strdup("value_one");
    int rc = memory_store(m, "key1", val, strlen(val)+1, MEMORY_TYPE_STRING, 0.9f);
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

    /* Store several items */
    char* vals[] = {strdup("a"), strdup("b"), strdup("c"), strdup("d"), strdup("e")};
    for (int i = 0; i < 5; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        int rc = memory_store(m, key, vals[i], strlen(vals[i])+1, MEMORY_TYPE_STRING, 0.9f);
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

int main(void) {
    printf("\n=== PivotMind Memory System Unit Tests ===\n\n");

    test_memory_create_destroy();
    test_memory_store_and_retrieve();
    test_memory_retrieve_missing();
    test_memory_store_multiple();

    printf("\n=== Results: %d run, %d passed, %d failed ===\n",
           tests_run, tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
