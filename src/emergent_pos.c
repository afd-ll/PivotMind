/**
 * @file emergent_pos.c
 * @brief 涌现式词类系统 — 种子锚点 + 特征向量聚类的实现
 */

#include "emergent_pos.h"
#include "multi_topology.h"
#include "huarong_topology.h"
#include "common.h"
#include "error.h"          /* v0.5.19: LOG_INFO（DistSig诊断用） */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>

/* 前向声明 (classify 函数中调用, 定义在文件后面) */
int emergent_pos_try_emerge(EmergentPOS* ep);

/* ================================================================
 *  内部辅助: POS 标签名
 * ================================================================ */

static const char* pos_label_cn(POSTag tag) {
    static const char* names[] = {
        "未知", "名词", "动词", "形容词", "副词",
        "代词", "介词", "连词", "数词", "助词", "叹词"
    };
    if (tag >= 0 && tag < POS_COUNT) return names[tag];
    return "?";
}

static const char* pos_label_en(POSTag tag) {
    static const char* names[] = {
        "?", "NOUN", "VERB", "ADJ", "ADV",
        "PRON", "PREP", "CONJ", "NUM", "PART", "INTJ"
    };
    if (tag >= 0 && tag < POS_COUNT) return names[tag];
    return "?";
}

/* ================================================================
 *  种子词表 — 每个词类 5 个最典型词（仅本文件使用）
 * ================================================================ */

