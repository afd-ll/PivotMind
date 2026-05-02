#include "../include/concept_processor.h"
#include "../include/multi_topology.h"
#include "../include/huarong_topology.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

// ==================== 类型识别辅助 ====================

// 预定义的字符集，避免重复计算
static const char* OPERATOR_CHARS = "+-*/=%^";
static const char* CAUSAL_KEYWORDS[] = {"因为", "所以", "导致", "引起", "造成", NULL};
static const char* RELATION_KEYWORDS[] = {"是", "属于", "包含", "有", "具有", NULL};

// 快速检查是否为操作符
static inline bool is_operator_char(char c) {
    return strchr(OPERATOR_CHARS, c) != NULL;
}

// 检查是否包含关键词（更高效）
static bool contains_keyword(const char* text, const char** keywords) {
    if (!text) return false;
    for (int i = 0; keywords[i] != NULL; i++) {
        if (strstr(text, keywords[i])) return true;
    }
    return false;
}

bool concept_is_number(const char* text) {
    if (!text || !*text) return false;
    
    int has_digit = 0;
    int has_dot = 0;
    
    for (int i = 0; text[i]; i++) {
        if (isdigit(text[i])) {
            has_digit = 1;
        } else if (text[i] == '.' && !has_dot) {
            has_dot = 1;
        } else if (text[i] == '-' && i == 0) {
            continue;
        } else {
            return false;
        }
    }
    
    return has_digit;
}

bool concept_is_math_expression(const char* text) {
    if (!text) return false;

    bool has_operator = false;
    bool has_digit = false;

    for (const char* p = text; *p; p++) {
        if (is_operator_char(*p)) {
            has_operator = true;
        }
        if (isdigit(*p)) {
            has_digit = true;
        }
    }

    return has_operator && has_digit;
}

// ==================== 递归下降表达式解析器 ====================
// 支持 +, -, *, / 运算符优先级和括号

typedef struct {
    const char* expr;
    int pos;
} ExprParser;

// 前向声明
static float parse_expr_additive(ExprParser* p);

static void parser_skip_spaces(ExprParser* p) {
    while (p->expr[p->pos] == ' ') p->pos++;
}

static float parse_number(ExprParser* p) {
    parser_skip_spaces(p);

    float sign = 1.0f;
    if (p->expr[p->pos] == '-') {
        sign = -1.0f;
        p->pos++;
    } else if (p->expr[p->pos] == '+') {
        p->pos++;
    }

    parser_skip_spaces(p);

    float result = 0.0f;
    bool has_digit = false;

    // 整数部分
    while (isdigit(p->expr[p->pos])) {
        result = result * 10.0f + (p->expr[p->pos] - '0');
        p->pos++;
        has_digit = true;
    }

    // 小数部分
    if (p->expr[p->pos] == '.') {
        p->pos++;
        float frac = 0.1f;
        while (isdigit(p->expr[p->pos])) {
            result += (p->expr[p->pos] - '0') * frac;
            frac *= 0.1f;
            p->pos++;
            has_digit = true;
        }
    }

    if (!has_digit) return 0.0f;
    return result * sign;
}

// 解析原子：数字或括号表达式
static float parse_atom(ExprParser* p) {
    parser_skip_spaces(p);

    if (p->expr[p->pos] == '(') {
        p->pos++; // 跳过 '('
        float result = parse_expr_additive(p);
        parser_skip_spaces(p);
        if (p->expr[p->pos] == ')') {
            p->pos++; // 跳过 ')'
        }
        return result;
    }

    return parse_number(p);
}

// 解析乘除（高优先级）
static float parse_expr_multiplicative(ExprParser* p) {
    float result = parse_atom(p);

    parser_skip_spaces(p);
    while (p->expr[p->pos] == '*' || p->expr[p->pos] == '/') {
        char op = p->expr[p->pos++];
        float right = parse_atom(p);

        if (op == '*') {
            result *= right;
        } else {
            if (right != 0.0f) {
                result /= right;
            }
        }
        parser_skip_spaces(p);
    }

    return result;
}

// 解析加减（低优先级）
static float parse_expr_additive(ExprParser* p) {
    float result = parse_expr_multiplicative(p);

    parser_skip_spaces(p);
    while (p->expr[p->pos] == '+' || p->expr[p->pos] == '-') {
        char op = p->expr[p->pos++];
        float right = parse_expr_multiplicative(p);

        if (op == '+') {
            result += right;
        } else {
            result -= right;
        }
        parser_skip_spaces(p);
    }

    return result;
}

static float eval_simple_expr(const char* expr) {
    if (!expr) return 0;

    ExprParser p = { .expr = expr, .pos = 0 };
    return parse_expr_additive(&p);
}

char* concept_calculate(const char* expression) {
    if (!expression) return NULL;
    
    printf("[概念处理] 检测到数学表达式: %s\n", expression);
    
    float result = eval_simple_expr(expression);
    
    char* output = (char*)malloc(64);
    snprintf(output, 64, "%.6g", result);
    
    printf("[概念处理] 计算结果: %s\n", output);
    
    return output;
}

ConceptValue* concept_parse(const char* text) {
    if (!text) return NULL;

    ConceptValue* cv = (ConceptValue*)calloc(1, sizeof(ConceptValue));
    if (!cv) return NULL;

    cv->raw_value = strdup(text);
    if (!cv->raw_value) {
        free(cv);
        return NULL;
    }

    // 按优先级检查类型
    if (concept_is_number(text)) {
        cv->type = CONCEPT_TYPE_NUMBER;
        cv->numeric_value = atof(text);
    } else if (concept_is_math_expression(text)) {
        cv->type = CONCEPT_TYPE_NUMBER;
        cv->numeric_value = eval_simple_expr(text);
    } else if (contains_keyword(text, CAUSAL_KEYWORDS)) {
        cv->type = CONCEPT_TYPE_CAUSAL;
    } else if (contains_keyword(text, RELATION_KEYWORDS)) {
        cv->type = CONCEPT_TYPE_RELATION;
    } else if (is_operator_char(text[0]) || is_operator_char(text[strlen(text)-1])) {
        cv->type = CONCEPT_TYPE_OPERATOR;
    } else {
        cv->type = CONCEPT_TYPE_ENTITY;
    }

    return cv;
}

void concept_value_free(ConceptValue* cv) {
    if (!cv) return;
    if (cv->raw_value) free(cv->raw_value);
    if (cv->unit) free(cv->unit);
    free(cv);
}

void concept_learn_rule(MasterTopology* master, const char* input, const char* result) {
    if (!master || !input || !result) return;
    
    printf("[概念处理] 学习规则: %s = %s\n", input, result);
    
    int topo_id = -1;
    for (int i = 0; i < master->sub_topo_count; i++) {
        if (master->sub_topologies[i]->type == TOPO_CONCEPT) {
            topo_id = i;
            break;
        }
    }
    
    if (topo_id < 0) {
        printf("[概念处理] 未找到概念拓扑\n");
        return;
    }
    
    SubTopology* sub = master->sub_topologies[topo_id];
    if (!sub || !sub->net) return;
    
    ReasoningNode* input_node = huarong_net_add_node(sub->net, input, NULL, 0);
    ReasoningNode* result_node = huarong_net_add_node(sub->net, result, NULL, 0);
    
    if (input_node && result_node) {
        huarong_net_add_connection(sub->net, input_node->node_id, result_node->node_id, 1.0f);
        printf("[概念处理] 已创建概念连接: %s → %s\n", input, result);
    }
}
