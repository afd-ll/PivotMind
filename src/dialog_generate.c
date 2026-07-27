/**
 * @file dialog_generate.c
 * @brief 回复生成：走边路径 + 自动学习概念
 * @single_thread 访问 MasterTopology 和 DialogReasoning 共享状态，
 * 多线程调用需外部同步（如 master->mutex）
 */

#include "dialog_system.h"
#include "multi_topology.h"
#include "huarong_topology.h"
#include "cognitive_params.h"
#include "cognitive_controller.h"
#include "diffusion.h"
#include "utf8_tokenizer.h"
#include "string_pool.h"
#include "common.h"
#include "error.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static int assoc_cmp_desc(const void* a, const void* b) {
    float fa = ((const DialogAssociation*)a)->activation;
    float fb = ((const DialogAssociation*)b)->activation;
    return (fa < fb) ? 1 : (fa > fb) ? -1 : 0;
}

char* dialog_generate(DialogReasoning* reasoning, const char* input,
                     MemorySystem* memory, int max_len, void* sys) {
    if (!reasoning) return strdup("...");
    
    DialogSystem* dsys = (DialogSystem*)sys;
    
    // 优先检查记忆系统：精确匹配完整输入
    if (memory && input && input[0]) {
        char exact_key[PM_PATH_BUF] = {0};
        snprintf(exact_key, sizeof(exact_key), "response:%s", input);
        MemoryEntry* exact = memory_retrieve(memory, exact_key);
        if (exact && exact->data) {
            return strdup((char*)exact->data);
        }
    }
    
    // ===== 计算输入锚点特征向量 =====
    // 将输入 token 匹配到拓扑节点，对匹配节点的特征向量做平均，
    // 得到一个 NODE_FEATURE_DIM 维的"输入语义锚点"向量
    float query_anchor[NODE_FEATURE_DIM];
    int anchor_valid = 0;
    memset(query_anchor, 0, sizeof(query_anchor));
    if (input && input[0] && dsys && dsys->master) {
        // 用 UTF-8 tokenizer 分词
        char* anchor_tokens[64];
        int anc_tok_count = utf8_tokenize(input, anchor_tokens, 64);
        int matched = 0;
        SubTopology* vocab = master_get_sub_topology_by_type(dsys->master, TOPO_VOCABULARY);
        /* 拓扑驱动自举分词：扫描相邻 token 对，若存在组合节点则合并为整体，
         * 使走边时 \"苹果\" 作为整体匹配而非 \"苹\"+\"果\"。 */
        if (vocab && vocab->net && anc_tok_count > 1) {
            for (int t = 0; t < anc_tok_count - 1; t++) {
                if (!anchor_tokens[t] || !anchor_tokens[t+1]) continue;
                /* 只对两个连续 CJK 单字尝试组合合并 */
                if (strlen(anchor_tokens[t]) != 3 || strlen(anchor_tokens[t+1]) != 3) continue;
                char cname[13];
                snprintf(cname, sizeof(cname), "%s%s", anchor_tokens[t], anchor_tokens[t+1]);
                int cnid = huarong_net_find_concept(vocab->net, cname);
                if (cnid >= 0) {
                    /* 组合节点存在：替换 t 为组合名，标记 t+1 为已消费 */
                    free(anchor_tokens[t]);
                    anchor_tokens[t] = strdup(cname);
                    free(anchor_tokens[t+1]);
                    anchor_tokens[t+1] = NULL;
                    t++;  /* 跳过已合并的 token */
                }
            }
        }
        if (vocab && vocab->net && anc_tok_count > 0) {
            for (int t = 0; t < anc_tok_count; t++) {
                if (!anchor_tokens[t]) continue;
                /* O(1) 哈希查找替代 O(n) 遍历 */
                int nid = huarong_net_find_concept(vocab->net, anchor_tokens[t]);
                if (nid >= 0 && nid < vocab->net->node_count && matched < 32) {
                    ReasoningNode* node = vocab->net->nodes[nid];
                    if (node && node->features) {
                        for (int d = 0; d < NODE_FEATURE_DIM; d++)
                            query_anchor[d] += node->features[d];
                        matched++;
                    }
                }
            }
            if (matched > 0) {
                for (int d = 0; d < NODE_FEATURE_DIM; d++)
                    query_anchor[d] /= (float)matched;
                anchor_valid = 1;
            }
        }
        for (int t = 0; t < anc_tok_count; t++) free(anchor_tokens[t]);
    }
    float* anchor_ptr = anchor_valid ? query_anchor : NULL;
    
    // 拓扑驱动生成：优先 master_generate_response
    if (dsys && dsys->master && dsys->master->sub_topo_count > 0) {
        int total_nodes = 0;
        for (int t = 0; t < dsys->master->sub_topo_count; t++)
            if (dsys->master->sub_topologies[t] && dsys->master->sub_topologies[t]->net)
                total_nodes += dsys->master->sub_topologies[t]->net->node_count;
        if (total_nodes >= 10) {
            char* topo_response = master_generate_response(dsys->master, input, max_len);
            if (topo_response && strlen(topo_response) > 0) {
                // 启用模板投票（为后续路径做铺垫）
                dsys->master->use_template_voting = 1;
                LOG_INFO("[gen] master_generate_response: '%s'", topo_response);
                return topo_response;
            }
            free(topo_response);
        }
    }
    
    // 走边/竞争队列生成：从推理结果中提取最高激活度的概念出发
    if (reasoning->assoc_count > 0) {
        char* response = (char*)calloc(max_len, 1);
        if (!response) return strdup("...");
        int pos = 0;

        // 按激活值降序排列（qsort O(n log n)）
        qsort(reasoning->associations, reasoning->assoc_count,
              sizeof(DialogAssociation), assoc_cmp_desc);

        // 获取意图权重（用于走边和竞争队列的拓扑级调制）
        // 增强版：取前3个关联的拓扑权重加权平均，放大分辨力
        float intent_weight = 0.7f;  // 基准：确保有足够调制空间
        if (dsys && dsys->controller) {
            float weighted_sum = 0.0f, sum_w = 0.0f;
            for (int ai = 0; ai < reasoning->assoc_count && ai < 3; ai++) {
                int topo = reasoning->associations[ai].topo_type;
                float act = reasoning->associations[ai].activation;
                if (topo >= 0 && topo < MAX_SUBTOPOS && act > 0.1f) {
                    weighted_sum += dsys->controller->intent_weights[topo] * act;
                    sum_w += act;
                }
            }
            if (sum_w > 0.0f) intent_weight = 0.4f + 0.6f * (weighted_sum / sum_w);
        }

        // ===== 句式 scaffold 选择：从输入提取 POS 序列匹配最佳句式 =====
        void* cc_ptr = dsys ? dsys->controller : NULL;
        if (cc_ptr && input && input[0]) {
            cc_select_sentence_pattern((CognitiveController*)cc_ptr, input);
        }

        unsigned char* global_visited = NULL;
        int global_bm_size = 0;
        int max_path = PM_WALK_MAX_OUTPUT;

        for (int start_i = 0; start_i < reasoning->assoc_count && start_i < 5 && pos < max_len - 10; start_i++) {
            DialogAssociation* assoc = &reasoning->associations[start_i];
            if (assoc->activation < 0.1f) continue;

            SubTopology* sub = master_get_sub_topology_by_type(
                dsys->master, assoc->topo_type);
            if (!sub || !sub->net || sub->net->node_count <= 0) continue;

            int start_id = assoc->node_id;
            if (start_id < 0 || start_id >= sub->net->node_count ||
                !sub->net->nodes[start_id]) continue;

            // 分配全局 visited（跨起点共享）
            if (!global_visited && sub->net->node_count > 0) {
                global_bm_size = (sub->net->node_count + 7) / 8;
                global_visited = (unsigned char*)calloc(global_bm_size, 1);
            }

            // 如果已被之前的起点访问过，跳过
            if (global_visited) {
                if (start_id < global_bm_size * 8 &&
                    (global_visited[start_id / 8] & (unsigned char)(1 << (start_id % 8))))
                    continue;
            }

            int path_nodes[32];
            float path_scores[32];
            int path_len = 0;

            // ===== 策略1：竞争队列（全局视野，首选） =====
            // 将起点设为高激活种子，让竞争队列从全图竞争胜出
            sub->net->nodes[start_id]->activation = fmaxf(
                sub->net->nodes[start_id]->activation, assoc->activation);
            path_len = competitive_queue_generate(
                sub, dsys ? dsys->master : NULL,
                intent_weight, anchor_ptr,
                max_path < max_len ? max_path : max_len,
                path_nodes, path_scores, cc_ptr);

            // ===== 策略2：Beam search（全局+局部混合，K=5） =====
            if (path_len <= 1) {
                path_len = topology_walk_beam(
                    sub, start_id, path_nodes, path_scores,
                    max_path < max_len ? max_path : max_len,
                    global_visited, intent_weight,
                    dsys ? dsys->master : NULL,
                    anchor_ptr, cc_ptr);
            }
            
            // ===== 策略3：贪心走边（最终回退） =====
            if (path_len <= 1) {
                path_len = topology_walk_greedy(
                    sub, start_id, path_nodes, path_scores,
                    max_path < max_len ? max_path : max_len,
                    global_visited, intent_weight,
                    dsys ? dsys->master : NULL,
                    anchor_ptr, cc_ptr);
            }

            if (path_len <= 1) continue;

            // 标记路径节点为已访问
            if (global_visited) {
                for (int p = 0; p < path_len && p < 32; p++) {
                    int nid = path_nodes[p];
                    if (nid >= 0 && nid < global_bm_size * 8)
                        global_visited[nid / 8] |= (unsigned char)(1 << (nid % 8));
                }
            }

            // 因果筛选：路径因果一致性太低 → 跳过此起点
            if (dsys && dsys->controller) {
                float causal = causal_path_score(
                    dsys->controller, sub, path_nodes, path_len);
                if (causal < 0.25f) continue;
            }

            // 内感受评估：路径质量不达标 → 试下一个起点
            if (dsys && dsys->controller) {
                PathResult draft;
                draft.topo_id = assoc->topo_type;
                draft.length = path_len < MAX_PATH_LENGTH ? path_len : MAX_PATH_LENGTH;
                draft.act_sum = 0.0f;
                draft.conf_sum = 0.0f;
                for (int pi = 0; pi < draft.length; pi++) {
                    draft.node_ids[pi] = path_nodes[pi];
                    if (path_nodes[pi] >= 0 && path_nodes[pi] < sub->net->node_count) {
                        ReasoningNode* n = sub->net->nodes[path_nodes[pi]];
                        if (n) {
                            draft.act_sum += n->activation;
                            draft.conf_sum += n->confidence;
                        }
                    }
                }
                float satisfaction = evaluate_draft(dsys->controller, &draft, draft.length);
                if (satisfaction < dsys->controller->satisfaction_threshold) continue;
            }

            // ===== 路径转输出：POS语法关系驱动的连接词映射 =====
            // 三级回退策略：
            //   1. 模板匹配 → 使用学习到的模板连接词
            //   2. POS对映射 → pos_connector_map 按语法角色决定连接词
            //      （定中→"的", 状中→"地", 主谓→"", 动宾→"", 等）
            //   3. 无连接词 → 直接拼接（语法上天然不需连接词的组合）
            const char* last_concept = NULL;
            CognitiveController* cc_dg = dsys ? dsys->controller : NULL;

            // 先输出路径起点
            {
                ReasoningNode* start_node = (path_nodes[0] >= 0 && path_nodes[0] < sub->net->node_count)
                    ? sub->net->nodes[path_nodes[0]] : NULL;
                if (start_node && start_node->concept && concept_is_printable(start_node->concept)) {
                    pos += snprintf(response + pos, max_len - pos, "%s", start_node->concept);
                    last_concept = start_node->concept;
                }
            }
            
            for (int p = 1; p < path_len && pos < max_len - 10; p++) {
                int nid = path_nodes[p];
                if (nid < 0 || nid >= sub->net->node_count) continue;
                ReasoningNode* node = sub->net->nodes[nid];
                if (!node || !node->concept || !concept_is_printable(node->concept)) continue;
                
                // 去重
                if (last_concept && node->concept && strcmp_null(last_concept, node->concept) == 0) continue;
                
                int prev_nid = path_nodes[p-1];
                const char* connector = NULL;
                
                // --- Level 1: 模板匹配 ---
                if (dsys && dsys->master && dsys->master->use_template_voting
                    && prev_nid >= 0 && prev_nid < sub->net->node_count) {
                    int tpl_id = master_find_template_for_pair(
                        dsys->master, sub->topo_id, prev_nid, nid);
                    if (tpl_id >= 0)
                        connector = template_get_connector(dsys->master, tpl_id, 0);
                }
                
                // --- Level 2: POS语法关系映射（替代硬编码连接词轮换）---
                if (!connector || !connector[0]) {
                    ReasoningNode* prev_node = (prev_nid >= 0 && prev_nid < sub->net->node_count)
                        ? sub->net->nodes[prev_nid] : NULL;
                    POSTag prev_pos = POS_UNKNOWN;
                    POSTag curr_pos = POS_UNKNOWN;
                    if (cc_dg) {
                        prev_pos = pos_tag_emergent(cc_dg,
                            prev_node ? prev_node->concept : NULL);
                        curr_pos = pos_tag_emergent(cc_dg, node->concept);
                    }
                    connector = pos_connector_map((int)prev_pos, (int)curr_pos);
                }
                
                // --- Level 3: 输出 ---
                if (connector && connector[0]) {
                    pos += snprintf(response + pos, max_len - pos,
                        "%s%s", connector, node->concept);
                } else {
                    pos += snprintf(response + pos, max_len - pos,
                        "%s", node->concept);
                }
                
                last_concept = node->concept;
            }

            // 喂走边路径到认知调度中心
            if (dsys->controller) {
                cognitive_controller_observe_path(
                    dsys->controller,
                    assoc->topo_type, path_nodes, path_len);
            }

            if (pos >= 5) break;
        }

        if (global_visited) free(global_visited);

        // 兜底1：模板生成无输出，回退到 master_generate_response
        if (pos == 0 && dsys && dsys->master) {
            char* topo_response = master_generate_response(
                dsys->master, input, max_len);
            if (topo_response && strlen(topo_response) > 0) {
                snprintf(response, max_len, "%s", topo_response);
                pos = (int)strlen(response);
            }
            if (topo_response) free(topo_response);
        }
        // 兜底2：走边没走出结果，输出最高激活概念
        if (pos == 0 && reasoning->assoc_count > 0) {
            if (concept_is_printable(reasoning->associations[0].concept))
                snprintf(response, max_len, "%s", reasoning->associations[0].concept);
        }

        // ===== 记录路径到预测误差反馈环 =====
        if (dsys) {
            dsys->has_last_turn = 1;
            dsys->last_path_count = 0;
            strncpy(dsys->last_input, input ? input : "", sizeof(dsys->last_input) - 1);
            dsys->last_input[sizeof(dsys->last_input) - 1] = '\0';
            strncpy(dsys->last_response, response, sizeof(dsys->last_response) - 1);
            dsys->last_response[sizeof(dsys->last_response) - 1] = '\0';
            // 记录路径节点
            for (int si = 0; si < reasoning->assoc_count && si < 5 && dsys->last_path_count < 128; si++) {
                DialogAssociation* a = &reasoning->associations[si];
                if (a->activation < 0.1f) continue;
                SubTopology* sub = master_get_sub_topology_by_type(dsys->master, a->topo_type);
                if (!sub || !sub->net || a->node_id < 0 || a->node_id >= sub->net->node_count)
                    continue;
                ReasoningNode* node = sub->net->nodes[a->node_id];
                if (!node) continue;
                int idx = dsys->last_path_count;
                dsys->last_path_topo_types[idx] = a->topo_type;
                dsys->last_path_node_ids[idx] = a->node_id;
                // 找 from_node_id → a->node_id 的边索引
                int edge_idx = -1;
                if (a->from_node_id >= 0 && a->from_node_id < sub->net->node_count) {
                    ReasoningNode* from = sub->net->nodes[a->from_node_id];
                    if (from) {
                        for (int e = 0; e < from->edge_count; e++) {
                            if (from->edges[e].target && from->edges[e].target->node_id == a->node_id) {
                                edge_idx = e;
                                break;
                            }
                        }
                    }
                }
                dsys->last_path_edge_ids[idx] = edge_idx;
                dsys->last_path_count++;
            }
            // 同步路径信息到主动学习器（供 feedback_correct 边压制使用）
            if (dsys->learner && dsys->last_path_count > 0) {
                dsys->learner->last_path_count = dsys->last_path_count;
                for (int pi = 0; pi < dsys->last_path_count && pi < PM_PATH_TRACK; pi++) {
                    dsys->learner->last_path_node_ids[pi]  = dsys->last_path_node_ids[pi];
                    dsys->learner->last_path_topo_types[pi] = dsys->last_path_topo_types[pi];
                    dsys->learner->last_path_edge_ids[pi]  = dsys->last_path_edge_ids[pi];
                }
            }
        }

        // 走边没走出内容时，尝试记忆回退
        if (pos == 0 && memory) {
            char key[PM_KEY_BUF] = {0};
            snprintf(key, sizeof(key) - 1, "input:%s", input);
            MemoryEntry* mem = memory_retrieve(memory, key);
            if (mem && mem->data && mem->importance > 0.6f) {
                pos += snprintf(response + pos, max_len - pos,
                    "我记得你教过我这个：%s", (char*)mem->data);
            } else {
                pos += snprintf(response + pos, max_len - pos,
                    "我还在学习中，关于这个你能教我吗？");
            }
        }
        // 最后的兜底
        if (pos == 0) {
            snprintf(response, max_len, "我正在思考这个问题...");
        }

        LOG_INFO("[dialog_generate] returning: '%s' (len=%d)", response, (int)strlen(response));
        return response;
    }
    
    return strdup("...");
}

