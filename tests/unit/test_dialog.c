/**
 * @file test_dialog.c
 * @brief Unit tests for dialog_system.c — input parsing lifecycle
 */

#include "../include/common.h"
#include "../include/dialog_system.h"
#include "../include/multi_topology.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0, tests_passed = 0, tests_failed = 0;

#define TEST_START(name) printf("Running: %s...", name); tests_run++;
#define TEST_END() tests_passed++; printf(" PASSED\n")
#define TEST_FAIL(msg) tests_failed++; printf(" FAILED: %s\n", msg);
#define ASSERT_TRUE(cond, msg) do { if (!(cond)) { TEST_FAIL(msg); return; } } while (0)
#define ASSERT_NOT_NULL(ptr, msg) ASSERT_TRUE((ptr) != NULL, msg)

void test_dialog_input_create(void) {
    TEST_START("dialog_input_create/destroy");
    DialogInput* input = dialog_input_create(
        "\xe4\xbb\x80\xe4\xb9\x88\xe6\x98\xaf\xe6\x84\x8f\xe8\xaf\x86"); /* 什么是意识 */
    ASSERT_NOT_NULL(input, "returned NULL");
    ASSERT_NOT_NULL(input->original, "original is NULL");
    ASSERT_TRUE(input->token_count > 0, "token_count should be > 0");
    ASSERT_NOT_NULL(input->tokens, "tokens is NULL");
    dialog_input_destroy(input);
    TEST_END();
}

void test_dialog_input_empty(void) {
    TEST_START("dialog_input empty string");
    DialogInput* input = dialog_input_create("");
    ASSERT_NOT_NULL(input, "empty input should still create");
    dialog_input_destroy(input);
    TEST_END();
}

void test_dialog_input_null(void) {
    TEST_START("dialog_input NULL");
    DialogInput* input = dialog_input_create(NULL);
    if (input) dialog_input_destroy(input);
    TEST_END();
}

void test_dialog_input_single_char(void) {
    TEST_START("dialog_input single char");
    DialogInput* input = dialog_input_create("\xe6\x84\x8f"); /* "意" */
    ASSERT_NOT_NULL(input, "returned NULL");
    ASSERT_NOT_NULL(input->tokens, "tokens NULL");
    dialog_input_destroy(input);
    TEST_END();
}

void test_dialog_input_punctuation(void) {
    TEST_START("dialog_input punctuation");
    DialogInput* input = dialog_input_create("?");
    ASSERT_NOT_NULL(input, "returned NULL");
    dialog_input_destroy(input);
    TEST_END();
}

int main(void) {
    printf("\n=== PivotMind Dialog System Unit Tests ===\n\n");
    test_dialog_input_create();
    test_dialog_input_empty();
    test_dialog_input_null();
    test_dialog_input_single_char();
    test_dialog_input_punctuation();
    printf("\n=== Results: %d run, %d passed, %d failed ===\n",
           tests_run, tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
