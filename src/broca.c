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
    b->build_interval_ticks = 300;  /* 默认每300 tick构建一次模板 */
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
                int need = snprintf(buf + out_pos, buf_cap - out_pos,
                                    "%s", words[i + k]);
                if (need < 0 || out_pos + need >= (int)buf_cap) goto done;
                out_pos += need;

                if (k < best_len - 1 && tn->tpl_connectors[k][0] != '\0') {
                    need = snprintf(buf + out_pos, buf_cap - out_pos,
                                    "%s", tn->tpl_connectors[k]);
                    if (need < 0 || out_pos + need >= (int)buf_cap) goto done;
                    out_pos += need;
                }
            }
            i += best_len;
        } else {
            /* 无模板：直接输出当前词 */
            int need = snprintf(buf + out_pos, buf_cap - out_pos,
                                "%s", words[i]);
            if (need < 0 || out_pos + need >= (int)buf_cap) goto done;
            out_pos += need;

            /* 硬编码连接词 */
            if (i + 1 < word_count && can_template) {
                const char* conn = broca_hardcoded_connector(
                    pos_tags[i], pos_tags[i + 1]);
                if (conn && conn[0]) {
                    need = snprintf(buf + out_pos, buf_cap - out_pos,
                                    "%s", conn);
                    if (need < 0 || out_pos + need >= (int)buf_cap) goto done;
                    out_pos += need;
                }
            }
            i++;
        }
    }

done:
    buf[out_pos] = '\0';
    return buf;
}