// ==================== 自动学习概念到拓扑网络 ====================
// 真正的学习应该像人脑一样，通过对话自然发生，而不是人为预先连接神经回路
// 实现：从对话文本中提取有意义的概念，加入拓扑网络并建立共现连接

/* 复用 diffusion 统一虚词表，消除重复维护 */
#define is_stop_word(w) diffusion_is_stop_word(w)

// 检查字符串是否包含中文或英文标点
static int contains_punctuation(const char* s) {
    if (!s) return 0;
    /* 仅检查单字节ASCII标点，避免将CJK字符的UTF-8字节误判为标点字节 */
    const char* punct = ",.;:!?\"'()[]{}<>/\\@#$%^&*_+-=~`";
    for (const char* p = punct; *p; p++) {
        if (strchr(s, *p)) return 1;
    }
    /* 检查多字节中文标点（逐字符匹配） */
    static const char* cjk_punct[] = {
        "\xe3\x80\x82",  /* 。 */
        "\xe3\x80\x81",  /* 、 */
        "\xef\xbc\x8c",  /* ， */
        "\xef\xbc\x9b",  /* ； */
        "\xef\xbc\x9a",  /* ： */
        "\xef\xbc\x81",  /* ！ */
        "\xef\xbc\x9f",  /* ？ */
        "\xe2\x80\xa6",  /* … */
        "\xe2\x80\x94",  /* — */
        "\xef\xbd\x9e",  /* ～ */
        "\xc2\xb7",      /* · */
        "\xef\xbc\x8e",  /* ． */
        "\xef\xbc\x88",  /* （ */
        "\xef\xbc\x89",  /* ） */
        "\xe3\x80\x90",  /* 【 */
        "\xe3\x80\x91",  /* 】 */
        "\xe3\x80\x8a",  /* 《 */
        "\xe3\x80\x8b",  /* 》 */
        NULL
    };
    for (const char** pp = cjk_punct; *pp; pp++) {
        if (strstr(s, *pp)) return 1;
    }
    return 0;
}

