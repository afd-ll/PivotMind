/**
 * @file model_io.h
 * @brief Model I/O functions for saving and loading models
 * @version 1.0
 */

#ifndef MODEL_IO_H
#define MODEL_IO_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "layer.h"
#include "model.h"

// Model format version
#define MODEL_FORMAT_VERSION 1

// Model file magic number
#define MODEL_MAGIC 0x4D4F444C // 'MODL'

// Metadata structure
typedef struct ModelMetadata {
    char name[64];
    char description[256];
    int version;
    int num_params;
    uint64_t param_size_bytes;
    char created_at[64];
} ModelMetadata;

// Save model to file with metadata
bool model_save(Model* model, const char* filepath, ModelMetadata* metadata);

// Load model from file
Model* model_load(const char* filepath);

// Save model weights only
bool model_save_weights(Model* model, const char* filepath);

// Load model weights only
bool model_load_weights(Model* model, const char* filepath);

// Save model checkpoint with epoch and loss
bool model_save_checkpoint(Model* model, const char* filepath, int epoch, float loss);

// Load model checkpoint with epoch and loss
bool model_load_checkpoint(Model* model, const char* filepath, int* epoch, float* loss);

// Export model to text format
bool model_export_text(Model* model, const char* filepath);

// Compare two models for equality
bool model_equal(Model* model1, Model* model2, float tolerance);

#endif // MODEL_IO_H
