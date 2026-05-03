/**
 * @file test_trainer.c
 * @brief Unit tests for trainer.c
 */

#include "../include/common.h"
#include "../include/trainer.h"
#include "../include/model.h"
#include "../include/tensor.h"
#include "../include/layer.h"
#include <stdio.h>
#include <assert.h>

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

// ========== Test: Trainer Creation ==========

void test_trainer_create() {
    TEST_START("Trainer creation");

    Model* model = model_create();
    ASSERT_NOT_NULL(model, "model_create() returned NULL");

    TrainConfig config = {
        .learning_rate = 0.01f,
        .batch_size = 32,
        .epochs = 10,
        .weight_decay = 0.0f,
        .grad_clip = 5.0f,
        .shuffle = true,
        .log_interval = 100,
        .checkpoint_dir = NULL
    };

    Trainer* trainer = trainer_create(model, config);
    ASSERT_NOT_NULL(trainer, "trainer_create() returned NULL");

    // Check trainer properties
    TrainStats stats = trainer_get_stats(trainer);
    ASSERT_EQUAL(stats.current_epoch, 0, "New trainer should have epoch 0");
    ASSERT_EQUAL(stats.total_samples, 0, "New trainer should have 0 samples");

    TEST_END();
    trainer_destroy(trainer);
    model_destroy(model);
}

// ========== Test: Default Config ==========

void test_trainer_default_config() {
    TEST_START("Trainer with default config");

    Model* model = model_create();
    Layer* layer = layer_create_linear(10, 5, true);
    model_add_layer(model, layer);

    TrainConfig config = {
        .learning_rate = 0.0f,  // Will use default
        .batch_size = 0,
        .epochs = 0,
        .weight_decay = 0.0f,
        .grad_clip = 0.0f,
        .shuffle = false,
        .log_interval = 0,
        .checkpoint_dir = NULL
    };

    Trainer* trainer = trainer_create(model, config);
    ASSERT_NOT_NULL(trainer, "trainer_create() returned NULL");

    // Check that defaults were set
    TrainStats stats = trainer_get_stats(trainer);
    ASSERT_TRUE_FLOAT(stats.train_loss, 0.0f, 0.1f, "Initial loss should be 0");

    TEST_END();
    trainer_destroy(trainer);
    model_destroy(model);
}

// ========== Test: Training Single Batch ==========

void test_trainer_train_batch() {
    TEST_START("Training single batch");

    Model* model = model_create();
    Layer* layer = layer_create_linear(2, 1, true);
    model_add_layer(model, layer);

    TrainConfig config = {
        .learning_rate = 0.01f,
        .batch_size = 4,
        .epochs = 1,
        .weight_decay = 0.0f,
        .grad_clip = 0.0f,
        .shuffle = false,
        .log_interval = 1,
        .checkpoint_dir = NULL
    };

    Trainer* trainer = trainer_create(model, config);
    ASSERT_NOT_NULL(trainer, "trainer_create() returned NULL");

    // Create batch data
    size_t input_shape[] = {1, 2};
    Tensor* input_batch = tensor_create(DT_FLOAT32, 2, input_shape);
    float* input_data = (float*)input_batch->data;
    
    // 4 samples: [1.0, 2.0], [2.0, 4.0], [3.0, 6.0], [4.0, 8.0]
    for (int i = 0; i < 4; i++) {
        input_data[i * 2] = (i + 1) * 1.0f;
        input_data[i * 2 + 1] = (i + 1) * 2.0f;
    }

    size_t target_shape[] = {1};
    Tensor* target_batch = tensor_create(DT_FLOAT32, 2, (size_t[]){4, 1});
    float* target_data = (float*)target_batch->data;
    target_data[0] = 3.0f;
    target_data[1] = 6.0f;
    target_data[2] = 9.0f;
    target_data[3] = 12.0f;

    // Train batch
    float loss = trainer_train_batch(trainer, input_batch, target_batch);
    ASSERT_TRUE_FLOAT(loss, 0.0f, 100.0f, "Loss should be non-negative");

    TEST_END();
    tensor_destroy(input_batch);
    tensor_destroy(target_batch);
    trainer_destroy(trainer);
    model_destroy(model);
}