/**
 * 概念文本是否适合输出到回复中
 * 允许：中文、字母数字、合法标点（中英文标点均放行）
 * 拒绝：@ # $ % ^ & * + = \ | ~ ` 等纯噪音符号
 */
int concept_is_printable(const char* concept) {
    if (!concept || !concept[0]) return 0;
    for (const char* p = concept; *p; p++) {
        unsigned char c = (unsigned char)*p;
        /* 多字节 UTF-8 continuation byte (10xxxxxx) — 安全跳过 */
        if ((c & 0xC0) == 0x80) continue;
        if (c >= 0x80) continue;                      /* CJK/全角字符 */
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) continue;
        if (c >= '0' && c <= '9') continue;
        if (c == ',' || c == '.' || c == '!' || c == '?' || c == ';' || c == ':') continue;
        if (c == '\'' || c == '"') continue;
        if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}') continue;
        if (c == '-' || c == '_' || c == '/') continue;
        return 0;  /* 空格 @#$%^&*+=|~` 等纯噪音 */
    }
    return 1;
}

/* ==================== 组合节点条件概率追踪器 ====================
 * 统计单字出现次数和相邻字符对共现次数，
 * 当条件概率 P(B|A) ≥ THETA 或 P(A|B) ≥ THETA 且共现数 ≥ N_MIN 时，
 * 自动创建组合节点（如 "苹"+"果" → "苹果"）。
 * 哈希表为静态全局变量，进程重启后从零重新累积。
 * 已创建的组合节点作为普通 ReasoningNode 持久化到状态文件。
 */
