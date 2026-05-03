#include "cognitive_params.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

// ==================== 工具函数 ====================

float clamp_float(float value, float min_val, float max_val) {
    if (value < min_val) return min_val;
    if (value > max_val) return max_val;
    return value;
}

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

// ==================== 置信度函数 ====================

CognitiveConfidence* cognitive_confidence_create(void) {
    CognitiveConfidence* conf = (CognitiveConfidence*)malloc(sizeof(CognitiveConfidence));
    if (!conf) return NULL;
    conf->predictive_accuracy = 0.0f;
    conf->user_satisfaction = 0.0f;
    conf->novelty_bonus = 0.0f;
    conf->combined = 0.0f;
    return conf;
}

void cognitive_confidence_compute(CognitiveConfidence* conf) {
    if (!conf) return;
    conf->combined = conf->predictive_accuracy * 0.4f +
                     conf->user_satisfaction * 0.3f +
                     conf->novelty_bonus * 0.3f;
    conf->combined = clamp_float(conf->combined, 0.0f, 1.0f);
}

void cognitive_confidence_update(CognitiveConfidence* conf,
                                float predictive,
                                float satisfaction,
                                float novelty) {
    if (!conf) return;
    conf->predictive_accuracy = clamp_float(predictive, 0.0f, 1.0f);
    conf->user_satisfaction = clamp_float(satisfaction, 0.0f, 1.0f);
    conf->novelty_bonus = clamp_float(novelty, 0.0f, 1.0f);
    cognitive_confidence_compute(conf);
}

void cognitive_confidence_destroy(CognitiveConfidence* conf) {
    if (conf) free(conf);
}

// ==================== 效价函数 ====================

float get_feedback_valence(const char* feedback) {
    if (!feedback) return 0.0f;

    float valence = 0.0f;

    if (strstr(feedback, "好") || strstr(feedback, "对") ||
        strstr(feedback, "棒") || strstr(feedback, "赞") ||
        strstr(feedback, "👍") || strstr(feedback, "优秀")) {
        valence += 0.7f;
    }
    if (strstr(feedback, "不好") || strstr(feedback, "不对") ||
        strstr(feedback, "错") || strstr(feedback, "差") ||
        strstr(feedback, "烂") || strstr(feedback, "👎")) {
        valence -= 0.7f;
    }
    if (strstr(feedback, "一般") || strstr(feedback, "还行")) {
        valence += 0.1f;
    }
    if (strstr(feedback, "谢谢") || strstr(feedback, "感谢")) {
        valence += 0.5f;
    }

    return clamp_float(valence, -1.0f, 1.0f);
}

float get_interaction_valence(float response_time, bool is_novel) {
    float valence = 0.0f;

    if (response_time < 0.1f) {
        valence += 0.1f;
    } else if (response_time > 5.0f) {
        valence -= 0.1f;
    }

    if (is_novel) {
        valence += 0.2f;
    }

    return clamp_float(valence, -0.3f, 0.3f);
}

float get_self_assessment_valence(float confidence, float prediction_error) {
    float valence = 0.0f;

    if (confidence > 0.8f) {
        valence += 0.1f;
    } else if (confidence < 0.3f) {
        valence -= 0.1f;
    }

    if (prediction_error < 0.1f) {
        valence += 0.1f;
    } else if (prediction_error > 0.5f) {
        valence -= 0.15f;
    }

    return clamp_float(valence, -0.3f, 0.3f);
}

float compute_valence_from_interaction(Interaction* interaction,
                                      float current_confidence,
                                      bool is_novel_concept) {
    if (!interaction) return 0.0f;

    float valence = 0.0f;

    float feedback_valence = get_feedback_valence(interaction->feedback);
    valence += feedback_valence * 0.5f;

    float interaction_valence = get_interaction_valence(
        interaction->response_time, is_novel_concept);
    valence += interaction_valence * 0.3f;

    float self_assessment_valence = get_self_assessment_valence(
        current_confidence, 1.0f - current_confidence);
    valence += self_assessment_valence * 0.2f;

    float recency_weight = calculate_recency_weight(interaction->timestamp);
    valence *= recency_weight;

    return clamp_float(valence, -1.0f, 1.0f);
}

float calculate_recency_weight(time_t interaction_time) {
    double hours_elapsed = difftime(time(NULL), interaction_time) / 3600.0;
    return (float)exp(-hours_elapsed / 24.0);
}

