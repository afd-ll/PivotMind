#include "common.h"
#include "broca.h"
#include "template_builder.h"
#include "emergent_pos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* v2.1 阶段0-A 回退开关：1=恢复输出路径 dist_sig 累积(旧行为)，0=删除(新行为，默认)。
 * 输出路径累积=把系统自己生成的 ≤4 词当语料喂 dist_sig（数据源错位 + 错尺子标签），
 * 已迁移到喂料路径（train_mode.c）。默认删，仅保留 emergent_pos_tag(模板) + 诊断(只读)。 */
#ifndef DIST_SIG_OUTPUT_ACCUM
#define DIST_SIG_OUTPUT_ACCUM 0
#endif

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

/* 动态缓冲扩容：确保 buf 至少还能容纳 need 字节 + 1 字节 NUL。
 * 不足时按 2 倍倍增 realloc。返回 0 成功，-1 分配失败。
 * （修 M2：空格插入/NUL 无边界检查导致的 1~2 字节堆溢出） */
static int bwr_reserve(char** buf, size_t* cap, int out_pos, size_t need) {
    if ((size_t)out_pos + need + 1 <= *cap) return 0;
    size_t ncap = *cap ? *cap : BWR_BUF_INIT;
    while ((size_t)out_pos + need + 1 > ncap) ncap *= 2;
    char* nb = (char*)realloc(*buf, ncap);
    if (!nb) return -1;
    *buf = nb;
    *cap = ncap;
    return 0;
}

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
    /* v0.5.20+ fix(M1): pos_tags 动态分配——原 BWR_MAX_POS(64) 栈数组在
     * word_count>64 时，下方模板匹配循环读 pos_tags[i+k] 会越界读。 */
    POSTag* pos_tags = NULL;
    int can_template = 0;
    if (ep) {
        pos_tags = (POSTag*)calloc((size_t)(word_count > 0 ? word_count : 1),
                                   sizeof(POSTag));
        if (!pos_tags) return NULL;
        int n = word_count;
        for (int i = 0; i < n; i++) {
            pos_tags[i] = emergent_pos_tag(ep, master, words[i]);
#if DIST_SIG_OUTPUT_ACCUM
            /* v0.5.19: 分布签名累积——左右邻POS + 位置标志（语法类涌现的数据源）
             * ⚠️ v2.1 阶段0-A 已废除此输出路径累积（数据源错位：把系统自己生成的
             * ≤4 词当语料 + emergent_pos_tag 错尺子标签），迁移到喂料路径。
             * 此块仅在 DIST_SIG_OUTPUT_ACCUM=1 时编译（回退用）。 */
            if (master && words[i]) {
                SubTopology* vsub = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
                if (vsub && vsub->net && words[i][0]) {
                    int vnid = huarong_net_find_concept(vsub->net, words[i]);
                    if (vnid >= 0 && vnid < vsub->net->node_count && vsub->net->nodes[vnid]) {
                        int lpos = (i > 0) ? (int)pos_tags[i-1] : -1;
                        int rpos = (i < n-1) ? (int)pos_tags[i+1] : -1;
                        int pflags = 0;
                        if (i == 0) pflags |= 1;                    /* 句首 */
                        if (i == n-1) pflags |= 2;                  /* 句尾 */
                        if (lpos == (int)POS_VERB) pflags |= 4;     /* 动词后 */
                        if (rpos == (int)POS_VERB) pflags |= 8;     /* 动词前 */
                        emergent_pos_update_dist_sig(
                            vsub->net->nodes[vnid], lpos, rpos, pflags);
                    }
                }
            }
#endif
        }
        can_template = 1;
        /* v0.5.19: 分布聚类诊断（内部每100次限流）——验证分布签名>语义聚类
         * v2.1 阶段0-A：诊断是只读（C 类），保留。 */
        emergent_pos_diag_dist_clusters(ep, master);
    }

    /* Step 2: 获取模板拓扑 */
    SubTopology* tpl_topo = NULL;
    if (can_template)
        tpl_topo = master_get_sub_topology_by_type(master, TOPO_TEMPLATE);

    /* Step 3: 滑动窗口匹配 + 拼接 */
    /* v0.5.20+ fix(M2): 动态缓冲——每次写入前 bwr_reserve 预留 need+1(NUL)，
     * 空格插入与 NUL 均有边界保证，消除原 1~2 字节堆溢出。 */
    size_t buf_cap = BWR_BUF_INIT;
    char* buf = (char*)malloc(buf_cap);
    if (!buf) { free(pos_tags); return NULL; }
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
                    if (last >= 0x20 && last < 0x80) {
                        if (bwr_reserve(&buf, &buf_cap, out_pos, 1)) goto oom;
                        buf[out_pos++] = ' ';
                    }
                }
                size_t slen = strlen(words[i + k]);
                if (bwr_reserve(&buf, &buf_cap, out_pos, slen)) goto oom;
                memcpy(buf + out_pos, words[i + k], slen);
                out_pos += (int)slen;

                if (k < best_len - 1 && tn->tpl_connectors[k][0] != '\0') {
                    slen = strlen(tn->tpl_connectors[k]);
                    if (bwr_reserve(&buf, &buf_cap, out_pos, slen)) goto oom;
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
                if (last >= 0x20 && last < 0x80) {
                    if (bwr_reserve(&buf, &buf_cap, out_pos, 1)) goto oom;
                    buf[out_pos++] = ' ';
                }
            }
            size_t slen = strlen(words[i]);
            if (bwr_reserve(&buf, &buf_cap, out_pos, slen)) goto oom;
            memcpy(buf + out_pos, words[i], slen);
            out_pos += (int)slen;

            /* 硬编码连接词 */
            if (i + 1 < word_count && can_template) {
                const char* conn = broca_hardcoded_connector(
                    pos_tags[i], pos_tags[i + 1]);
                if (conn && conn[0]) {
                    slen = strlen(conn);
                    if (bwr_reserve(&buf, &buf_cap, out_pos, slen)) goto oom;
                    memcpy(buf + out_pos, conn, slen);
                    out_pos += (int)slen;
                }
            }
            i++;
        }
    }

    if (out_pos >= (int)buf_cap) out_pos = (int)buf_cap - 1;  /* 防御：不应发生 */
    buf[out_pos] = '\0';
    free(pos_tags);
    return buf;

oom:
    free(buf);
    free(pos_tags);
    return NULL;
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
            if (tpl->net->nodes[j] && tpl->net->nodes[j]->concept && strcmp_null(tpl->net->nodes[j]->concept, seeds[i]) == 0)
                { exists = 1; break; }
        if (exists) continue;

        ReasoningNode* node = huarong_net_find_or_create_node(tpl->net, seeds[i], NULL, 0, NULL);
        if (node) created++;
    }

    if (created > 0)
        fprintf(stderr, "[布罗卡区] 注入语法种子: %d 条\n", created);
    return created;
}
