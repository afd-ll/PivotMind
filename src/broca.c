#include "broca.h"
#include "template_builder.h"
#include "emergent_pos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Broca* broca_create(MasterTopology* master) {
    if (!master) return NULL;
    Broca* b = (Broca*)calloc(1, sizeof(Broca));
    if (!b) return NULL;
    b->master              = master;
    b->build_interval_ticks = 10;   /* 每10 tick构建一次语法 */
    b->max_build_depth     = 3;
    b->decay_threshold     = 5;     /* 活跃度低于此值衰减 */
    b->decay_rate          = 0.85f;
    printf("[布罗卡区] 就绪 (模板构建间隔=%d ticks, 深度=%d)\n",
           b->build_interval_ticks, b->max_build_depth);
    return b;
}

void broca_destroy(Broca* b) {
    free(b);
}

int broca_tick(Broca* b) {
    if (!b || !b->master) return 0;
    b->tick_count++;

    int new_templates = 0;

    /* 按间隔触发模板自动构建 */
    if (b->tick_count % b->build_interval_ticks == 0) {
        new_templates = template_auto_build(b->master, 50, b->max_build_depth);

        /* 自动构建无产出时的降级：注入基础中文语法种子 */
        if (new_templates == 0 && b->tick_count % 30 == 0) {
            SubTopology* tpl = master_get_sub_topology_by_type(b->master, TOPO_TEMPLATE);
            if (tpl && tpl->net && tpl->net->node_count < 20) {
                broca_seed_grammar(b->master);
            }
        }
        b->total_builds++;
        b->total_new_templates += (new_templates > 0 ? new_templates : 0);
    }

    /* 每5个间隔触发一次模板衰减 */
    if (b->tick_count % (b->build_interval_ticks * 5) == 0) {
        template_decay_inactive_links(b->master, b->decay_threshold, b->decay_rate);
    }

    return new_templates;
}

int broca_build_templates(MasterTopology* topology, int count, int max_depth) {
    return template_auto_build(topology, count, max_depth);
}

void broca_decay_templates(MasterTopology* topology, int threshold, float decay) {
    template_decay_inactive_links(topology, threshold, decay);
}

int broca_template_count(MasterTopology* topology) {
    SubTopology* tpl = master_get_sub_topology_by_type(topology, TOPO_TEMPLATE);
    return (tpl && tpl->net) ? tpl->net->node_count : 0;
}

/* ================================================================
 *  broca_wrap_response — 模板包裹词序列为自然语言句子
 *
 *  对扩散引擎产出的词级碎片序列，通过模板匹配插入连接词，
 *  把 "苹果红色的苹果红色的甜" 整理为 "红色的苹果，红色的苹果很甜"。
 *
 *  算法：
 *    1. 对每个词做 POS 标注（涌现词类系统）
 *    2. 滑动窗口（4→3→2）在 TOPO_TEMPLATE 中匹配 POS 序列
 *    3. 命中模板 → 插入 tpl_connectors[] 连接词
 *    4. 未命中 → 硬编码启发式规则兜底（ADJ+NOUN→"的" 等）
 * ================================================================ */

/* 硬编码连接词：基于 POS 对的启发式兜底规则 */
static const char* broca_hardcoded_connector(POSTag prev, POSTag next) {
    switch (prev) {
    case POS_ADJ:
        if (next == POS_NOUN) return "的";   /* 红色的苹果 */
        if (next == POS_ADJ)  return "";     /* 红红 */
        break;
    case POS_ADV:
        if (next == POS_VERB) return "地";   /* 慢慢地走 */
        break;
    case POS_VERB:
        if (next == POS_ADJ) return "得";    /* 跑得快 */
        break;
    case POS_NOUN:
        if (next == POS_VERB) return "";     /* 苹果好吃（主谓，无连接词） */
        if (next == POS_ADJ)  return "";     /* 苹果红（主谓） */
        break;
    default:
        break;
    }
    return "";
}

#define BWR_BUF_INIT  2048
#define BWR_MAX_POS   64

