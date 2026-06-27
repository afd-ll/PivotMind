/**
 * @file json_config.c
 * @brief 运行时配置加载器 — 最小化 JSON 解析，零外部依赖
 */

#include "json_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ── 默认值（与 constants.h / cognitive_params.h 一致） ── */

static void config_init_defaults(ConfigContext* ctx) {
    /* topology */
    ctx->topology.feature_dim         = 512;
    ctx->topology.max_nodes_per_topo  = 10000;
    ctx->topology.cross_hit_table_size = 2048;

    /* learning */
    ctx->learning.decay_rate           = 0.7f;
    ctx->learning.learn_rate           = 0.005f;
    ctx->learning.autonomic_shard_count = 16;
    ctx->learning.active_learner_interval = 300;
    ctx->learning.max_connections      = 8000;
    ctx->learning.flush_threshold      = 50;
    ctx->learning.idle_flush_seconds   = 30;

    /* inference */
    ctx->inference.max_response_len   = 2048;
    ctx->inference.default_hop_count  = 3;
    ctx->inference.max_associations   = 100;
    ctx->inference.max_hops_reasoning = 200;

    /* clock */
    ctx->clock.tick_interval_ms      = 1000;
    ctx->clock.decay_per_tick        = 0.97f;
    ctx->clock.spontaneous_prob      = 0.0001f;
    ctx->clock.spontaneous_strength  = 0.15f;
    ctx->clock.consolidate_interval  = 10;

    /* brain regions — all enabled by default */
    ctx->brain_regions.prefrontal   = true;
    ctx->brain_regions.hippocampus  = true;
    ctx->brain_regions.dmn          = true;
    ctx->brain_regions.perception   = true;
    ctx->brain_regions.broca        = true;
    ctx->brain_regions.cerebellum   = true;
    ctx->brain_regions.amygdala     = true;
    ctx->brain_regions.hypothalamus = true;
}

/* ── 最小化 JSON 解析器 ── */

static void skip_ws(const char** p) {
    while (**p && isspace((unsigned char)**p)) (*p)++;
}

/* 读取双引号字符串 */
static int read_string(const char** p, char* buf, int max_len) {
    if (**p != '"') return -1;
    (*p)++;
    int i = 0;
    while (**p && **p != '"' && i < max_len - 1) {
        if (**p == '\\') {
            (*p)++;
            if (**p == 'n') buf[i++] = '\n';
            else if (**p == 't') buf[i++] = '\t';
            else if (**p == '\\') buf[i++] = '\\';
            else if (**p == '"') buf[i++] = '"';
            else buf[i++] = **p;
        } else {
            buf[i++] = **p;
        }
        if (**p) (*p)++;
    }
    buf[i] = '\0';
    if (**p == '"') (*p)++;
    return i;
}

/* 读取数字 (int or float) */
static float read_number(const char** p) {
    char num_buf[64];
    int i = 0;
    while (**p && (isdigit((unsigned char)**p) || **p == '.' ||
           **p == '-' || **p == '+' || **p == 'e' || **p == 'E') && i < 62) {
        num_buf[i++] = **p;
        (*p)++;
    }
    num_buf[i] = '\0';
    return (float)atof(num_buf);
}

/* 读取 true/false */
static int read_bool(const char** p) {
    if (strncmp(*p, "true", 4) == 0) { *p += 4; return 1; }
    if (strncmp(*p, "false", 5) == 0) { *p += 5; return 0; }
    return -1;
}

/* 跳过值 */
static void skip_value(const char** p) {
    skip_ws(p);
    if (**p == '"') { char buf[256]; read_string(p, buf, sizeof(buf)); return; }
    if (**p == '{') {
        (*p)++;
        int depth = 1;
        while (**p && depth > 0) {
            if (**p == '{') depth++;
            if (**p == '}') depth--;
            (*p)++;
        }
        return;
    }
    if (**p == '[') {
        (*p)++;
        int depth = 1;
        while (**p && depth > 0) {
            if (**p == '[') depth++;
            if (**p == ']') depth--;
            (*p)++;
        }
        return;
    }
    if (**p == 't' || **p == 'f') { read_bool(p); return; }
    if (isdigit((unsigned char)**p) || **p == '-' || **p == '.') { read_number(p); return; }
    while (**p && **p != ',' && **p != '}' && **p != ']') (*p)++;
}

/* ── 字段设置 ── */