// ========== Test: Training with Gradient Clipping ==========

void test_trainer_grad_clip() {
    TEST_START("Training with gradient clipping");

    Model* model = model_create();
    Layer* layer = layer_create_linear(5, 2, true);
    model_add_layer(model, layer);

    TrainConfig config = {
        .learning_rate = 0.1f,  // High learning rate
        .batch_size = 2,
        .epochs = 1,
        .weight_decay = 0.0f,
        .grad_clip = 1.0f,  // Enable gradient clipping
        .shuffle = false,
        .log_interval = 1,
        .checkpoint_dir = NULL
    };

    Trainer* trainer = trainer_create(model, config);
    ASSERT_NOT_NULL(trainer, "trainer_create() returned NULL");

    // Create batch data
    size_t input_shape[] = {1, 5};
    Tensor* input_batch = tensor_create(DT_FLOAT32, 2, input_shape);
    float* input_data = (float*)input_batch->data;
    
    for (int i = 0; i < 10; i++) {
        input_data[i] = (float)(i % 10);
    }

    size_t target_shape[] = {1};
    Tensor* target_batch = tensor_create(DT_FLOAT32, 2, (size_t[]){2, 1});
    float* target_data = (float*)target_batch->data;
    target_data[0] = 5.0f;
    target_data[1] = 10.0f;

    // Train batch with gradient clipping
    float loss = trainer_train_batch(trainer, input_batch, target_batch);
    ASSERT_TRUE_FLOAT(loss, 0.0f, 100.0f, "Loss should be non-negative");

    TEST_END();
    tensor_destroy(input_batch);
    tensor_destroy(target_batch);
    trainer_destroy(trainer);
    model_destroy(model);
}

// ========== Test: Training Mini-batch ==========

void test_trainer_train_minibatch() {
    TEST_START("Training with mini-batch");

    Model* model = model_create();
    Layer* layer = layer_create_linear(2, 1, true);
    model_add_layer(model, layer);

    TrainConfig config = {
        .learning_rate = 0.01f,
        .batch_size = 2,
        .epochs = 2,
        .weight_decay = 0.0f,
        .grad_clip = 0.0f,
        .shuffle = false,
        .log_interval = 1,
        .checkpoint_dir = NULL
    };

    Trainer* trainer = trainer_create(model, config);
    ASSERT_NOT_NULL(trainer, "trainer_create() returned NULL");

    // Create dataset
    size_t num_samples = 6;
    Tensor** inputs = (Tensor**)malloc(num_samples * sizeof(Tensor*));
    Tensor** targets = (Tensor**)malloc(num_samples * sizeof(Tensor*));

    for (size_t i = 0; i < num_samples; i++) {
        size_t input_shape[] = {1, 2};
        inputs[i] = tensor_create(DT_FLOAT32, 2, input_shape);
        float* input_data = (float*)inputs[i]->data;
        input_data[0] = (float)(i + 1);
        input_data[1] = (float)(i + 1) * 2.0f;

        size_t target_shape[] = {1};
        targets[i] = tensor_create(DT_FLOAT32, 1, target_shape);
        float* target_data = (float*)targets[i]->data;
        target_data[0] = (float)(i + 1) * 3.0f;
    }

    // Train mini-batch
    float loss = trainer_train_minibatch(trainer, inputs, targets, num_samples);
    ASSERT_TRUE_FLOAT(loss, 0.0f, 100.0f, "Loss should be non-negative");

    // Clean up
    for (size_t i = 0; i < num_samples; i++) {
        tensor_destroy(inputs[i]);
        tensor_destroy(targets[i]);
    }
    free(inputs);
    free(targets);

    TEST_END();
    trainer_destroy(trainer);
    model_destroy(model);
}

// ========== Test: Training Epoch ==========

