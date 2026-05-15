/**
 * @file associative_reasoning.h
 * @brief 联想推理引擎
 */

#ifndef ASSOCIATIVE_REASONING_H
#define ASSOCIATIVE_REASONING_H

#include "multi_topology.h"

typedef struct AssociativeEngine AssociativeEngine;

// 创建联想引擎
AssociativeEngine* assoc_engine_create(MasterTopology* topology);

// 释放引擎
void assoc_engine_free(AssociativeEngine* engine);

// 从文本开始联想
int associate_from_text(AssociativeEngine* engine, const char* text, int max_hops);

// 基于联想生成内容
char* generate_from_associations(AssociativeEngine* engine, int max_len, const char* input_text);

// 打印联想路径
void print_associations(AssociativeEngine* engine);

#endif