#define CP_HASH_SIZE   65536
#define CP_HASH_MASK   (CP_HASH_SIZE - 1)
#define COMPOUND_N_MIN 10       /* 最少共现次数，防小样本噪声 */
#define COMPOUND_THETA 0.5f     /* 条件概率阈值 */

typedef struct {
    int node_a;   /* smaller node_id */
    int node_b;   /* larger node_id */
    int cooc;     /* co-occurrence count */
} CoocEntry;

typedef struct { CoocEntry table[CP_HASH_SIZE]; } PairCoocTable;
static PairCoocTable* g_pair_cooc = NULL;
static int* g_single_count = NULL;
static int  g_single_capacity = 0;

static inline int cp_pair_hash(int a, int b) {
    if (a > b) { int t = a; a = b; b = t; }
    uint64_t key = ((uint64_t)a << 32) | (uint64_t)b;
    return (int)((key * 0x9E3779B97F4A7C15ULL) & CP_HASH_MASK);
}

static void cp_tracker_ensure(int max_nodes) {
    if (!g_pair_cooc) g_pair_cooc = (PairCoocTable*)calloc(1, sizeof(PairCoocTable));
    if (g_single_capacity < max_nodes) {
        int new_cap = max_nodes + 2048;
        int* p = (int*)realloc(g_single_count, (size_t)new_cap * sizeof(int));
        if (p) {
            memset(p + g_single_capacity, 0,
                   (size_t)(new_cap - g_single_capacity) * sizeof(int));
            g_single_count = p;
            g_single_capacity = new_cap;
        }
    }
}