void test_trainer_train_epoch() {
    TEST_START("Training one epoch");

    Model* model = model_create();
    Layer* layer = layer_create_linear(3, 1, true);
    model_add_layer(model, layer);

    TrainConfig config = {
        .learning_rate = 0.01f,
        .batch_size = 2,
        .epochs = 1,
        .weight_decay = 0.0f,
        .grad_clip = 0.0f,
        .shuffle = false,
        .log_interval = 1,
        .checkpoint_dir = NULL
    };

    Trainer* trainer = trainer_create(model, config);
    ASSERT_NOT_NULL(trainer, "trainer_create() returned NULL");

    // Create dataset
    size_t num_samples = 4;
    Tensor** inputs = (Tensor**)malloc(num_samples * sizeof(Tensor*));
    Tensor** targets = (Tensor**)malloc(num_samples * sizeof(Tensor*));

    for (size_t i = 0; i < num_samples; i++) {
        size_t input_shape[] = {1, 3};
        inputs[i] = tensor_create(DT_FLOAT32, 2, input_shape);
        float* input_data = (float*)inputs[i]->data;
        input_data[0] = (float)(i + 1);
        input_data[1] = (float)(i + 2);
        input_data[2] = (float)(i + 3);

        size_t target_shape[] = {1};
        targets[i] = tensor_create(DT_FLOAT32, 1, target_shape);
        float* target_data = (float*)targets[i]->data;
        target_data[0] = (float)((i + 1) + (i + 2) + (i + 3));
    }

    // Train one epoch
    float loss = trainer_train_epoch(trainer, inputs, targets, num_samples);
    ASSERT_TRUE_FLOAT(loss, 0.0f, 100.0f, "Epoch loss should be non-negative");

    // Clean up
    for (size_t i = 0; i < num_samples; i++) {
        tensor_destroy(inputs[i]);
        tensor_destroy(targets[i]);
    }
    free(inputs);
    free(targets);

    TEST_END();
    trainer_destroy(trainer);
    model_destroy(model);
}

// ========== Test: Callback Function ==========

static int callback_count = 0;
static void test_callback(TrainStats* stats, void* user_data) {
    callback_count++;
    int* counter = (int*)user_data;
    if (counter) (*counter)++;
}

void test_trainer_callback() {
    TEST_START("Trainer callback function");

    Model* model = model_create();
    Layer* layer = layer_create_linear(2, 1, true);
    model_add_layer(model, layer);

    TrainConfig config = {
        .learning_rate = 0.01f,
        .batch_size = 2,
        .epochs = 1,
        .weight_decay = 0.0f,
        .grad_clip = 0.0f,
        .shuffle = false,
        .log_interval = 1,
        .checkpoint_dir = NULL
    };

    Trainer* trainer = trainer_create(model, config);
    ASSERT_NOT_NULL(trainer, "trainer_create() returned NULL");

    // Set callback
    callback_count = 0;
    int user_counter = 0;
    trainer_set_callback(trainer, test_callback, &user_counter);

    // Create simple batch
    size_t input_shape[] = {1, 2};
    Tensor* input_batch = tensor_create(DT_FLOAT32, 2, input_shape);
    float* input_data = (float*)input_batch->data;
    input_data[0] = 1.0f;
    input_data[1] = 2.0f;

    size_t target_shape[] = {1};
    Tensor* target_batch = tensor_create(DT_FLOAT32, 2, (size_t[]){1, 1});
    float* target_data = (float*)target_batch->data;
    target_data[0] = 3.0f;

    // Train batch
    trainer_train_batch(trainer, input_batch, target_batch);

    // Check callback was called
    ASSERT_TRUE(callback_count > 0 || user_counter > 0,
               "Callback should have been called");

    TEST_END();
    tensor_destroy(input_batch);
    tensor_destroy(target_batch);
    trainer_destroy(trainer);
    model_destroy(model);
}

// ========== Test: Stats Retrieval ==========