static void set_int_field(ConfigContext* ctx, const char* group, const char* key, float val) {
    int iv = (int)val;
    if (strcmp(group, "topology") == 0) {
        if      (strcmp(key, "feature_dim") == 0) ctx->topology.feature_dim = iv;
        else if (strcmp(key, "max_nodes_per_topo") == 0) ctx->topology.max_nodes_per_topo = iv;
        else if (strcmp(key, "cross_hit_table_size") == 0) ctx->topology.cross_hit_table_size = iv;
    } else if (strcmp(group, "learning") == 0) {
        if      (strcmp(key, "autonomic_shard_count") == 0) ctx->learning.autonomic_shard_count = iv;
        else if (strcmp(key, "active_learner_interval") == 0) ctx->learning.active_learner_interval = iv;
        else if (strcmp(key, "max_connections") == 0) ctx->learning.max_connections = iv;
        else if (strcmp(key, "flush_threshold") == 0) ctx->learning.flush_threshold = iv;
        else if (strcmp(key, "idle_flush_seconds") == 0) ctx->learning.idle_flush_seconds = iv;
    } else if (strcmp(group, "inference") == 0) {
        if      (strcmp(key, "max_response_len") == 0) ctx->inference.max_response_len = iv;
        else if (strcmp(key, "default_hop_count") == 0) ctx->inference.default_hop_count = iv;
        else if (strcmp(key, "max_associations") == 0) ctx->inference.max_associations = iv;
        else if (strcmp(key, "max_hops_reasoning") == 0) ctx->inference.max_hops_reasoning = iv;
    } else if (strcmp(group, "clock") == 0) {
        if      (strcmp(key, "tick_interval_ms") == 0) ctx->clock.tick_interval_ms = iv;
        else if (strcmp(key, "consolidate_interval") == 0) ctx->clock.consolidate_interval = iv;
    }
}

static void set_float_field(ConfigContext* ctx, const char* group, const char* key, float val) {
    if (strcmp(group, "learning") == 0) {
        if      (strcmp(key, "decay_rate") == 0) ctx->learning.decay_rate = val;
        else if (strcmp(key, "learn_rate") == 0) ctx->learning.learn_rate = val;
    } else if (strcmp(group, "clock") == 0) {
        if      (strcmp(key, "decay_per_tick") == 0) ctx->clock.decay_per_tick = val;
        else if (strcmp(key, "spontaneous_prob") == 0) ctx->clock.spontaneous_prob = val;
        else if (strcmp(key, "spontaneous_strength") == 0) ctx->clock.spontaneous_strength = val;
    }
}

static void set_bool_field(ConfigContext* ctx, const char* group, const char* key, int val) {
    int b = (val != 0);
    if (strcmp(group, "brain_regions") == 0) {
        if      (strcmp(key, "prefrontal") == 0)   ctx->brain_regions.prefrontal = b;
        else if (strcmp(key, "hippocampus") == 0)  ctx->brain_regions.hippocampus = b;
        else if (strcmp(key, "dmn") == 0)          ctx->brain_regions.dmn = b;
        else if (strcmp(key, "perception") == 0)   ctx->brain_regions.perception = b;
        else if (strcmp(key, "broca") == 0)        ctx->brain_regions.broca = b;
        else if (strcmp(key, "cerebellum") == 0)   ctx->brain_regions.cerebellum = b;
        else if (strcmp(key, "amygdala") == 0)     ctx->brain_regions.amygdala = b;
        else if (strcmp(key, "hypothalamus") == 0) ctx->brain_regions.hypothalamus = b;
    }
}

/* ── 主解析 ── */

static int parse_config(const char* json, ConfigContext* ctx) {
    const char* p = json;

    /* 跳过到第一个 { */
    while (*p && *p != '{') p++;
    if (*p != '{') return -1;
    p++;

    char group[64];
    char key[64];
    group[0] = '\0';

    while (*p) {
        skip_ws(&p);
        if (!*p || *p == '}') break;

        char closing = '\0';
        if (*p == ',' ) { p++; continue; }

        if (*p == '"') {
            char name[64];
            if (read_string(&p, name, sizeof(name)) < 0) { skip_value(&p); continue; }

            skip_ws(&p);
            if (*p != ':') { skip_value(&p); continue; }
            p++;

            /* Check if value is an object (sub-group) */
            skip_ws(&p);
            if (*p == '{') {
                /* group name -> sub-object */
                strncpy(group, name, sizeof(group) - 1);
                group[sizeof(group)-1] = '\0';
                p++; /* skip { */
                /* parse key:value pairs until } */
                while (*p) {
                    skip_ws(&p);
                    if (!*p || *p == '}') { p++; break; } /* close sub-object */
                    if (*p == ',') { p++; continue; }

                    if (*p == '"') {
                        if (read_string(&p, key, sizeof(key)) < 0) { skip_value(&p); continue; }
                        skip_ws(&p);
                        if (*p != ':') { skip_value(&p); continue; }
                        p++;
                        skip_ws(&p);

                        /* value */
                        if (*p == '"') {
                            /* string — not used for numeric config */
                            skip_value(&p);
                        } else if (*p == 't' || *p == 'f') {
                            int b = read_bool(&p);
                            set_bool_field(ctx, group, key, b);
                        } else if (isdigit((unsigned char)*p) || *p == '-' || *p == '.') {
                            float v = read_number(&p);
                            /* int or float? */
                            if (strchr(key, 'rate') || strchr(key, 'prob') || strchr(key, 'strength') ||
                                strchr(key, 'decay')) {
                                set_float_field(ctx, group, key, v);
                            } else {
                                set_int_field(ctx, group, key, v);
                            }
                        } else {
                            skip_value(&p);
                        }
                    }
                }
                continue;
            }

            /* value directly (top-level key) — skip */
            skip_value(&p);
        } else {
            skip_value(&p);
        }
    }

    return 0;
}