void node_update_valence(float* current_valence, float new_valence, float learning_rate) {
    if (!current_valence) return;
    float alpha = learning_rate;
    *current_valence = alpha * new_valence + (1.0f - alpha) * (*current_valence);
    *current_valence = clamp_float(*current_valence, -1.0f, 1.0f);
}

// ==================== 边权重函数 ====================

EdgeWeightDual* edge_weight_create(void) {
    EdgeWeightDual* weight = (EdgeWeightDual*)malloc(sizeof(EdgeWeightDual));
    if (!weight) return NULL;
    weight->logical_strength = 0.5f;
    weight->motivational_bias = 0.5f;
    weight->combined = 0.25f;
    return weight;
}

void edge_weight_compute(EdgeWeightDual* weight) {
    if (!weight) return;
    weight->combined = weight->logical_strength * weight->motivational_bias;
    weight->combined = clamp_float(weight->combined, 0.0f, 1.0f);
}

void edge_weight_update_from_valence(EdgeWeightDual* weight, float valence) {
    if (!weight) return;

    if (valence > 0.3f) {
        weight->logical_strength *= 1.0f + valence * 0.2f;
    } else if (valence < -0.3f) {
        weight->logical_strength *= 1.0f + valence * 0.2f;
    }

    weight->logical_strength = clamp_float(weight->logical_strength, 0.0f, 1.0f);
    edge_weight_compute(weight);
}

void edge_weight_destroy(EdgeWeightDual* weight) {
    if (weight) free(weight);
}

// ==================== 认知状态函数 ====================

CognitiveState* cognitive_state_create(void) {
    CognitiveState* state = (CognitiveState*)malloc(sizeof(CognitiveState));
    if (!state) return NULL;
    cognitive_state_init(state);
    return state;
}

void cognitive_state_init(CognitiveState* state) {
    if (!state) return;

    state->drive_curiosity = 0.5f;
    state->drive_hunger = 0.3f;
    state->drive_social = 0.4f;
    state->drive_comfort = 0.5f;

    state->emotion_pleasure = 0.0f;
    state->emotion_pain = 0.0f;
    state->emotion_security = 0.5f;

    state->valence = 0.0f;

    state->current_goal = NULL;
    state->goal_strength = 0.0f;
    state->plan_step = 0;

    state->explore_rate = 0.5f;
}

void cognitive_state_update(CognitiveState* state, Interaction* interaction, float outcome) {
    if (!state || !interaction) return;

    float new_valence = compute_valence_from_interaction(
        interaction, outcome, false);
    cognitive_state_update_valence(state, new_valence);

    state->explore_rate = compute_explore_exploit_balance(state->valence);

    if (interaction->feedback && strlen(interaction->feedback) > 0) {
        float feedback_val = get_feedback_valence(interaction->feedback);
        state->emotion_pleasure = clamp_float(
            state->emotion_pleasure + feedback_val * 0.3f,
            -1.0f, 1.0f);
    }
}

float compute_explore_exploit_balance(float recent_valence) {
    float rate = 0.5f;

    if (recent_valence < -0.5f) {
        rate = 0.8f;
    } else if (recent_valence < -0.2f) {
        rate = 0.6f;
    } else if (recent_valence > 0.5f) {
        rate = 0.3f;
    } else if (recent_valence > 0.2f) {
        rate = 0.4f;
    }

    return rate;
}

void cognitive_state_update_valence(CognitiveState* state, float new_valence) {
    if (!state) return;
    node_update_valence(&state->valence, new_valence, 0.3f);

    state->emotion_pleasure = state->valence;
    if (state->valence < 0) {
        state->emotion_pain = -state->valence;
    } else {
        state->emotion_pain = 0.0f;
    }
}

void cognitive_state_destroy(CognitiveState* state) {
    if (!state) return;
    if (state->current_goal) free(state->current_goal);
    free(state);
}

// ==================== 节点激活函数 ====================

void activate_node_with_valence(float* activation, float input_signal,
                                float logical_weight, float motivational_bias,
                                float node_valence) {
    if (!activation) return;

    float base_activation = input_signal * logical_weight * motivational_bias;
    float valence_factor = 1.0f + node_valence * 0.5f;

    *activation = base_activation * valence_factor;
    *activation = clamp_float(*activation, 0.0f, 1.0f);
}

float compute_edge_activation_with_valence(float logical_strength,
                                          float motivational_bias,
                                          float valence) {
    return logical_strength * motivational_bias * (1.0f + valence * 0.5f);
}