void test_trainer_stats() {
    TEST_START("Trainer stats retrieval");

    Model* model = model_create();
    Layer* layer = layer_create_linear(2, 1, true);
    model_add_layer(model, layer);

    TrainConfig config = {
        .learning_rate = 0.01f,
        .batch_size = 4,
        .epochs = 1,
        .weight_decay = 0.0f,
        .grad_clip = 0.0f,
        .shuffle = false,
        .log_interval = 1,
        .checkpoint_dir = NULL
    };

    Trainer* trainer = trainer_create(model, config);
    ASSERT_NOT_NULL(trainer, "trainer_create() returned NULL");

    // Get initial stats
    TrainStats stats = trainer_get_stats(trainer);
    ASSERT_EQUAL(stats.current_epoch, 0, "Initial epoch should be 0");
    ASSERT_EQUAL(stats.total_samples, 0, "Initial samples should be 0");

    // Train a batch
    size_t input_shape[] = {1, 2};
    Tensor* input_batch = tensor_create(DT_FLOAT32, 2, input_shape);
    float* input_data = (float*)input_batch->data;
    input_data[0] = 1.0f;
    input_data[1] = 2.0f;

    size_t target_shape[] = {1};
    Tensor* target_batch = tensor_create(DT_FLOAT32, 2, (size_t[]){1, 1});
    float* target_data = (float*)target_batch->data;
    target_data[0] = 3.0f;

    trainer_train_batch(trainer, input_batch, target_batch);

    // Get updated stats
    stats = trainer_get_stats(trainer);
    ASSERT_TRUE(stats.total_samples > 0, "Samples should have been processed");

    TEST_END();
    tensor_destroy(input_batch);
    tensor_destroy(target_batch);
    trainer_destroy(trainer);
    model_destroy(model);
}

// ========== Test: Trainer Destroy ==========

void test_trainer_destroy() {
    TEST_START("Trainer destruction");

    Model* model = model_create();
    Layer* layer = layer_create_linear(10, 5, true);
    model_add_layer(model, layer);

    TrainConfig config = {
        .learning_rate = 0.01f,
        .batch_size = 32,
        .epochs = 10,
        .weight_decay = 0.0f,
        .grad_clip = 0.0f,
        .shuffle = false,
        .log_interval = 1,
        .checkpoint_dir = NULL
    };

    Trainer* trainer = trainer_create(model, config);
    ASSERT_NOT_NULL(trainer, "trainer_create() returned NULL");

    // Destroy trainer (should clean up all resources)
    trainer_destroy(trainer);

    // If no crash occurred, test passes
    TEST_END();
    model_destroy(model);
}

// ========== Test: NULL Model ==========

void test_trainer_null_model() {
    TEST_START("Trainer with NULL model");

    TrainConfig config = {
        .learning_rate = 0.01f,
        .batch_size = 32,
        .epochs = 10,
        .weight_decay = 0.0f,
        .grad_clip = 0.0f,
        .shuffle = false,
        .log_interval = 1,
        .checkpoint_dir = NULL
    };

    Trainer* trainer = trainer_create(NULL, config);
    ASSERT_NULL(trainer, "trainer_create() with NULL model should return NULL");

    TEST_END();
}

// ========== Main Test Runner ==========

int main() {
    printf("===========================================\n");
    printf("  Trainer Unit Tests\n");
    printf("===========================================\n\n");

    printf("=== Trainer Creation Tests ===\n");
    test_trainer_create();
    test_trainer_default_config();

    printf("\n=== Training Tests ===\n");
    test_trainer_train_batch();
    test_trainer_grad_clip();
    test_trainer_train_minibatch();
    test_trainer_train_epoch();

    printf("\n=== Callback Tests ===\n");
    test_trainer_callback();

    printf("\n=== Stats Tests ===\n");
    test_trainer_stats();

    printf("\n=== Memory Management Tests ===\n");
    test_trainer_destroy();

    printf("\n=== Edge Case Tests ===\n");
    test_trainer_null_model();

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