static void cp_tracker_record(int id_a, int id_b) {
    if (!g_pair_cooc || id_a < 0 || id_b < 0 || id_a == id_b) return;
    int max_id = id_a > id_b ? id_a : id_b;
    if (max_id >= g_single_capacity) cp_tracker_ensure(max_id + 2048);
    if (g_single_count) {
        if (id_a < g_single_capacity) g_single_count[id_a]++;
        if (id_b < g_single_capacity) g_single_count[id_b]++;
    }
    int h = cp_pair_hash(id_a, id_b);
    int min_id = id_a < id_b ? id_a : id_b;
    int max_id_ = id_a > id_b ? id_a : id_b;
    for (int p = 0; p < CP_HASH_SIZE; p++) {
        int idx = (h + p) & CP_HASH_MASK;
        CoocEntry* e = &g_pair_cooc->table[idx];
        if (e->cooc == 0) {
            e->node_a = min_id; e->node_b = max_id_; e->cooc = 1; return;
        }
        if (e->node_a == min_id && e->node_b == max_id_) { e->cooc++; return; }
    }
}

static int cp_tracker_get_cooc(int id_a, int id_b) {
    if (!g_pair_cooc) return 0;
    int h = cp_pair_hash(id_a, id_b);
    int min_id = id_a < id_b ? id_a : id_b;
    int max_id = id_a > id_b ? id_a : id_b;
    for (int p = 0; p < CP_HASH_SIZE; p++) {
        int idx = (h + p) & CP_HASH_MASK;
        CoocEntry* e = &g_pair_cooc->table[idx];
        if (e->cooc == 0) return 0;
        if (e->node_a == min_id && e->node_b == max_id) return e->cooc;
    }
    return 0;
}

