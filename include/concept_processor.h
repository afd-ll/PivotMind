#ifndef CONCEPT_PROCESSOR_H
#define CONCEPT_PROCESSOR_H

#include "multi_topology.h"
#include <stdbool.h>

typedef enum {
    CONCEPT_TYPE_UNKNOWN,
    CONCEPT_TYPE_NUMBER,      // 数值: 1, 2, 3.14
    CONCEPT_TYPE_OPERATOR,    // 运算符: +, -, *, /, =, >, <
    CONCEPT_TYPE_RULE,        // 规则: 如果...那么...
    CONCEPT_TYPE_ENTITY,      // 实体: 狗、水、北京
    CONCEPT_TYPE_RELATION,    // 关系: 属于、包含
    CONCEPT_TYPE_CAUSAL       // 因果: 加热→沸腾
} ConceptType;

typedef struct {
    ConceptType type;
    char* raw_value;
    float numeric_value;
    char* unit;
} ConceptValue;

bool concept_is_math_expression(const char* text);

bool concept_is_number(const char* text);

char* concept_calculate(const char* expression);

void concept_learn_rule(MasterTopology* master, const char* input, const char* result);

ConceptValue* concept_parse(const char* text);

void concept_value_free(ConceptValue* cv);

#endif