static const char* POS_CN_SEEDS[POS_COUNT][POS_ANCHOR_MAX_SEEDS] = {
    /* POS_UNKNOWN */  {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
    /* v2.1 阶段0-A：名词单字种子扩充（喂料路径按单字切分，多字种子永不命中；
     * 名词是配价宾语最关键类，原来只有"人"1 个单字种子，右邻名词信号近乎饿死）。
     * 前 20 个为单字名词（可信标签），后 4 个多字名词保留给词级 emergent_pos_tag。 */
    /* POS_NOUN    */  {"人",   "山",   "水",   "天",   "地",   "书",   "门",   "路",
                        "车",   "花",   "鱼",   "树",   "河",   "家",   "国",   "手",
                        "心",   "海",   "马",   "牛",   "苹果", "时间", "桌子", "思想",
                        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
    /* POS_VERB    */  {"吃",   "看",   "跑",   "想",   "说"  },
    /* POS_ADJ     */  {"大",   "好",   "美",   "快",   "新"  },
    /* POS_ADV     */  {"很",   "不",   "都",   "非常", "已经"},
    /* POS_PRON    */  {"我",   "你",   "他",   "这",   "什么"},
    /* POS_PREP    */  {"在",   "从",   "到",   "对",   "用"  },
    /* POS_CONJ    */  {"和",   "但",   "因为", "所以", "如果"},
    /* POS_NUM     */  {"一",   "十",   "百",   "个",   "次"  },
    /* POS_PARTICLE*/  {"的",   "了",   "着",   "吗",   "吧"  },
    /* POS_INTERJ  */  {"啊",   "哦",   "嗯",   "哈",   "哇"  },
};

static const char* POS_EN_SEEDS[POS_COUNT][POS_ANCHOR_MAX_SEEDS] = {
    /* POS_UNKNOWN */  {NULL, NULL, NULL, NULL, NULL},
    /* POS_NOUN    */  {"apple","time", "table","idea", "person"},
    /* POS_VERB    */  {"eat",  "run",  "think","say",  "see" },
    /* POS_ADJ     */  {"big",  "good", "fast", "new",  "beautiful"},
    /* POS_ADV     */  {"very", "not",  "all",  "often","already"},
    /* POS_PRON    */  {"I",    "you",  "he",   "this", "what"},
    /* POS_PREP    */  {"in",   "from", "to",   "for",  "with"},
    /* POS_CONJ    */  {"and",  "but",  "because","so", "if"  },
    /* POS_NUM     */  {"one",  "ten",  "hundred","first","time"},
    /* POS_PARTICLE*/  {"the",  "a",    "an",   "of",   "to"  },
    /* POS_INTERJ  */  {"oh",   "ah",   "wow",  "hey",  "oops"},
};

/* ================================================================
 *  创建/销毁
 * ================================================================ */

EmergentPOS* emergent_pos_create(const char* lang) {
    EmergentPOS* ep = (EmergentPOS*)calloc(1, sizeof(EmergentPOS));
    if (!ep) return NULL;

    ep->sim_threshold = POS_ANCHOR_DEFAULT_THRESHOLD;
    ep->learn_rate    = POS_ANCHOR_LEARN_RATE;
    ep->classify_count = 0;
    ep->anchor_count   = 0;

    /* 选择语言种子表 */
    const char* (*seed_table)[POS_ANCHOR_MAX_SEEDS];
    int is_en = (lang && lang[0] == 'e');
    seed_table = is_en ? POS_EN_SEEDS : POS_CN_SEEDS;

    pthread_mutex_init(&ep->lock, NULL);

    /* 遍历 POS 标签（跳过 POS_UNKNOWN=0） */
    for (int tag = POS_NOUN; tag < POS_COUNT; tag++) {
        POSAnchor* anchor = &ep->anchors[tag];
        anchor->tag      = (POSTag)tag;
        anchor->label_cn = pos_label_cn((POSTag)tag);
        anchor->label_en = pos_label_en((POSTag)tag);
        anchor->seed_count = 0;

        for (int s = 0; s < POS_ANCHOR_MAX_SEEDS; s++) {
            if (seed_table[tag][s] && seed_table[tag][s][0]) {
                anchor->seeds[anchor->seed_count++] = seed_table[tag][s];
            }
        }

        /* 初始化中心向量为 0 */
        memset(anchor->centroid, 0, sizeof(anchor->centroid));
        anchor->member_count = 0;
        anchor->centroid_stability = 0.0f;
        anchor->is_active = (anchor->seed_count > 0) ? 0 : 0; /* 0=等待中心初始化 */
    }

    ep->anchor_count = POS_COUNT - 1; /* 除去 POS_UNKNOWN */

    fprintf(stderr, "[EmergentPOS] 创建成功: %d 锚点, 语言=%s, 阈值=%.2f\n",
            ep->anchor_count, is_en ? "en" : "zh", ep->sim_threshold);
    return ep;
}

void emergent_pos_destroy(EmergentPOS* ep) {
    if (!ep) return;
    /* 退出前持久化 */
    if (ep->total_classifications > 0) {
        emergent_pos_save(ep, NULL);
    }
    pthread_mutex_destroy(&ep->lock);
    free(ep);
}

/* ================================================================
 *  锚点中心初始化
 * ================================================================ */

int emergent_pos_init_centroids(EmergentPOS* ep, MasterTopology* master) {
    if (!ep || !master) return 0;

    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    if (!vocab || !vocab->net) {
        fprintf(stderr, "[EmergentPOS] 词汇拓扑未就绪，推迟中心初始化\n");
        return 0;
    }

    HuarongTopologyNet* vnet = vocab->net;
    int nc = vnet->node_count;
    if (nc < 10) {
        fprintf(stderr, "[EmergentPOS] 词汇节点不足 (%d)，推迟中心初始化\n", nc);
        return 0;
    }

    int initialized = 0;

    for (int tag = POS_NOUN; tag < POS_COUNT; tag++) {
        POSAnchor* anchor = &ep->anchors[tag];
        if (anchor->is_active) continue; /* 已初始化 */
        if (anchor->seed_count == 0) continue;

        /* 扫描词汇拓扑，找种子词的节点 */
        int found = 0;
        float sum_feat[PM_NODE_FEATURE_DIM];
        memset(sum_feat, 0, sizeof(sum_feat));

        for (int i = 0; i < nc && found < anchor->seed_count; i++) {
            ReasoningNode* node = vnet->nodes[i];
            if (!node || !node->concept || !node->features) continue;

            /* 检查是否是种子词 */
            for (int s = 0; s < anchor->seed_count; s++) {
                if (node->concept && anchor->seeds[s] && strcmp_null(node->concept, anchor->seeds[s]) == 0) {
                    for (int d = 0; d < PM_NODE_FEATURE_DIM; d++) {
                        sum_feat[d] += node->features[d];
                    }
                    found++;
                    break;
                }
            }
        }

        if (found >= 2) {
            /* 取均值作为锚点中心 */
            float inv_n = 1.0f / (float)found;
            for (int d = 0; d < PM_NODE_FEATURE_DIM; d++) {
                anchor->centroid[d] = sum_feat[d] * inv_n;
            }
            anchor->member_count = found;
            anchor->is_active = 1;
            anchor->centroid_stability = 0.3f; /* 初始稳定性低 */
            initialized++;
            fprintf(stderr, "[EmergentPOS] 锚点[%s] 中心初始化: %d 个种子词\n",
                    anchor->label_cn, found);
        } else if (found == 1) {
            /* 只有一个种子词找到，直接用其特征向量 */
            for (int i = 0; i < nc; i++) {
                ReasoningNode* node = vnet->nodes[i];
                if (!node || !node->concept || !node->features) continue;
                for (int s = 0; s < anchor->seed_count; s++) {
                    if (node->concept && anchor->seeds[s] && strcmp_null(node->concept, anchor->seeds[s]) == 0) {
                        memcpy(anchor->centroid, node->features,
                               PM_NODE_FEATURE_DIM * sizeof(float));
                        anchor->member_count = 1;
                        anchor->is_active = 1;
                        anchor->centroid_stability = 0.15f;
                        initialized++;
                        break;
                    }
                }
                if (anchor->is_active) break;
            }
        } else {
            /* 种子词都不在词汇拓扑中，标记为待初始化 */
            fprintf(stderr, "[EmergentPOS] 锚点[%s] 无种子词节点，待后续初始化\n",
                    anchor->label_cn);
        }
    }

    if (initialized > 0) {
        fprintf(stderr, "[EmergentPOS] 锚点中心初始化完成: %d/%d 激活\n",
                initialized, ep->anchor_count);
    }
    return initialized;
}

/* ================================================================
 *  硬分类
 * ================================================================ */

POSTag emergent_pos_classify(EmergentPOS* ep, const float* features) {
    if (!ep || !features) return POS_UNKNOWN;

    /* v0.5.10: 锁保护——本函数写 unclassified 池/extra_classes/计数器，
     * tag_soft 锁外调用（article_flush 移出 ar->mutex），多 learn worker
     * 并发写会堆损坏（08-08 15:28 double free 实锤） */
    pthread_mutex_lock(&ep->lock);

    float best_sim = ep->sim_threshold;
    POSTag best_tag = POS_UNKNOWN;
    int best_is_extra = 0;

    /* 检查 10 个硬编码锚点 */
    for (int tag = POS_NOUN; tag < POS_COUNT; tag++) {
        POSAnchor* anchor = &ep->anchors[tag];
        if (!anchor->is_active) continue;

        float sim = cosine_similarity(features, anchor->centroid, PM_NODE_FEATURE_DIM);
        if (sim > best_sim) {
            best_sim = sim;
            best_tag = (POSTag)tag;
            best_is_extra = 0;
        }
    }

    /* 检查涌现出的额外词类 */
    for (int ei = 0; ei < ep->extra_class_count; ei++) {
        if (!ep->extra_classes[ei].is_active) continue;
        float sim = cosine_similarity(features, ep->extra_classes[ei].centroid, PM_NODE_FEATURE_DIM);
        if (sim > best_sim) {
            best_sim = sim;
            best_tag = (POSTag)(ep->extra_classes[ei].class_id);
            best_is_extra = 1;
        }
    }

    /* 分类成功 → 微调中心 */
    if (best_tag != POS_UNKNOWN) {
        if (best_is_extra && (int)best_tag >= POS_COUNT) {
            /* 微调额外词类中心 */
            int ei = (int)best_tag - POS_COUNT;
            if (ei < 16) {
                float lr = ep->learn_rate;
                for (int d = 0; d < PM_NODE_FEATURE_DIM; d++) {
                    ep->extra_classes[ei].centroid[d] += lr * (features[d] - ep->extra_classes[ei].centroid[d]);
                }
                ep->extra_classes[ei].member_count++;
            }
        } else {
            emergent_pos_adjust_centroid(ep, best_tag, features);
        }
    } else {
        /* 未分类 → 加入涌现池 */
        if (ep->unclassified_count < 64) {
            memcpy(ep->unclassified_feats[ep->unclassified_count], features,
                   PM_NODE_FEATURE_DIM * sizeof(float));
            ep->unclassified_pool_nodes[ep->unclassified_count] = -1;
            ep->unclassified_count++;
        }
    }

    ep->total_classifications++;
    ep->emerge_check_counter++;

    /* 定期触发涌现检查 */
    if (ep->emerge_check_counter >= EMERGE_CHECK_INTERVAL &&
        ep->unclassified_count >= EMERGE_POOL_TRIGGER) {
        ep->emerge_check_counter = 0;
        emergent_pos_try_emerge(ep);
    }

    /* 每 5000 次分类自动持久化 */
    if (ep->total_classifications % 5000 == 0) {
        emergent_pos_save(ep, NULL);
    }

    pthread_mutex_unlock(&ep->lock);
    return best_tag;
}

/* ================================================================
 *  软分类
 * ================================================================ */

void emergent_pos_classify_soft(EmergentPOS* ep, const float* features,
                                SoftClassResult* result) {
    if (!ep || !features || !result) {
        if (result) { result->count = 0; }
        return;
    }
    result->count = 0;

    /* v0.5.10: 锁保护——本函数写 extra_classes/涌现池/计数器，
     * tag_soft 锁外调用，多 learn worker 并发写会堆损坏
     * （08-08 15:28 double free 实锤）。注意：emergent_pos_tag_soft
     * 调本函数后不再回写节点（v0.5.10 已删），锁只护 ep 内部状态。 */
    pthread_mutex_lock(&ep->lock);

    /* 收集所有超过阈值的锚点 */
    typedef struct { POSTag tag; float sim; } Candidate;
    #define SOFT_CAND_MAX (POS_COUNT + 16)  /* 10 + 最多16个额外词类 */
    Candidate cands[SOFT_CAND_MAX];
    int nc = 0;

    for (int tag = POS_NOUN; tag < POS_COUNT; tag++) {
        POSAnchor* anchor = &ep->anchors[tag];
        if (!anchor->is_active) continue;

        float sim = cosine_similarity(features, anchor->centroid, PM_NODE_FEATURE_DIM);
        if (sim > ep->sim_threshold && nc < SOFT_CAND_MAX) {
            cands[nc].tag = (POSTag)tag;
            cands[nc].sim = sim;
            nc++;
        }
    }

    /* 检查涌现出的额外词类 */
    for (int ei = 0; ei < ep->extra_class_count && nc < SOFT_CAND_MAX; ei++) {
        if (!ep->extra_classes[ei].is_active) continue;
        float sim = cosine_similarity(features, ep->extra_classes[ei].centroid, PM_NODE_FEATURE_DIM);
        if (sim > ep->sim_threshold) {
            cands[nc].tag = (POSTag)(ep->extra_classes[ei].class_id);
            cands[nc].sim = sim;
            nc++;
        }
    }

    /* 按相似度降序排序 */
    for (int i = 1; i < nc; i++) {
        Candidate tmp = cands[i];
        int j = i - 1;
        while (j >= 0 && cands[j].sim < tmp.sim) {
            cands[j + 1] = cands[j];
            j--;
        }
        cands[j + 1] = tmp;
    }

    /* 输出前 SOFT_CLASS_MAX 个 */
    for (int i = 0; i < nc && i < SOFT_CLASS_MAX; i++) {
        result->tags[i] = cands[i].tag;
        result->confs[i] = cands[i].sim;
        result->count++;
    }

    /* 对最佳候选微调中心 */
    if (result->count > 0) {
        int best_tag = (int)result->tags[0];
        if (best_tag >= POS_COUNT) {
            int ei = best_tag - POS_COUNT;
            if (ei < 16) {
                float lr = ep->learn_rate;
                for (int d = 0; d < PM_NODE_FEATURE_DIM; d++)
                    ep->extra_classes[ei].centroid[d] += lr * (features[d] - ep->extra_classes[ei].centroid[d]);
                ep->extra_classes[ei].member_count++;
            }
        } else {
            emergent_pos_adjust_centroid(ep, result->tags[0], features);
        }
    } else {
        /* 未分类 → 加入涌现池 */
        if (ep->unclassified_count < 64) {
            memcpy(ep->unclassified_feats[ep->unclassified_count], features,
                   PM_NODE_FEATURE_DIM * sizeof(float));
            ep->unclassified_pool_nodes[ep->unclassified_count] = -1;
            ep->unclassified_count++;
        }
    }

    ep->total_classifications++;
    ep->emerge_check_counter++;
    if (result->count > 1) ep->soft_classifications++;

    /* 定期触发涌现检查 */
    if (ep->emerge_check_counter >= EMERGE_CHECK_INTERVAL &&
        ep->unclassified_count >= EMERGE_POOL_TRIGGER) {
        ep->emerge_check_counter = 0;
        emergent_pos_try_emerge(ep);
    }

    pthread_mutex_unlock(&ep->lock);
}

/* ================================================================
 *  按词名标注 — 双层路由
 * ================================================================ */

POSTag emergent_pos_seed_tag(EmergentPOS* ep, const char* word) {
    if (!ep || !word || !word[0]) return POS_UNKNOWN;
    /* 第一层: 种子词检查 — O(n) 线性扫描（人标先验，可信标签唯一来源） */
    for (int tag = POS_NOUN; tag < POS_COUNT; tag++) {
        POSAnchor* anchor = &ep->anchors[tag];
        for (int s = 0; s < anchor->seed_count; s++) {
            if (anchor->seeds[s] && strcmp(word, anchor->seeds[s]) == 0) {
                return (POSTag)tag;
            }
        }
    }
    return POS_UNKNOWN;
}

POSTag emergent_pos_tag(EmergentPOS* ep, MasterTopology* master,
                        const char* word) {
    if (!ep || !master || !word || !word[0]) return POS_UNKNOWN;

    /* 懒初始化: 若锚点中心未初始化且词汇拓扑就绪，自动初始化 */
    if (ep->anchor_count > 0 && !ep->anchors[POS_NOUN].is_active) {
        SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
        if (vocab && vocab->net && vocab->net->node_count >= 100) {
            emergent_pos_init_centroids(ep, master);
        }
    }

    /* 第一层: 种子词检查（v2.1 阶段0-A：抽出 emergent_pos_seed_tag，DRY） */
    POSTag seed_tag = emergent_pos_seed_tag(ep, word);
    if (seed_tag != POS_UNKNOWN) return seed_tag;

    /* 第二层: 特征向量相似度匹配 */
    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    if (!vocab || !vocab->net) return POS_UNKNOWN;

    int node_id = huarong_net_find_concept(vocab->net, word);
    if (node_id < 0) return POS_UNKNOWN;

    /* v0.5.10 fix: node_id 空洞防御——词巩固/压缩线程可能并发 shrink
     * nodes 数组（node_id 空洞→越界 SIGSEGV，08-08 同族崩溃）。
     * 锁外读的唯一安全边界就是 node_count 本身。 */
    if (node_id >= vocab->net->node_count) return POS_UNKNOWN;

    ReasoningNode* node = vocab->net->nodes[node_id];
    if (!node || !node->features) return POS_UNKNOWN;

    POSTag tag = emergent_pos_classify(ep, node->features);

    /* v0.5.10 fix: 移除回写词类到节点（emergent_class_ids/emergent_class_confs）。
     *
     * 原代码在锁外写共享节点字段 → 对话线程与词巩固线程并发时
     * 悬垂指针 SIGSEGV（08-08 三次 139 崩溃实锤，栈在 diffusion_generate
     * → emergent_pos_tag）。POS 标注是纯查询，回写是缓存优化——但:
     *   1) 本函数调用者（diffusion/multi_topology/cognitive_controller）
     *      全部只用返回值，无人依赖回写结果
     *   2) 回写由喂料路径 emergent_pos_tag_soft（article_flush 持锁调用）
     *      负责，数据持续供给模板生长与 diffusion 的 emergent_class_ids 读取
     *   3) 查询路径零写 = 零悬垂风险，锁外安全
     */
    return tag;
}

void emergent_pos_tag_soft(EmergentPOS* ep, MasterTopology* master,
                           const char* word, SoftClassResult* result) {
    if (result) result->count = 0;
    if (!ep || !master || !word || !word[0] || !result) return;

    /* 懒初始化 */
    if (ep->anchor_count > 0 && !ep->anchors[POS_NOUN].is_active) {
        SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
        if (vocab && vocab->net && vocab->net->node_count >= 100) {
            emergent_pos_init_centroids(ep, master);
        }
    }

    /* 第一层: 种子词检查 */
    for (int tag = POS_NOUN; tag < POS_COUNT; tag++) {
        POSAnchor* anchor = &ep->anchors[tag];
        for (int s = 0; s < anchor->seed_count; s++) {
            if (anchor->seeds[s] && strcmp(word, anchor->seeds[s]) == 0) {
                result->tags[0] = (POSTag)tag;
                result->confs[0] = 1.0f;
                result->count = 1;
                return;
            }
        }
    }

    /* 第二层: 特征向量软匹配 */
    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    if (!vocab || !vocab->net) return;

    int node_id = huarong_net_find_concept(vocab->net, word);
    if (node_id < 0) return;

    /* v0.5.10 fix: node_id 空洞防御——词巩固/扩容线程并发 shrink/realloc
     * nodes 数组时锁外读会悬垂（08-08 15:28 double free + SIGSEGV 实锤，
     * 栈: auto_learn_concepts → huarong_net_add_connection）。 */
    if (node_id >= vocab->net->node_count) return;

    ReasoningNode* node = vocab->net->nodes[node_id];
    if (!node || !node->features) return;

    emergent_pos_classify_soft(ep, node->features, result);

    /* v0.5.10 fix: 移除锁外回写词类到节点。
     * 原代码在 ar->mutex 锁外（article_flush 特意移出锁外避 O(n²) 聚类）
     * 写共享节点 emergent_class_ids/count——与扩容 realloc 并发时
     * 节点指针悬垂 → 堆损坏（double free）。POS 标注是查询，回写是
     * 缓存优化；模板生长/diffusion 读的数据由喂料主路径持续供给，
     * 此处零写 = 锁外安全。 */
}

/* ================================================================
 *  锚点中心微调
 * ================================================================ */

void emergent_pos_adjust_centroid(EmergentPOS* ep, POSTag tag,
                                  const float* features) {
    if (!ep || !features) return;
    if (tag <= POS_UNKNOWN || tag >= POS_COUNT) return;

    POSAnchor* anchor = &ep->anchors[tag];
    if (!anchor->is_active) return;

    /* 动态学习率: 中心越稳定，步长越小 */
    float lr = ep->learn_rate;
    if (anchor->centroid_stability > POS_ANCHOR_STABILITY_THRESHOLD) {
        lr *= 0.5f; /* 高稳定性时减半 */
    }
    /* 长期使用后进一步衰减 */
    if (ep->classify_count > 10000) lr *= 0.5f;

    /* EMA 更新: centroid += lr * (features - centroid) */
    for (int d = 0; d < PM_NODE_FEATURE_DIM; d++) {
        anchor->centroid[d] += lr * (features[d] - anchor->centroid[d]);
    }

    anchor->member_count++;

    /* 更新稳定性: 成员越多越稳定 */
    float new_stability = 1.0f - (1.0f / (1.0f + (float)anchor->member_count * 0.1f));
    anchor->centroid_stability = anchor->centroid_stability * 0.95f + new_stability * 0.05f;

    ep->classify_count++;
}

/* ================================================================
 *  新词类涌现 — 从未分类池中发现新的词类
 * ================================================================ */

int emergent_pos_try_emerge(EmergentPOS* ep) {
    if (!ep) return 0;
    if (ep->unclassified_count < EMERGE_POOL_TRIGGER) return 0;
    if (ep->extra_class_count >= 16) return 0; /* 达上限 */

    int n = ep->unclassified_count;
    fprintf(stderr, "[EmergentPOS] 涌现检查: 未分类池 %d 个词\n", n);

    /* 构建 pairwise 余弦相似度矩阵 (上三角) */
    /* 动态分配避免大栈数组 */
    float* sim_matrix = (float*)malloc((size_t)n * (size_t)n * sizeof(float));
    if (!sim_matrix) return 0;

    for (int i = 0; i < n; i++) {
        sim_matrix[i * n + i] = 1.0f;
        for (int j = i + 1; j < n; j++) {
            float s = cosine_similarity(ep->unclassified_feats[i],
                                    ep->unclassified_feats[j],
                                    PM_NODE_FEATURE_DIM);
            sim_matrix[i * n + j] = s;
            sim_matrix[j * n + i] = s;
        }
    }

    /* 简单贪婪聚类: 找第一个未访问的, 聚其所有相似度 > threshold 的邻居 */
    typedef struct { int* members; int count; int capacity; } Cluster;
    Cluster clusters[16];
    int nclusters = 0;
    int* visited = (int*)calloc((size_t)n, sizeof(int));
    if (!visited) { free(sim_matrix); return 0; }

    for (int i = 0; i < n && nclusters < 16; i++) {
        if (visited[i]) continue;
        visited[i] = 1;

        /* 创建新簇 */
        Cluster* cl = &clusters[nclusters];
        cl->capacity = n;
        cl->members = (int*)malloc((size_t)n * sizeof(int));
        if (!cl->members) break;
        cl->members[0] = i;
        cl->count = 1;

        /* 找所有相似度超阈值的邻居 */
        for (int j = i + 1; j < n; j++) {
            if (visited[j]) continue;
            /* 检查与簇内所有已有成员的相似度 (保守策略) */
            int all_connected = 1;
            for (int mi = 0; mi < cl->count; mi++) {
                if (sim_matrix[cl->members[mi] * n + j] < EMERGE_COHERENCE_THRESH) {
                    all_connected = 0;
                    break;
                }
            }
            if (all_connected) {
                visited[j] = 1;
                cl->members[cl->count++] = j;
            }
        }

        if (cl->count >= EMERGE_MIN_CLUSTER_SIZE) {
            nclusters++;
        } else {
            /* 簇太小，释放 */
            free(cl->members);
            cl->members = NULL;
            cl->count = 0;
        }
    }

    free(visited);

    /* 为每个有效簇创建新词类 */
    int created = 0;
    for (int ci = 0; ci < nclusters; ci++) {
        Cluster* cl = &clusters[ci];
        if (cl->count < EMERGE_MIN_CLUSTER_SIZE) {
            free(cl->members);
            continue;
        }

        /* 计算簇内平均 pairwise 相似度（coherence） */
        float total_sim = 0.0f;
        int npairs = 0;
        for (int a = 0; a < cl->count; a++) {
            for (int b = a + 1; b < cl->count; b++) {
                total_sim += sim_matrix[cl->members[a] * n + cl->members[b]];
                npairs++;
            }
        }
        float coherence = (npairs > 0) ? total_sim / (float)npairs : 0.0f;

        if (coherence < EMERGE_COHERENCE_THRESH) {
            fprintf(stderr, "[EmergentPOS] 簇 %d 紧密度 %.3f < %.2f, 放弃\n",
                    ci, coherence, EMERGE_COHERENCE_THRESH);
            free(cl->members);
            continue;
        }

        /* 创建新词类 */
        int ei = ep->extra_class_count;
        ep->extra_classes[ei].class_id = POS_COUNT + ei;
        ep->extra_classes[ei].member_count = cl->count;
        ep->extra_classes[ei].coherence = coherence;
        ep->extra_classes[ei].is_active = 1;

        /* 计算中心：簇内特征向量均值 */
        memset(ep->extra_classes[ei].centroid, 0,
               PM_NODE_FEATURE_DIM * sizeof(float));
        for (int mi = 0; mi < cl->count; mi++) {
            int pool_idx = cl->members[mi];
            for (int d = 0; d < PM_NODE_FEATURE_DIM; d++) {
                ep->extra_classes[ei].centroid[d] +=
                    ep->unclassified_feats[pool_idx][d];
            }
        }
        float inv_n = 1.0f / (float)cl->count;
        for (int d = 0; d < PM_NODE_FEATURE_DIM; d++) {
            ep->extra_classes[ei].centroid[d] *= inv_n;
        }

        /* 生成人类可读标签提示 */
        snprintf(ep->extra_classes[ei].label_hint,
                 sizeof(ep->extra_classes[ei].label_hint),
                 "Emerge%d", ep->extra_classes[ei].class_id);

        ep->extra_class_count++;
        created++;

        fprintf(stderr,
            "[EmergentPOS] 新词类涌现! class_id=%d size=%d coherence=%.3f label=%s\n",
            ep->extra_classes[ei].class_id, cl->count, coherence,
            ep->extra_classes[ei].label_hint);

        free(cl->members);
    }

    /* 从池中移除已聚类的词 */
    {
        int* keep = (int*)calloc((size_t)n, sizeof(int));
        if (keep) {
            int kept = 0;
            for (int pi = 0; pi < n; pi++) {
                int in_cluster = 0;
                for (int ci = 0; ci < nclusters; ci++) {
                    if (clusters[ci].count >= EMERGE_MIN_CLUSTER_SIZE) {
                        for (int mi = 0; mi < clusters[ci].count; mi++) {
                            if (clusters[ci].members[mi] == pi) {
                                in_cluster = 1; break;
                            }
                        }
                    }
                    if (in_cluster) break;
                }
                if (!in_cluster) keep[pi] = 1, kept++;
            }

            /* 压缩池: 保留未聚类的 */
            int dst = 0;
            for (int pi = 0; pi < n; pi++) {
                if (keep[pi]) {
                    if (dst != pi) {
                        memcpy(ep->unclassified_feats[dst],
                               ep->unclassified_feats[pi],
                               PM_NODE_FEATURE_DIM * sizeof(float));
                        ep->unclassified_pool_nodes[dst] =
                            ep->unclassified_pool_nodes[pi];
                    }
                    dst++;
                }
            }
            ep->unclassified_count = dst;
            free(keep);
        }
    }

    /* v0.5.8: 聚类检查后清池（无论成功失败）——
     * 散词彼此不相似，永远聚不成簇；保留会导致每 500 次分类调用
     * 反复触发 O(n²) 聚类，锁内长持 → 学习线程堆积
     * （08-07 慢喂料 36 线程排队实锤）。散词为低价值噪声，
     * 清空后重新积累成本低。 */
    ep->unclassified_count = 0;

    free(sim_matrix);
    return created;
}

/* ================================================================
 *  持久化 — 保存/加载锚点中心 + 额外词类
 * ================================================================ */

#define EMERGENT_POS_DEFAULT_FILE "emergent_pos.bin"

int emergent_pos_save(EmergentPOS* ep, const char* filepath) {
    if (!ep) return -1;
    const char* path = filepath ? filepath : EMERGENT_POS_DEFAULT_FILE;

    FILE* f = fopen(path, "wb");
    if (!f) { return -1; }

    /* 写 magic + version */
    uint32_t magic = 0x504D4550; /* "PMEP" */
    uint32_t version = 1;
    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&version, sizeof(version), 1, f);

    /* 统计: 激活的硬编码锚点数 */
    uint32_t active_count = 0;
    for (int tag = POS_NOUN; tag < POS_COUNT; tag++)
        if (ep->anchors[tag].is_active) active_count++;
    fwrite(&active_count, sizeof(active_count), 1, f);

    /* 逐个写入激活的硬编码锚点 */
    for (int tag = POS_NOUN; tag < POS_COUNT; tag++) {
        POSAnchor* anchor = &ep->anchors[tag];
        if (!anchor->is_active) continue;

        fwrite(&tag, sizeof(int), 1, f);
        fwrite(anchor->centroid, sizeof(float), PM_NODE_FEATURE_DIM, f);
        fwrite(&anchor->member_count, sizeof(int), 1, f);
        fwrite(&anchor->centroid_stability, sizeof(float), 1, f);
    }

    /* 额外词类 */
    fwrite(&ep->extra_class_count, sizeof(int), 1, f);
    for (int ei = 0; ei < ep->extra_class_count; ei++) {
        fwrite(&ep->extra_classes[ei].class_id, sizeof(int), 1, f);
        fwrite(ep->extra_classes[ei].centroid, sizeof(float), PM_NODE_FEATURE_DIM, f);
        fwrite(&ep->extra_classes[ei].member_count, sizeof(int), 1, f);
        fwrite(&ep->extra_classes[ei].coherence, sizeof(float), 1, f);
        fwrite(ep->extra_classes[ei].label_hint, sizeof(char), 32, f);
    }

    fclose(f);
    fprintf(stderr, "[EmergentPOS] 持久化完成: %d 硬编码锚点 + %d 额外词类 → %s\n",
            (int)active_count, ep->extra_class_count, path);
    return 0;
}

int emergent_pos_load(EmergentPOS* ep, const char* filepath) {
    if (!ep) return 0;
    const char* path = filepath ? filepath : EMERGENT_POS_DEFAULT_FILE;

    FILE* f = fopen(path, "rb");
    if (!f) return 0; /* 文件不存在是正常情况，返回 0 */

    /* 验证 magic + version */
    uint32_t magic = 0, version = 0;
    if (fread(&magic, sizeof(magic), 1, f) != 1) goto fail;
    if (fread(&version, sizeof(version), 1, f) != 1) goto fail;
    if (magic != 0x504D4550 || version != 1) goto fail;

    /* 读取硬编码锚点 */
    uint32_t active_count = 0;
    if (fread(&active_count, sizeof(active_count), 1, f) != 1) goto fail;

    int loaded = 0;
    for (uint32_t i = 0; i < active_count; i++) {
        int tag = 0;
        if (fread(&tag, sizeof(int), 1, f) != 1) goto fail;
        if (tag <= POS_UNKNOWN || tag >= POS_COUNT) goto fail;

        POSAnchor* anchor = &ep->anchors[tag];
        if (fread(anchor->centroid, sizeof(float), PM_NODE_FEATURE_DIM, f) != PM_NODE_FEATURE_DIM)
            goto fail;
        if (fread(&anchor->member_count, sizeof(int), 1, f) != 1) goto fail;
        if (fread(&anchor->centroid_stability, sizeof(float), 1, f) != 1) goto fail;
        anchor->is_active = 1;
        loaded++;
    }

    /* 读取额外词类 */
    int extra_count = 0;
    if (fread(&extra_count, sizeof(int), 1, f) != 1) goto fail;
    if (extra_count > 16) extra_count = 16;

    for (int ei = 0; ei < extra_count; ei++) {
        if (fread(&ep->extra_classes[ei].class_id, sizeof(int), 1, f) != 1) goto fail;
        if (fread(ep->extra_classes[ei].centroid, sizeof(float), PM_NODE_FEATURE_DIM, f) != PM_NODE_FEATURE_DIM)
            goto fail;
        if (fread(&ep->extra_classes[ei].member_count, sizeof(int), 1, f) != 1) goto fail;
        if (fread(&ep->extra_classes[ei].coherence, sizeof(float), 1, f) != 1) goto fail;
        if (fread(ep->extra_classes[ei].label_hint, sizeof(char), 32, f) != 32) goto fail;
        ep->extra_classes[ei].is_active = 1;
        ep->extra_class_count++;
    }

    fclose(f);
    fprintf(stderr, "[EmergentPOS] 加载完成: %d 硬编码锚点 + %d 额外词类 ← %s\n",
            loaded, ep->extra_class_count, path);
    return loaded + ep->extra_class_count;

fail:
    fclose(f);
    return 0;
}

/* ================================================================
 *  获取锚点信息
 * ================================================================ */

const POSAnchor* emergent_pos_get_anchor(EmergentPOS* ep, POSTag tag) {
    if (!ep || tag <= POS_UNKNOWN || tag >= POS_COUNT) return NULL;
    return &ep->anchors[tag];
}

int emergent_pos_anchor_count(EmergentPOS* ep) {
    if (!ep) return 0;
    int count = 0;
    for (int tag = POS_NOUN; tag < POS_COUNT; tag++) {
        if (ep->anchors[tag].is_active) count++;
    }
    return count;
}

const char* emergent_pos_class_name(EmergentPOS* ep, int class_id) {
    if (!ep || class_id <= 0) return "?";
    if (class_id < POS_COUNT) return ep->anchors[class_id].label_cn;
    /* 额外词类 */
    int ei = class_id - POS_COUNT;
    if (ei < ep->extra_class_count && ep->extra_classes[ei].is_active)
        return ep->extra_classes[ei].label_hint;
    return "?";
}

/* ================================================================
 * v0.5.19: 分布签名（语法词类的正确尺子——pro 评审 08-14）
 * 语义特征学不出分布类（可替换性）；语法类的本质是"上下文分布相同"。
 * dist_sig[26] = 左邻POS分布[0..10] + 右邻POS分布[11..21] + 位置先验[22..25]
 * ================================================================ */

/* v2.1 阶段0-A 回退开关：1=真分布(decay-all+bump,只写[0..21])，0=旧行为(只增不减+[22..25]+自计数) */
#ifndef DIST_SIG_TRUE_DISTRIBUTION
#define DIST_SIG_TRUE_DISTRIBUTION 1
#endif

void emergent_pos_update_dist_sig(ReasoningNode* node, int left_pos,
                                  int right_pos, int pos_flags) {
    if (!node) return;
    float lr = 0.05f;  /* EMA 学习率——在线累积（不事后批量） */
#if DIST_SIG_TRUE_DISTRIBUTION
    /* v2.1 阶段0-A：真分布——每个 token 出现先全槽衰减，再抬观察槽。
     * 只写 [0..21]（左邻 POS [0..10] + 右邻 POS [11..21]）：
     *   - 邻词是种子（left/right_pos 为 1..10 的 POS 标签）→ 抬对应槽；
     *   - 邻词非种子/无邻（POS_UNKNOWN 或 -1）→ 只全衰减、不抬，正确吃掉未知邻词质量。
     * [22][23] 交回 funcword_record_position 独占；[24][25]（动词后/前）废弃。
     * dist_sig_count 由 funcword_record_position 独占，本函数不再 ++（避免喂料双计数）。 */
    for (int d = 0; d < 22; d++)
        node->dist_sig[d] += lr * (0.0f - node->dist_sig[d]);
    if (left_pos >= POS_NOUN && left_pos < POS_COUNT)
        node->dist_sig[left_pos] += lr * (1.0f - node->dist_sig[left_pos]);
    if (right_pos >= POS_NOUN && right_pos < POS_COUNT)
        node->dist_sig[11 + right_pos] += lr * (1.0f - node->dist_sig[11 + right_pos]);
    (void)pos_flags;
#else
    /* 旧行为（回退保留）：只增不减（饱和 one-vector）+ [22..25] 位置位 + 自计数 */
    if (left_pos >= 0 && left_pos < POS_COUNT)
        node->dist_sig[left_pos] += lr * (1.0f - node->dist_sig[left_pos]);
    if (right_pos >= 0 && right_pos < POS_COUNT)
        node->dist_sig[11 + right_pos] += lr * (1.0f - node->dist_sig[11 + right_pos]);
    for (int i = 0; i < 4; i++)
        if (pos_flags & (1 << i))
            node->dist_sig[22 + i] += lr * (1.0f - node->dist_sig[22 + i]);
    node->dist_sig_count++;
#endif
}

void emergent_pos_diag_dist_clusters(EmergentPOS* ep, MasterTopology* master) {
    if (!ep || !master) return;
    static int diag_cnt = 0;
    if (++diag_cnt % 100 != 0) return;   /* 每100次调用打一次诊断 */
    SubTopology* vsub = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    if (!vsub || !vsub->net) return;
    HuarongTopologyNet* vnet = vsub->net;

    /* 对每个锚点类：种子词的 dist_sig 均值作分布中心 → 找最相似 top3（非种子） */
    for (int a = 0; a < ep->anchor_count; a++) {
        POSAnchor* anchor = &ep->anchors[a];
        if (!anchor || anchor->seed_count == 0) continue;
        float ctr[26] = {0};
        int found = 0;
        for (int i = 0; i < vnet->node_count; i++) {
            ReasoningNode* nd = vnet->nodes[i];
            if (!nd || !nd->concept || nd->dist_sig_count < 20) continue;
            for (int s = 0; s < anchor->seed_count; s++) {
                if (anchor->seeds[s] && strcmp_null(nd->concept, anchor->seeds[s]) == 0) {
                    for (int d = 0; d < 26; d++) ctr[d] += nd->dist_sig[d];
                    found++;
                    break;
                }
            }
        }
        if (found < 1) continue;
        for (int d = 0; d < 26; d++) ctr[d] /= (float)found;

        char top_names[3][64] = {{0},{0},{0}};
        float top_sim[3] = {0.0f, 0.0f, 0.0f};
        for (int i = 0; i < vnet->node_count; i++) {
            ReasoningNode* nd = vnet->nodes[i];
            if (!nd || !nd->concept || nd->dist_sig_count < 20) continue;
            int is_seed = 0;
            for (int s = 0; s < anchor->seed_count; s++)
                if (anchor->seeds[s] && strcmp_null(nd->concept, anchor->seeds[s]) == 0) {
                    is_seed = 1; break;
                }
            if (is_seed) continue;
            float dot = 0.0f, na = 0.0f, nb = 0.0f;
            for (int d = 0; d < 26; d++) {
                dot += ctr[d] * nd->dist_sig[d];
                na += ctr[d] * ctr[d];
                nb += nd->dist_sig[d] * nd->dist_sig[d];
            }
            float sim = (na > 0.0f && nb > 0.0f) ?
                dot / (sqrtf(na) * sqrtf(nb)) : 0.0f;
            for (int k = 0; k < 3; k++) {
                if (sim > top_sim[k]) {
                    for (int j = 2; j > k; j--) {
                        top_sim[j] = top_sim[j-1];
                        strncpy(top_names[j], top_names[j-1], 63);
                    }
                    top_sim[k] = sim;
                    strncpy(top_names[k], nd->concept, 63);
                    top_names[k][63] = '\0';
                    break;
                }
            }
        }
        LOG_INFO("[DistSig诊断] 类[%s]: 分布最近={%s(%.2f),%s(%.2f),%s(%.2f)}",
                 anchor->label_cn,
                 top_names[0], top_sim[0],
                 top_names[1], top_sim[1],
                 top_names[2], top_sim[2]);
    }
}