static int cp_tracker_get_single(int id) {
    if (!g_single_count || id < 0 || id >= g_single_capacity) return 0;
    return g_single_count[id];
}

static int cp_tracker_should_create(int id_a, int id_b) {
    int cooc = cp_tracker_get_cooc(id_a, id_b);
    if (cooc < COMPOUND_N_MIN) return 0;
    int ta = cp_tracker_get_single(id_a);
    int tb = cp_tracker_get_single(id_b);
    if (ta <= 0 || tb <= 0) return 0;
    float p_ba = (float)cooc / (float)ta;  /* P(B|A) */
    float p_ab = (float)cooc / (float)tb;  /* P(A|B) */
    return (p_ba >= COMPOUND_THETA || p_ab >= COMPOUND_THETA) ? 1 : 0;
}

/* 继承源节点 A 和 B 的出边到组合节点 cn，权重取平均或按 0.7 折 */
static void cp_inherit_edges(HuarongTopologyNet* net, ReasoningNode* cn,
                              ReasoningNode* na, ReasoningNode* nb) {
    if (!net || !cn || !na || !nb) return;
    for (int e = 0; e < na->edge_count; e++) {
        if (!na->edges[e].target) continue;
        float w = na->edges[e].weight;
        float wb = 0.0f; int found = 0;
        for (int eb = 0; eb < nb->edge_count; eb++) {
            if (nb->edges[eb].target == na->edges[e].target) {
                wb = nb->edges[eb].weight; found = 1; break;
            }
        }
        huarong_net_add_connection(net, cn->node_id,
            na->edges[e].target->node_id, found ? (w + wb) * 0.5f : w * 0.7f);
    }
    for (int e = 0; e < nb->edge_count; e++) {
        if (!nb->edges[e].target) continue;
        int dup = 0;
        for (int ea = 0; ea < na->edge_count; ea++) {
            if (na->edges[ea].target == nb->edges[e].target) { dup = 1; break; }
        }
        if (!dup)
            huarong_net_add_connection(net, cn->node_id,
                nb->edges[e].target->node_id, nb->edges[e].weight * 0.7f);
    }
}

