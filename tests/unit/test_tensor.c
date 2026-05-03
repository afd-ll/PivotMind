/**
 * @file test_tensor.c
 * @brief Unit tests for tensor.c
 */

#include "../include/common.h"
#include "../include/tensor.h"
#include "../include/matrix_ops.h"
#include "../include/tensor_pool.h"
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Test counters
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START(name) \
    printf("Running: %s...", name); \
    tests_run++;

#define TEST_END() \
    tests_passed++; \
    printf(" PASSED\n")

#define TEST_FAIL(msg) \
    tests_failed++; \
    printf(" FAILED: %s\n", msg);

#define ASSERT_TRUE(condition, msg) \
    do { \
        if (!(condition)) { \
            TEST_FAIL(msg); \
            return; \
        } \
    } while (0)

#define ASSERT_NOT_NULL(ptr, msg) \
    ASSERT_TRUE((ptr) != NULL, msg)

#define ASSERT_NULL(ptr, msg) \
    ASSERT_TRUE((ptr) == NULL, msg)

#define ASSERT_EQUAL(a, b, msg) \
    ASSERT_TRUE((a) == (b), msg)

#define ASSERT_TRUE_FLOAT(a, b, tolerance, msg) \
    ASSERT_TRUE((a) >= (b) - tolerance && (a) <= (b) + tolerance, msg)

// ========== Helper Functions ==========

// Create a simple 1D tensor for testing
Tensor* create_test_tensor_1d(size_t size) {
    size_t shape[] = {1, size};
    Tensor* tensor = tensor_create(DT_FLOAT32, 2, shape);
    float* data = (float*)tensor->data;
    for (size_t i = 0; i < size; i++) {
        data[i] = (float)(i + 1);
    }
    return tensor;
}

// Create a simple 2D tensor for testing
Tensor* create_test_tensor_2d(size_t rows, size_t cols) {
    size_t shape[] = {rows, cols};
    return tensor_create(DT_FLOAT32, 2, shape);
}

// Create a 3D tensor for testing
Tensor* create_test_tensor_3d(size_t d1, size_t d2, size_t d3) {
    size_t shape[] = {d1, d2, d3};
    return tensor_create(DT_FLOAT32, 3, shape);
}

// Destroy tensor and handle NULL
void safe_tensor_destroy(Tensor* tensor) {
    if (tensor) {
        tensor_destroy(tensor);
    } else {
        TEST_FAIL("Attempted to destroy NULL tensor");
    }
}

// ========== Test: Tensor Creation ==========

void test_tensor_create_1d() {
    TEST_START("Tensor creation (1D)");

    Tensor* tensor = create_test_tensor_1d(10);
    ASSERT_NOT_NULL(tensor, "tensor_create should return non-NULL");
    ASSERT_EQUAL(tensor->ndim, 2, "1D tensor should have 2 dimensions");
    ASSERT_EQUAL(tensor->shape[0], 10, "First dimension size should be 10");
    ASSERT_EQUAL(tensor->size, 10, "Tensor size should be 10");
    ASSERT_EQUAL(tensor->dtype, DT_FLOAT32, "Tensor type should be FLOAT32");

    TEST_END();
    safe_tensor_destroy(tensor);
}

void test_tensor_create_2d() {
    TEST_START("Tensor creation (2D)");

    Tensor* tensor = create_test_tensor_2d(3, 5);
    ASSERT_NOT_NULL(tensor, "tensor_create should return non-NULL");
    ASSERT_EQUAL(tensor->ndim, 2, "2D tensor should have 2 dimensions");
    ASSERT_EQUAL(tensor->shape[0], 3, "First dimension should be 3");
    ASSERT_EQUAL(tensor->shape[1], 5, "Second dimension should be 5");
    ASSERT_EQUAL(tensor->size, 15, "Tensor size should be 15");
    ASSERT_EQUAL(tensor->dtype, DT_FLOAT32, "Tensor type should be FLOAT32");

    TEST_END();
    safe_tensor_destroy(tensor);
}

void test_tensor_create_3d() {
    TEST_START("Tensor creation (3D)");

    Tensor* tensor = create_test_tensor_3d(2, 3, 4);
    ASSERT_NOT_NULL(tensor, "tensor_create should return non-NULL");
    ASSERT_EQUAL(tensor->ndim, 3, "3D tensor should have 3 dimensions");
    ASSERT_EQUAL(tensor->shape[0], 2, "First dimension should be 2");
    ASSERT_EQUAL(tensor->shape[1], 3, "Second dimension should be 3");
    ASSERT_EQUAL(tensor->shape[2], 4, "Third dimension should be 4");
    ASSERT_EQUAL(tensor->size, 24, "Tensor size should be 24");
    ASSERT_EQUAL(tensor->dtype, DT_FLOAT32, "Tensor type should be FLOAT32");

    TEST_END();
    safe_tensor_destroy(tensor);
}