char* broca_wrap_response(MasterTopology* master, EmergentPOS* ep,
                          const char** words, int word_count) {
    if (!master || !words || word_count <= 0) return NULL;

    /* 单字直接返回 */
    if (word_count == 1) return strdup(words[0]);

    /* 模板拓扑为空时不做 POS 包裹，直接拼接原词返回 */
    int tpl_count = broca_template_count(master);
    if (tpl_count == 0) {
        size_t len = 0;
        for (int i = 0; i < word_count; i++) len += strlen(words[i]);
        char* raw = (char*)malloc(len + 1);
        if (!raw) return NULL;
        raw[0] = '\0';
        for (int i = 0; i < word_count; i++) strcat(raw, words[i]);
        return raw;
    }

    /* Step 1: POS 标注 */
    POSTag pos_tags[BWR_MAX_POS];
    int can_template = 0;
    if (ep) {
        int n = word_count < BWR_MAX_POS ? word_count : BWR_MAX_POS;
        for (int i = 0; i < n; i++) {
            pos_tags[i] = emergent_pos_tag(ep, master, words[i]);
        }
        can_template = 1;
    }

    /* Step 2: 获取模板拓扑 */
    SubTopology* tpl_topo = NULL;
    if (can_template)
        tpl_topo = master_get_sub_topology_by_type(master, TOPO_TEMPLATE);

    /* Step 3: 滑动窗口匹配 + 拼接 */
    size_t buf_cap = BWR_BUF_INIT;
    char* buf = (char*)malloc(buf_cap);
    if (!buf) return NULL;
    int out_pos = 0;
    buf[0] = '\0';

    int i = 0;
    while (i < word_count) {
        int best_len = 1;
        int best_tpl  = -1;

        /* 尝试最长模板匹配 (4 → 3 → 2) */
        if (tpl_topo && tpl_topo->net && can_template) {
            HuarongTopologyNet* net = tpl_topo->net;
            for (int wsize = 4; wsize >= 2 && i + wsize <= word_count; wsize--) {
                int found = 0;
                for (int n = 0; n < net->node_count && !found; n++) {
                    ReasoningNode* tn = net->nodes[n];
                    if (!tn || tn->tpl_pos_len != wsize) continue;

                    int match = 1;
                    for (int k = 0; k < wsize; k++) {
                        if ((int)pos_tags[i + k] != tn->tpl_pos_seq[k]) {
                            match = 0;
                            break;
                        }
                    }
                    if (match) {
                        best_len = wsize;
                        best_tpl = n;
                        found    = 1;
                    }
                }
                if (best_tpl >= 0) break;  /* 最长匹配优先 */
            }
        }

            if (best_tpl >= 0) {
            /* 模板命中：输出词 + 槽位间连接词 */
            ReasoningNode* tn = tpl_topo->net->nodes[best_tpl];
            for (int k = 0; k < best_len; k++) {
                /* 英文词间自动插空格 */
                if (out_pos > 0 && (unsigned char)words[i+k][0] < 0x80) {
                    unsigned char last = (unsigned char)buf[out_pos - 1];
                    if (last >= 0x20 && last < 0x80)
                        buf[out_pos++] = ' ';
                }
                size_t slen = strlen(words[i + k]);
                if (slen >= (size_t)(buf_cap - out_pos)) goto done;
                memcpy(buf + out_pos, words[i + k], slen);
                out_pos += (int)slen;

                if (k < best_len - 1 && tn->tpl_connectors[k][0] != '\0') {
                    slen = strlen(tn->tpl_connectors[k]);
                    if (slen >= (size_t)(buf_cap - out_pos)) goto done;
                    memcpy(buf + out_pos, tn->tpl_connectors[k], slen);
                    out_pos += (int)slen;
                }
            }
            i += best_len;
        } else {
            /* 无模板：直接输出当前词 */
            /* 英文词间自动插空格 */
            if (out_pos > 0 && (unsigned char)words[i][0] < 0x80) {
                unsigned char last = (unsigned char)buf[out_pos - 1];
                if (last >= 0x20 && last < 0x80)
                    buf[out_pos++] = ' ';
            }
            size_t slen = strlen(words[i]);
            if (slen >= (size_t)(buf_cap - out_pos)) goto done;
            memcpy(buf + out_pos, words[i], slen);
            out_pos += (int)slen;

            /* 硬编码连接词 */
            if (i + 1 < word_count && can_template) {
                const char* conn = broca_hardcoded_connector(
                    pos_tags[i], pos_tags[i + 1]);
                if (conn && conn[0]) {
                    slen = strlen(conn);
                    if (slen >= (size_t)(buf_cap - out_pos)) goto done;
                    memcpy(buf + out_pos, conn, slen);
                    out_pos += (int)slen;
                }
            }
            i++;
        }
    }

done:
    buf[out_pos] = '\0';
    return buf;
}

/* ================================================================
 *  broca_seed_grammar — 注入基础中文语法种子节点到模板拓扑
 *
 *  当自动语法发现（template_auto_build）无产出时，作为兜底方案
 *  直接创建 POS 序列语法节点，让 broca_wrap_response 能正常工作
 * ================================================================ */
#include "emergent_pos.h"

int broca_seed_grammar(MasterTopology* master) {
    if (!master) return 0;
    SubTopology* tpl = master_get_sub_topology_by_type(master, TOPO_TEMPLATE);
    if (!tpl || !tpl->net) return 0;
    if (tpl->net->node_count >= 20) return 0;

    /* 注入基础语法节点 — 只需存在即可让 broca_wrap_response 走 POS 匹配路径 */
    static const char* seeds[] = {
        "ADJ+NOUN:的", "NOUN+ADJ", "NOUN+VERB",
        "VERB+ADJ:得", "ADV+VERB:地", "NOUN+NOUN:和",
        "VERB+NOUN", "ADJ+ADJ"
    };
    int n = sizeof(seeds) / sizeof(seeds[0]);
    int created = 0;

    for (int i = 0; i < n; i++) {
        int exists = 0;
        for (int j = 0; j < tpl->net->node_count; j++)
            if (tpl->net->nodes[j] && tpl->net->nodes[j]->concept && strcmp(tpl->net->nodes[j]->concept, seeds[i]) == 0)
                { exists = 1; break; }
        if (exists) continue;

        ReasoningNode* node = huarong_net_find_or_create_node(tpl->net, seeds[i], NULL, 0, NULL);
        if (node) created++;
    }

    if (created > 0)
        fprintf(stderr, "[布罗卡区] 注入语法种子: %d 条\n", created);
    return created;
}