static int get_or_create_concept(SubTopology* topo, const char* concept) {
    if (!topo || !topo->net || !concept) return -1;
    int existing = huarong_net_find_concept(topo->net, concept);
    if (existing >= 0) return existing;
    float feat[NODE_FEATURE_DIM];
    for (int i = 0; i < NODE_FEATURE_DIM; i++)
        feat[i] = ((float)rand() / RAND_MAX - 0.5f) * 0.1f;
    ReasoningNode* node = huarong_net_add_node(topo->net, concept, feat, NODE_FEATURE_DIM);
    return node ? node->node_id : -1;
}

/* ==================== 自动学习概念（条件概率驱动版）==================== */
void auto_learn_concepts(MasterTopology* master, const char* text, void* str_pool) {
    (void)str_pool;
    if (!master || !text || strlen(text) == 0) return;

    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    if (!vocab || !vocab->net) return;
    cp_tracker_ensure(vocab->net->max_nodes);

    /* UTF-8 分词（中文按单字切） */
    char* tokens[128];
    int token_count = utf8_tokenize(text, tokens, 128);
    if (token_count <= 0) return;

    /* 提取中文单字（跳过标点/停用词），获取或创建节点ID */
    int cjk_pos[128], cjk_ids[128], cjk_count = 0;
    for (int i = 0; i < token_count && cjk_count < 128; i++) {
        if (!tokens[i] || strlen(tokens[i]) != 3) continue;
        unsigned char c0 = (unsigned char)tokens[i][0];
        if ((c0 & 0x80) == 0) continue;
        if (contains_punctuation(tokens[i]) || is_stop_word(tokens[i])) continue;
        int nid = get_or_create_concept(vocab, tokens[i]);
        if (nid >= 0) {
            cjk_pos[cjk_count] = i;
            cjk_ids[cjk_count] = nid;
            cjk_count++;
        }
    }
    if (cjk_count <= 0) { for (int i = 0; i < token_count; i++) free(tokens[i]); return; }

    /* 收集所有参与节点（单字 + 将创建的组合词）用于后续建边 */
    int node_ids[128], node_count = 0;
    for (int i = 0; i < cjk_count; i++) node_ids[node_count++] = cjk_ids[i];

    /* ---- 条件概率驱动的组合节点创建 ---- */
    for (int i = 0; i < cjk_count - 1; i++) {
        int id_a = cjk_ids[i], id_b = cjk_ids[i + 1];
        cp_tracker_record(id_a, id_b);
        if (!cp_tracker_should_create(id_a, id_b)) continue;

        char cname[13];  /* 4 CJK chars max (12 bytes) + null */
        snprintf(cname, sizeof(cname), "%s%s",
                 tokens[cjk_pos[i]], tokens[cjk_pos[i+1]]);
        int existing = huarong_net_find_concept(vocab->net, cname);
        if (existing >= 0) {
            if (existing < vocab->net->node_count && vocab->net->nodes[existing])
                vocab->net->nodes[existing]->activation += 0.05f;
            continue;
        }

        /* 特征向量 = 两源节点均值 */
        float feat[NODE_FEATURE_DIM];
        ReasoningNode *na = vocab->net->nodes[id_a], *nb = vocab->net->nodes[id_b];
        if (na && na->features && nb && nb->features) {
            for (int d = 0; d < NODE_FEATURE_DIM; d++)
                feat[d] = (na->features[d] + nb->features[d]) * 0.5f;
        } else {
            for (int d = 0; d < NODE_FEATURE_DIM; d++)
                feat[d] = ((float)rand() / RAND_MAX - 0.5f) * 0.1f;
        }

        ReasoningNode* cn = huarong_net_add_node(vocab->net, cname, feat, NODE_FEATURE_DIM);
        if (cn) {
            cp_inherit_edges(vocab->net, cn, na, nb);
            /* 将新创建的组合节点也加入滑动窗口候选 */
            if (node_count < 128) node_ids[node_count++] = cn->node_id;
        }
    }

    /* ---- 扫描已有组合节点，加入滑动窗口候选 ---- */
    for (int i = 0; i < cjk_count - 1 && node_count < 128; i++) {
        char cname[13];
        snprintf(cname, sizeof(cname), "%s%s",
                 tokens[cjk_pos[i]], tokens[cjk_pos[i+1]]);
        int cnid = huarong_net_find_concept(vocab->net, cname);
        if (cnid >= 0) {
            /* 去重：检查是否已在 node_ids 中 */
            int dup = 0;
            for (int k = 0; k < node_count; k++)
                if (node_ids[k] == cnid) { dup = 1; break; }
            if (!dup) node_ids[node_count++] = cnid;
        }
    }

    /* ---- 滑动窗口建边（对已有节点） ---- */
    #define COOCCUR_WINDOW 5
    for (int i = 0; i < node_count; i++) {
        int j_max = (i + COOCCUR_WINDOW < node_count) ? i + COOCCUR_WINDOW : node_count;
        for (int j = i + 1; j < j_max; j++) {
            int found = 0;
            ReasoningNode* na = vocab->net->nodes[node_ids[i]];
            if (na) {
                for (int k = 0; k < na->edge_count; k++) {
                    if (na->edges[k].target &&
                        na->edges[k].target->node_id == node_ids[j]) {
                        na->edges[k].weight += 0.1f;
                        if (na->edges[k].weight > 1.0f) na->edges[k].weight = 1.0f;
                        found = 1; break;
                    }
                }
            }
            if (!found)
                huarong_net_add_connection(vocab->net, node_ids[i], node_ids[j], 0.5f);
        }
    }

    /* ---- Hebbian 在线更新 ---- */
    float lr = 0.02f;
    for (int i = 0; i < node_count; i++) {
        ReasoningNode* ni = vocab->net->nodes[node_ids[i]];
        if (!ni || !ni->features || ni->feature_dim != NODE_FEATURE_DIM) continue;
        for (int j = i + 1; j < node_count; j++) {
            ReasoningNode* nj = vocab->net->nodes[node_ids[j]];
            if (!nj || !nj->features || nj->feature_dim != NODE_FEATURE_DIM) continue;
            hebbian_update(ni->features, nj->features, NODE_FEATURE_DIM, lr);
        }
    }

    for (int i = 0; i < token_count; i++) free(tokens[i]);
}



