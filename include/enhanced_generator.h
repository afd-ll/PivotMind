/**
 * @file enhanced_generator.h
 * @brief 增强的文本生成器
 */

#ifndef ENHANCED_GENERATOR_H
#define ENHANCED_GENERATOR_H

#include "multi_topology.h"

// 增强的生成函数
char* enhanced_generate(MasterTopology* master, const char* input_text, int max_output_len);

#endif // ENHANCED_GENERATOR_H