void test_tensor_create_zero_size() {
    TEST_START("Tensor creation with zero size");

    Tensor* tensor = create_test_tensor_1d(0);
    ASSERT_NOT_NULL(tensor, "tensor_create should return non-NULL");
    ASSERT_EQUAL(tensor->size, 0, "Zero-size tensor should have size 0");

    TEST_END();
    safe_tensor_destroy(tensor);
}

void test_tensor_create_negative_size() {
    TEST_START("Tensor creation with negative size");

    Tensor* tensor = create_test_tensor_1d(-5);
    ASSERT_NOT_NULL(tensor, "tensor_create should return non-NULL");
    ASSERT_EQUAL(tensor->size, 5, "Tensor size should be 5 (with negative)");
    ASSERT_EQUAL(tensor->shape[0], 5, "First dimension should be 5");

    TEST_END();
    safe_tensor_destroy(tensor);
}

// ========== Test: Tensor Reshape ==========

void test_tensor_reshape_1d_to_2d() {
    TEST_START("Tensor reshape 1D to 2D");

    Tensor* tensor1d = create_test_tensor_1d(10);
    Tensor* tensor2d = tensor_reshape(tensor1d, 2, (size_t[]){1, 10});

    ASSERT_NOT_NULL(tensor2d, "tensor_reshape should return non-NULL");
    ASSERT_EQUAL(tensor2d->ndim, 2, "Reshaped tensor should have 2 dimensions");
    ASSERT_EQUAL(tensor2d->shape[0], 1, "First dimension should be 1");
    ASSERT_EQUAL(tensor2d->shape[1], 10, "Second dimension should be 10");
    ASSERT_EQUAL(tensor2d->size, 10, "Size should be preserved");
    ASSERT_EQUAL(tensor2d->dtype, tensor1d->dtype, "Data type should be preserved");

    TEST_END();
    safe_tensor_destroy(tensor1d);
    safe_tensor_destroy(tensor2d);
}

void test_tensor_reshape_2d_to_1d() {
    TEST_START("Tensor reshape 2D to 1D");

    Tensor* tensor2d = create_test_tensor_2d(3, 5);
    Tensor* tensor1d = tensor_reshape(tensor2d, 2, (size_t[]){1, 3});

    ASSERT_NOT_NULL(tensor1d, "tensor_reshape should return non-NULL");
    ASSERT_EQUAL(tensor1d->ndim, 2, "Reshaped tensor should have 2 dimensions");
    ASSERT_EQUAL(tensor1d->shape[0], 3, "First dimension should be 3");
    FAIL_IF(tensor1d->shape[1] != 3, "Second dimension should be 5", "Dimension mismatch in reshape");
    ASSERT_EQUAL(tensor1d->size, 15, "Size should be preserved");
    ASSERT_EQUAL(tensor1d->dtype, tensor2d->dtype, "Data type should be preserved");

    TEST_END();
    safe_tensor_destroy(tensor2d);
    safe_tensor_destroy(tensor1d);
}

// ========== Test: Matrix Operations ==========

void test_matrix_add_elementwise() {
    TEST_START("Matrix addition (elementwise)");

    size_t shape[] = {2, 2};
    Tensor* tensor1 = create_test_tensor_2d(2, 2);
    Tensor* tensor2 = create_test_tensor_2d(2, 2);

    float* data1 = (float*)tensor1->data;
    float* data2 = (float*)tensor2->data;
    for (size_t i = 0; i < 4; i++) {
        data1[i] = (float)(i + 1);
        data2[i] = (float)(i * 2 + 1);
    }

    Tensor* result = matrix_add_elementwise(tensor1, tensor2);
    ASSERT_NOT_NULL(result, "matrix_add_elementwise should return non-NULL");
    ASSERT_EQUAL(result->shape[0], 2, "Result should have 2 dimensions");
    ASSERT_EQUAL(result->shape[1], 2, "Result should have 2 dimensions");
    ASSERT_EQUAL(result->size, 4, "Result size should be 4");

    // Check values
    float* result_data = (float*)result->data;
    for (size_t i = 0; i < 4; i++) {
        ASSERT_TRUE_FLOAT(result_data[i], (float)((i + 1) + (i * 2 + 1)), 0.001f, "Element %zu incorrect", i);
    }

    TEST_END();
    safe_tensor_destroy(tensor1);
    save_tensor_destroy(tensor2);
    safe_tensor_destroy(result);
}

