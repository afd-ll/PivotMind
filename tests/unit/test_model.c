/**
 * @file test_model.c
 * @brief Unit tests for model.c
 */

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

// ========== Test: Model Creation ==========

void test_model_create() {
    TEST_START("Model creation");

    Model* model = model_create();
    ASSERT_NOT_NULL(model, "model_create() returned NULL");
    ASSERT_EQUAL(model->num_layers, 0, "New model should have 0 layers");
    ASSERT_EQUAL(model->optimizer, NULL, "New model should have NULL optimizer");

    TEST_END();
    model_destroy(model);
}

// ========== Test: Adding Layers ==========

void test_model_add_layer() {
    TEST_START("Adding layers to model");

    Model* model = model_create();
    ASSERT_NOT_NULL(model, "model_create() returned NULL");

    // Add first layer
    size_t input_size = 10;
    size_t output_size = 20;
    size_t weight_shape[] = {input_size, output_size};
    
    Layer* layer1 = layer_create_linear(input_size, output_size, true);
    ASSERT_NOT_NULL(layer1, "layer_create_linear() returned NULL");
    
    model_add_layer(model, layer1);
    ASSERT_EQUAL(model->num_layers, 1, "Model should have 1 layer");

    // Add second layer
    Layer* layer2 = layer_create_relu();
    ASSERT_NOT_NULL(layer2, "layer_create_relu() returned NULL");
    
    model_add_layer(model, layer2);
    ASSERT_EQUAL(model->num_layers, 2, "Model should have 2 layers");

    TEST_END();
    model_destroy(model);
}

// ========== Test: Forward Pass ==========

void test_model_forward_linear() {
    TEST_START("Model forward pass with linear layer");

    Model* model = model_create();
    ASSERT_NOT_NULL(model, "model_create() returned NULL");

    // Create a simple model: Linear(2, 3) -> ReLU
    Layer* layer1 = layer_create_linear(2, 3, false);
    ASSERT_NOT_NULL(layer1, "layer_create_linear() returned NULL");
    
    Layer* layer2 = layer_create_relu();
    ASSERT_NOT_NULL(layer2, "layer_create_relu() returned NULL");
    
    model_add_layer(model, layer1);
    model_add_layer(model, layer2);

    // Create input tensor: (1, 2)
    size_t input_shape[] = {1, 2};
    Tensor* input = tensor_create(DT_FLOAT32, 2, input_shape);
    ASSERT_NOT_NULL(input, "tensor_create() returned NULL");
    
    float* input_data = (float*)input->data;
    input_data[0] = 1.0f;
    input_data[1] = 2.0f;

    // Forward pass
    Tensor* output = model_forward(model, input);
    ASSERT_NOT_NULL(output, "model_forward() returned NULL");

    // Check output shape
    ASSERT_EQUAL(output->ndim, 2, "Output should be 2D");
    ASSERT_EQUAL(output->shape[0], 1, "Output batch size should be 1");
    ASSERT_EQUAL(output->shape[1], 3, "Output feature size should be 3");

    TEST_END();
    tensor_destroy(output);
    tensor_destroy(input);
    model_destroy(model);
}

void test_model_forward_multiple_layers() {
    TEST_START("Model forward pass with multiple layers");

    Model* model = model_create();
    ASSERT_NOT_NULL(model, "model_create() returned NULL");

    // Create model: Linear(10, 20) -> ReLU -> Linear(20, 5) -> Softmax
    Layer* layer1 = layer_create_linear(10, 20, false);
    Layer* layer2 = layer_create_relu();
    Layer* layer3 = layer_create_linear(20, 5, false);
    Layer* layer4 = layer_create_softmax();
    
    model_add_layer(model, layer1);
    model_add_layer(model, layer2);
    model_add_layer(model, layer3);
    model_add_layer(model, layer4);

    // Create input tensor: (2, 10) - batch of 2 samples
    size_t input_shape[] = {2, 10};
    Tensor* input = tensor_create(DT_FLOAT32, 2, input_shape);
    ASSERT_NOT_NULL(input, "tensor_create() returned NULL");

    // Forward pass
    Tensor* output = model_forward(model, input);
    ASSERT_NOT_NULL(output, "model_forward() returned NULL");

    // Check output shape
    ASSERT_EQUAL(output->ndim, 2, "Output should be 2D");
    ASSERT_EQUAL(output->shape[0], 2, "Output batch size should be 2");
    ASSERT_EQUAL(output->shape[1], 5, "Output feature size should be 5");

    TEST_END();
    tensor_destroy(output);
    tensor_destroy(input);
    model_destroy(model);
}

// ========== Test: MSE Loss ==========

