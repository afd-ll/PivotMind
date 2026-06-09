/**
 * @file associative_reasoning.h
 * @brief 联想推理引擎
 */

#ifndef ASSOCIATIVE_REASONING_H
#define ASSOCIATIVE_REASONING_H

// 前向声明（打破 multi_topology ↔ associative_reasoning 循环依赖）
typedef struct MasterTopology MasterTopology;

typedef struct AssociativeEngine AssociativeEngine;

// 创建联想引擎
AssociativeEngine* assoc_engine_create(MasterTopology* topology);

// 释放引擎
void assoc_engine_free(AssociativeEngine* engine);

// 从文本开始联想
int associate_from_text(AssociativeEngine* engine, const char* text, int max_hops);

// 基于联想生成内容
char* generate_from_associations(AssociativeEngine* engine, int max_len,
                                 const char* input_text,
                                 const float* topo_act);

// 打印联想路径
void print_associations(AssociativeEngine* engine);

// 梦境引擎接口：获取联想结果
int assoc_get_count(AssociativeEngine* engine);
const char* assoc_get_concept(AssociativeEngine* engine, int index,
                              float* activation_out, int* topo_type_out,
                              int* hop_count_out);

#endif