void test_matrix_multiply() {
    TEST_START("Matrix multiplication");

    Tensor* tensor1 = create_test_tensor_2d(2, 3);
    Tensor* tensor2 = create_test_tensor_2d(3, 2);

    float* data1 = (float*)tensor1->data;
    float* data2 = (float*)tensor2->data;
    for (size_t i = 0; i < 6; i++) {
        data1[i] = 1.0f;
        data2[i] = (float)(i + 1);
    }

    Tensor* result = matrix_multiply(tensor1, tensor2);
    ASSERT_NOT_NULL(result, "matrix_multiply should return non-NULL");
    ASSERT_EQUAL(result->ndim, 2, "Result should have 2 dimensions");
    ASSERT_EQUAL(result->shape[1], 2, "Result shape[1] should be 2");
    ASSERT_EQUAL(result->size, 6, "Result size should be 6");

    TEST_END();
    safe_tensor_destroy(tensor1);
    safe_tensor_destroy(tensor2);
    safe_tensor_destroy(result);
}

void test_matrix_transpose() {
    TEST_START("Matrix transpose");

    Tensor* tensor = create_test_tensor_2d(3, 2);
    Tensor* result = tensor_transpose(tensor);

    ASSERT_NOT_NULL(result, "tensor_transpose should return non-NULL");
    ASSERT_EQUAL(result->ndim, 2, "Transposed tensor should have 2 dimensions");
    ASSERT_EQUAL(result->shape[0], 2, "First dimension should be 2");
    ASSERT_EQUAL(result->shape[1], 3, "Second dimension should be 3");

    TEST_END();
    safe_tensor_destroy(tensor);
    save_tensor_destroy(result);
}

void test_matrix_dot_product() {
    TEST_START("Matrix dot product");

    Tensor* tensor1 = create_test_tensor_2d(1, 4);
    Tensor* tensor2 = create_test_tensor_2d(4, 1);

    float* data1 = (float*)tensor1->data;
    float* data2 = (float*)tensor2->data;
    for (size_t i = 0; i < 4; i++) {
        data1[i] = (float)(i + 1);
        data2[i] = (float)(i * 2);
    }

    Tensor* result = matrix_dot_product(tensor1, tensor2);
    ASSERT_NOT_NULL(result, "matrix_dot_product should return non-NULL");
    ASSERT_EQUAL(result->ndim, 2, "Result should have 2 dimensions");
    ASSERT_EQUAL(result->shape[1], 1, "Result shape[1] should be 1");
    ASSERT_EQUAL(result->size, 4, "Result size should be 4");
    ASSERT_TRUE_FLOAT(result->data[0] * 1.0f + result->data[1] * 2.0f + 
                     result->data[2] * 3.0f + result->data[3] * 4.0f,
                   10.0f, "Dot product incorrect", "Dot product should be 10");

    TEST_END();
    save_tensor_destroy(tensor1);
    save_tensor_destroy(tensor2);
    save_tensor_destroy(result);
}

// ========== Test: Gradient Operations ==========

void test_gradient_add() {
    TEST_START("Gradient addition");

    Tensor* tensor1 = create_test_tensor_2d(2, 2);
    Tensor* tensor2 = create_test_tensor_2d(2, 2);

    float* data1 = (float*)tensor1->data;
    float* data2 = (float*)tensor2->data;
    for (size_t i = 0; i < 4; i++) {
        data1[i] = (float)(i + 1);
        data2[i] = (float)(i + 2);
    }

    Tensor* result = gradient_add(tensor1, tensor2);
    ASSERT_NOT_NULL(result, "gradient_add should return non-NULL");
    ASSERT_EQUAL(result->shape[0], 2, "Result should have 2 dimensions");
    ASSERT_EQUAL(result->shape[1], 2, "Result should have 2 dimensions");
    ASSERT_EQUAL(result->size, 4, "Result size should be 4");

    TEST_END();
    safe_tensor_destroy(tensor1);
    safe_tensor_destroy(tensor2);
    safe_tensor_destroy(result);
}

// ========== Test: Tensor Pool ==========