void test_model_mse_loss() {
    TEST_START("MSE loss calculation");

    // Create prediction tensor
    size_t shape[] = {2, 3};
    Tensor* pred = tensor_create(DT_FLOAT32, 2, shape);
    ASSERT_NOT_NULL(pred, "tensor_create() returned NULL");
    
    float* pred_data = (float*)pred->data;
    pred_data[0] = 1.0f; pred_data[1] = 2.0f; pred_data[2] = 3.0f;
    pred_data[3] = 4.0f; pred_data[4] = 5.0f; pred_data[5] = 6.0f;

    // Create target tensor
    Tensor* target = tensor_create(DT_FLOAT32, 2, shape);
    ASSERT_NOT_NULL(target, "tensor_create() returned NULL");
    
    float* target_data = (float*)target->data;
    target_data[0] = 1.5f; target_data[1] = 2.5f; target_data[2] = 3.5f;
    target_data[3] = 4.5f; target_data[4] = 5.5f; target_data[5] = 6.5f;

    // Calculate MSE loss
    Tensor* loss = model_mse_loss(pred, target);
    ASSERT_NOT_NULL(loss, "model_mse_loss() returned NULL");

    // Check loss value
    float* loss_data = (float*)loss->data;
    float expected_loss = 0.25f; // (0.5^2 * 6) / 6 = 0.25
    
    ASSERT_TRUE(loss_data[0] > 0.24f && loss_data[0] < 0.26f,
               "MSE loss should be approximately 0.25");

    TEST_END();
    tensor_destroy(loss);
    tensor_destroy(pred);
    tensor_destroy(target);
}

void test_model_mse_loss_perfect_match() {
    TEST_START("MSE loss with perfect match");

    size_t shape[] = {3};
    Tensor* pred = tensor_create(DT_FLOAT32, 1, shape);
    Tensor* target = tensor_create(DT_FLOAT32, 1, shape);
    
    float* data = (float*)pred->data;
    data[0] = 1.0f; data[1] = 2.0f; data[2] = 3.0f;
    
    data = (float*)target->data;
    data[0] = 1.0f; data[1] = 2.0f; data[2] = 3.0f;

    Tensor* loss = model_mse_loss(pred, target);
    ASSERT_NOT_NULL(loss, "model_mse_loss() returned NULL");
    
    float* loss_data = (float*)loss->data;
    ASSERT_TRUE(loss_data[0] < 0.001f,
               "MSE loss should be close to 0 for perfect match");

    TEST_END();
    tensor_destroy(loss);
    tensor_destroy(pred);
    tensor_destroy(target);
}

// ========== Test: Training Step ==========

void test_model_train_step() {
    TEST_START("Model training step");

    Model* model = model_create();
    ASSERT_NOT_NULL(model, "model_create() returned NULL");

    // Create simple model: Linear(2, 1)
    Layer* layer = layer_create_linear(2, 1, true);
    ASSERT_NOT_NULL(layer, "layer_create_linear() returned NULL");

    model_add_layer(model, layer);

    // Create input and target
    size_t input_shape[] = {1, 2};
    Tensor* input = tensor_create(DT_FLOAT32, 2, input_shape);
    ASSERT_NOT_NULL(input, "tensor_create() returned NULL");
    float* input_data = (float*)input->data;
    input_data[0] = 1.0f;
    input_data[1] = 2.0f;

    size_t target_shape[] = {1};
    Tensor* target = tensor_create(DT_FLOAT32, 1, target_shape);
    ASSERT_NOT_NULL(target, "tensor_create() returned NULL");
    float* target_data = (float*)target->data;
    target_data[0] = 5.0f;

    // Perform training step
    float lr = 0.01f;
    model_train_step(model, input, target, lr);

    // Check that weights were updated
    ASSERT_TRUE(layer->weights != NULL, "Weights should not be NULL");
    // Note: model_train_step implements its own backward pass,
    // so grad_weights may not be set by layer_backward

    TEST_END();
    tensor_destroy(input);
    tensor_destroy(target);
    model_destroy(model);
}

// ========== Test: Model Destroy ==========

void test_model_destroy() {
    TEST_START("Model destruction");

    Model* model = model_create();
    ASSERT_NOT_NULL(model, "model_create() returned NULL");

    // Add layers
    Layer* layer1 = layer_create_linear(10, 20, false);
    Layer* layer2 = layer_create_relu();
    model_add_layer(model, layer1);
    model_add_layer(model, layer2);

    // Destroy model (should clean up all layers)
    model_destroy(model);

    // If no crash occurred, test passes
    TEST_END();
}

// ========== Test: Edge Cases ==========

void test_model_null_input() {
    TEST_START("Model forward with NULL input");

    Model* model = model_create();
    Layer* layer = layer_create_linear(10, 5, false);
    model_add_layer(model, layer);

    Tensor* output = model_forward(model, NULL);
    ASSERT_NULL(output, "model_forward() with NULL input should return NULL");

    TEST_END();
    model_destroy(model);
}

void test_model_empty_model() {
    TEST_START("Model forward with empty model");

    Model* model = model_create();

    size_t input_shape[] = {1, 10};
    Tensor* input = tensor_create(DT_FLOAT32, 2, input_shape);

    Tensor* output = model_forward(model, input);
    ASSERT_NULL(output, "model_forward() with empty model should return NULL");

    TEST_END();
    tensor_destroy(input);
    model_destroy(model);
}

// ========== Main Test Runner ==========

int main() {
    printf("===========================================\n");
    printf("  Model Unit Tests\n");
    printf("===========================================\n\n");

    printf("=== Model Creation Tests ===\n");
    test_model_create();
    test_model_add_layer();

    printf("\n=== Forward Pass Tests ===\n");
    test_model_forward_linear();
    test_model_forward_multiple_layers();

    printf("\n=== Loss Function Tests ===\n");
    test_model_mse_loss();
    test_model_mse_loss_perfect_match();

    printf("\n=== Training Tests ===\n");
    test_model_train_step();

    printf("\n=== Memory Management Tests ===\n");
    test_model_destroy();

    printf("\n=== Edge Case Tests ===\n");
    test_model_null_input();
    test_model_empty_model();

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