/* ── 公共 API ── */

ConfigContext* config_load(const char* path) {
    ConfigContext* ctx = calloc(1, sizeof(ConfigContext));
    if (!ctx) return NULL;

    config_init_defaults(ctx);

    if (!path) path = "pivotmind_config.json";

    FILE* fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "[config] 配置文件 '%s' 不存在，使用默认配置\n", path);
        ctx->loaded = 0;
        goto done;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0 || size > 65536) {
        fclose(fp);
        fprintf(stderr, "[config] 配置文件大小异常 (%ld bytes)，使用默认配置\n", size);
        ctx->loaded = 0;
        goto done;
    }

    char* json = malloc(size + 1);
    if (!json) {
        fclose(fp);
        ctx->loaded = 0;
        goto done;
    }

    size_t n = fread(json, 1, size, fp);
    fclose(fp);
    json[n] = '\0';

    if (parse_config(json, ctx) == 0) {
        fprintf(stderr, "[config] 已加载 '%s'\n", path);
        ctx->loaded = 1;
    } else {
        fprintf(stderr, "[config] 解析失败，使用默认配置\n");
    }

    free(json);
done:
    return ctx;
}

void config_destroy(ConfigContext* ctx) {
    free(ctx);
}

int config_write_default(const char* path) {
    if (!path) path = "pivotmind_config.json";
    FILE* fp = fopen(path, "w");
    if (!fp) return -1;

    fprintf(fp, "{\n");
    fprintf(fp, "    \"topology\": {\n");
    fprintf(fp, "        \"feature_dim\": 512,\n");
    fprintf(fp, "        \"max_nodes_per_topo\": 10000,\n");
    fprintf(fp, "        \"cross_hit_table_size\": 2048\n");
    fprintf(fp, "    },\n");
    fprintf(fp, "    \"learning\": {\n");
    fprintf(fp, "        \"decay_rate\": 0.7,\n");
    fprintf(fp, "        \"learn_rate\": 0.005,\n");
    fprintf(fp, "        \"autonomic_shard_count\": 16,\n");
    fprintf(fp, "        \"active_learner_interval\": 300,\n");
    fprintf(fp, "        \"max_connections\": 8000,\n");
    fprintf(fp, "        \"flush_threshold\": 50,\n");
    fprintf(fp, "        \"idle_flush_seconds\": 30\n");
    fprintf(fp, "    },\n");
    fprintf(fp, "    \"inference\": {\n");
    fprintf(fp, "        \"max_response_len\": 2048,\n");
    fprintf(fp, "        \"default_hop_count\": 3,\n");
    fprintf(fp, "        \"max_associations\": 100,\n");
    fprintf(fp, "        \"max_hops_reasoning\": 200\n");
    fprintf(fp, "    },\n");
    fprintf(fp, "    \"clock\": {\n");
    fprintf(fp, "        \"tick_interval_ms\": 1000,\n");
    fprintf(fp, "        \"decay_per_tick\": 0.97,\n");
    fprintf(fp, "        \"spontaneous_prob\": 0.0001,\n");
    fprintf(fp, "        \"spontaneous_strength\": 0.15,\n");
    fprintf(fp, "        \"consolidate_interval\": 10\n");
    fprintf(fp, "    },\n");
    fprintf(fp, "    \"brain_regions\": {\n");
    fprintf(fp, "        \"prefrontal\": true,\n");
    fprintf(fp, "        \"hippocampus\": true,\n");
    fprintf(fp, "        \"dmn\": true,\n");
    fprintf(fp, "        \"perception\": true,\n");
    fprintf(fp, "        \"broca\": true,\n");
    fprintf(fp, "        \"cerebellum\": true,\n");
    fprintf(fp, "        \"amygdala\": true,\n");
    fprintf(fp, "        \"hypothalamus\": true\n");
    fprintf(fp, "    }\n");
    fprintf(fp, "}\n");

    fclose(fp);
    return 0;
}