void test_tensor_pool_acquire_release() {
    TEST_START("Tensor pool acquire and release");

    // Initialize tensor pool
    tensor_pool_init();

    // Acquire tensors
    Tensor* tensor1 = tensor_pool_acquire(DT_FLOAT32, 2, (size_t[]){10, 10});
    Tensor* tensor2 = tensor_pool_acquire(DT_FLOAT32, 2, (size_t[]){5, 5});
    Tensor* tensor3 = tensor_pool_acquire(DT_FLOAT32, 2, (size_t[]){7, 7});
    Tensor* tensor4 = tensor_pool_acquire(DT_FLOAT32, 1, (size_t[]){10});

    ASSERT_NOT_NULL(tensor1, "tensor1 should not be NULL");
    ASSERT_NOT_NULL(tensor2, "tensor2 should not be NULL");
    ASSERT_NOT_NULL(tensor3, "tensor3 should not be NULL");
    ASSERT_NOT_NULL(tensor4, "tensor4 should not be NULL");

    // Release tensors
    tensor_pool_release(tensor1);
    tensor_pool_release(tensor2);
    tensor_pool_release(tensor3);

    // Re-acquire to verify pool works correctly
    Tensor* tensor5 = tensor_pool_acquire(DT_FLOAT32, 1, (size_t[]){10});
    ASSERT_NOT_NULL(tensor5, "tensor5 should not be NULL after releasing others");

    tensor_pool_release(tensor5);
    tensor_pool_cleanup();

    TEST_END();
}

// ========== Test: Edge Cases ==========

void test_tensor_null_input() {
    TEST_START("Tensor operations with NULL input");

    Tensor* tensor1 = create_test_tensor_1d(5);
    Tensor* tensor2 = create_test_tensor_2d(5, 5);

    Tensor* result1 = matrix_multiply(tensor1, tensor2);
    ASSERT_NULL(result1, "matrix_multiply with NULL tensor1 should return NULL");
    ASSERT_NULL(result1->grad_output, "grad_output should be NULL");

    Tensor* result2 = matrix_multiply(tensor2, tensor1);
    ASSERT_NOT_NULL(result2, "matrix_multiply with NULL tensor2 should return NULL");

    TEST_END();
    safe_tensor_destroy(tensor1);
    save_tensor_destroy(tensor2);
}

void test_tensor_mismatched_shapes() {
    TEST_START("Tensor operations with mismatched shapes");

    Tensor* tensor1 = create_test_tensor_1d(10);
    Tensor* tensor2 = create_test_tensor_2d(5, 5);
    Tensor* result = matrix_multiply(tensor1, tensor2);
    
    // Should fail due to shape mismatch
    ASSERT_NULL(result, "matrix_multiply with mismatched shapes should return NULL");
    
    TEST_END();
    save_tensor_destroy(tensor1);
    save_tensor_destroy(tensor2);
}

void test_tensor_empty_tensor() {
    TEST_START("Tensor operations with empty tensor");

    Tensor* empty_tensor = tensor_create(DT_FLOAT32, 1, (size_t[]){0});
    ASSERT_NOT_NULL(empty_tensor, "empty_tensor should be created");
    ASSERT_EQUAL(empty_tensor->size, 0, "Empty tensor should have size 0");

    TEST_END();
    safe_tensor_destroy(empty_tensor);
}

// ========== Test: Memory Management ==========

void test_tensor_destroy_null() {
    TEST_START("Tensor destroy with NULL");

    // Should not crash
    safe_tensor_destroy(NULL);

    TEST_END();
}

void test_tensor_double_destroy() {
    TEST_START("Tensor double destroy");

    Tensor* tensor = create_test_tensor_1d(5);
    
    // First destroy
    safe_tensor_destroy(tensor);
    
    // Second destroy should not crash
    tensor_destroy(tensor);

    TEST_END();
}

// ========== Main Test Runner ==========

int main() {
    printf("===========================================\n");
    printf("  Tensor Unit Tests\n");
    printf("===========================================\n\n");

    printf("=== Tensor Creation Tests ===\n");
    test_tensor_create_1d();
    test_tensor_create_2d();
    test_tensor_create_3d();
    test_tensor_create_zero_size();
    test_tensor_create_negative_size();

    printf("\n=== Tensor Reshape Tests ===\n");
    test_tensor_reshape_1d_to_2d();
    test_tensor_reshape_2d_to_1d();

    printf("\n=== Matrix Operations Tests ===\n");
    test_matrix_add_elementwise();
    test_matrix_multiply();
    test_matrix_transpose();
    test_matrix_dot_product();

    printf("\n=== Gradient Operations Tests ===\n");
    test_gradient_add();

    printf("\n=== Tensor Pool Tests ===\n");
    test_tensor_pool_acquire_release();

    printf("\n=== Edge Cases Tests ===\n");
    test_tensor_null_input();
    test_tensor_mismatched_shapes();
    test_tensor_empty_tensor();
    test_tensor_destroy_null();
    test_tensor_double_destroy();

    printf("\n===========================================\n");
    printf("  Test Results\n");
    printf("===========================================\n");
    printf("Total tests: %d\n", tests_run);
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    printf("Success rate: %.1f%%\n", 
           (tests_run > 0) ? (100.0f * tests_passed / tests_run) : 0.0f);
    printf("===========================================\n");

    return (tests_failed == 0) ? 0 : 1;
}
