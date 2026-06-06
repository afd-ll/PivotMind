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
#include "utf8_tokenizer.h"
#include "string_pool.h"
#include "common.h"
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
        if (vocab && vocab->net && anc_tok_count > 0) {
            for (int t = 0; t < anc_tok_count; t++) {
                if (!anchor_tokens[t]) continue;
                for (int n = 0; n < vocab->net->node_count && matched < 32; n++) {
                    ReasoningNode* node = vocab->net->nodes[n];
                    if (!node || !node->concept || !node->features) continue;
                    if (strcmp(node->concept, anchor_tokens[t]) == 0) {
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
    
    // 拓扑驱动生成：联想推理产出的概念
    if (dsys && dsys->master && dsys->master->sub_topo_count > 0) {
        int total_nodes = 0;
        for (int t = 0; t < dsys->master->sub_topo_count; t++) {
            SubTopology* sub = dsys->master->sub_topologies[t];
            if (sub && sub->net) total_nodes += sub->net->node_count;
        }
        if (total_nodes >= 10) {
            char* topo_response = master_generate_response(
                dsys->master, input, max_len);
            if (topo_response && strlen(topo_response) > 0) {
                char* safe = strdup(topo_response);
                free(topo_response);
                return safe;
            }
            if (topo_response) free(topo_response);
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

            // ===== 路径转输出：模板驱动结构化 =====
            // 对相邻概念对 (path[p-1], path[p]) 查模板匹配。
            // 匹配 → 用模板代表概念作为桥梁，输出 "A的[代表概念]是B"
            // 无匹配 → 纯拼接 + 连接词兜底
            const char* last_concept = NULL;
            const char* connectives[] = {"", "的", "是", "和", "了", "在"};
            int conn_count = sizeof(connectives)/sizeof(connectives[0]);
            int conn_idx = 0;

            // 先输出路径起点本身的语义
            {
                ReasoningNode* start_node = (path_nodes[0] >= 0 && path_nodes[0] < sub->net->node_count)
                    ? sub->net->nodes[path_nodes[0]] : NULL;
                if (start_node && start_node->concept) {
                    pos += snprintf(response + pos, max_len - pos, "%s", start_node->concept);
                    last_concept = start_node->concept;
                }
            }
            
            for (int p = 1; p < path_len && pos < max_len - 10; p++) {
                int nid = path_nodes[p];
                if (nid < 0 || nid >= sub->net->node_count) continue;
                ReasoningNode* node = sub->net->nodes[nid];
                if (!node || !node->concept) continue;
                
                // 去重
                if (last_concept && strcmp(last_concept, node->concept) == 0) continue;
                
                // --- 模板锚点对匹配 ---
                int prev_nid = path_nodes[p-1];
                int tpl_id = -1;
                if (dsys && dsys->master && dsys->master->use_template_voting
                    && prev_nid >= 0 && prev_nid < sub->net->node_count) {
                    tpl_id = master_find_template_for_pair(
                        dsys->master, sub->topo_id, prev_nid, nid);
                }

                if (tpl_id >= 0) {
                    // 语法模板匹配：使用模板的连接词连接当前节点
                    const char* connector = template_get_connector(
                        dsys->master, tpl_id, 0);
                    if (connector && connector[0]) {
                        // 先输出连接词，再输出当前概念（如 "的苹果"）
                        pos += snprintf(response + pos, max_len - pos,
                            "%s%s", connector, node->concept);
                    } else {
                        // 无连接词：直接拼接（如主谓/动宾结构）
                        pos += snprintf(response + pos, max_len - pos,
                            "%s", node->concept);
                    }
                } else {
                    // 无模板匹配：连接词兜底
                    if (p > 1 && p < path_len - 1 && (p % 3 == 0)
                        && strlen(connectives[conn_idx]) > 0) {
                        pos += snprintf(response + pos, max_len - pos, "%s",
                                        connectives[conn_idx]);
                        conn_idx = (conn_idx + 1) % conn_count;
                    }
                    pos += snprintf(response + pos, max_len - pos, "%s", node->concept);
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

        // 兜底：走边没走出结果，输出最高激活概念
        if (pos == 0 && reasoning->assoc_count > 0) {
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
                        for (int e = 0; e < from->connection_count; e++) {
                            if (from->connections[e] && from->connections[e]->node_id == a->node_id) {
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

        return response;
    }
    
    return strdup("...");
}

// ==================== 自动学习概念到拓扑网络 ====================
// 真正的学习应该像人脑一样，通过对话自然发生，而不是人为预先连接神经回路
// 实现：从对话文本中提取有意义的概念，加入拓扑网络并建立共现连接

// 中文停用词（过滤高频无意义字词）
static const char* STOP_WORDS[] = {
    "的", "了", "是", "在", "有", "和", "就", "不", "都", "而",
    "及", "与", "着", "或", "也", "很", "会", "可", "但", "这",
    "那", "上", "下", "到", "去", "来", "为", "以", "能", "要",
    "我", "你", "他", "她", "它", "们", "个", "之", "对", "被",
    "把", "让", "向", "从", "比", "还", "又", "再", "才", "啊",
    "吧", "吗", "呢", "哈", "呀", "哦", "嗯", "嘛", "啦", "哇",
    // 注：疑问词（什么/怎么/为什么/如何）已从停用词移除 — 它们是语义核心载体
    // 标点符号
    "，", "。", "、", "；", "：", "？", "！", "…", "—", "～",
    "·", "．", "（", "）", "【", "】", "《", "》", "”", "“",
    "‘", "’", "　"
};
#define STOP_WORDS_COUNT (sizeof(STOP_WORDS) / sizeof(STOP_WORDS[0]))

static int is_stop_word(const char* word) {
    if (!word || strlen(word) == 0) return 1;
    for (int i = 0; i < (int)STOP_WORDS_COUNT; i++) {
        if (strcmp(word, STOP_WORDS[i]) == 0) return 1;
    }
    return 0;
}

// 检查字符串是否包含中文或英文标点
static int contains_punctuation(const char* s) {
    if (!s) return 0;
    const char* punct = "，。、；：？！…—～·．（）【】《》”“‘’　,.;:!?\"'()[]{}<>/\\@#$%^&*_+-=~`";
    for (const char* p = punct; *p; p++) {
        if (strchr(s, *p)) return 1;
    }
    return 0;
}

static int concept_exists(HuarongTopologyNet* net, const char* concept) {
    if (!net || !concept) return -1;
    for (int i = 0; i < net->node_count; i++) {
        if (net->nodes[i] && net->nodes[i]->concept &&
            strcmp(net->nodes[i]->concept, concept) == 0) {
            return i;  // 返回已有节点ID
        }
    }
    return -1;  // 不存在
}

static int get_or_create_concept(SubTopology* topo, const char* concept) {
    if (!topo || !topo->net || !concept) return -1;
    int existing = concept_exists(topo->net, concept);
    if (existing >= 0) return existing;
    float feat[NODE_FEATURE_DIM];
    for (int i = 0; i < NODE_FEATURE_DIM; i++)
        feat[i] = ((float)rand() / RAND_MAX - 0.5f) * 0.1f;
    ReasoningNode* node = huarong_net_add_node(topo->net, concept, feat, NODE_FEATURE_DIM);
    return node ? node->node_id : -1;
}

void auto_learn_concepts(MasterTopology* master, const char* text, void* str_pool) {
    (void)str_pool;  // 保留供后续字符串池优化使用
    if (!master || !text || strlen(text) == 0) return;

    // 获取词汇拓扑和语义拓扑
    SubTopology* vocab = master_get_sub_topology_by_type(master, TOPO_VOCABULARY);
    SubTopology* semantic = master_get_sub_topology_by_type(master, TOPO_SEMANTIC);
    if (!vocab || !vocab->net || !semantic || !semantic->net) return;

    // 分词
    char* tokens[64];
    int token_count = utf8_tokenize(text, tokens, 64);
    if (token_count <= 0) return;

    // 第一步：从单个字符组合成有意义的双字/三字概念
    // 对于中文，连续中文字符合并为2~3字窗口
    char* concepts[64];
    int concept_count = 0;

    for (int i = 0; i < token_count && concept_count < 64; i++) {
        // 跳过停用词和含标点的词
        if (is_stop_word(tokens[i]) || contains_punctuation(tokens[i])) continue;

        // 英文/ASCII词直接作为概念
        if (!tokens[i] || strlen(tokens[i]) == 0) continue;
        unsigned char c = (unsigned char)tokens[i][0];
        if ((c & 0x80) == 0) {
            // ASCII token（英文词、数字等）
            if (strlen(tokens[i]) >= 2) {  // 至少2个字符才有意义
                concepts[concept_count++] = strdup(tokens[i]);
            }
        }
    }

    // 对于中文单字，尝试组合成双字词
    // 收集所有中文单字位置
    int chinese_pos[64], chinese_count = 0;
    for (int i = 0; i < token_count && chinese_count < 64; i++) {
        if (!tokens[i]) continue;
        unsigned char c = (unsigned char)tokens[i][0];
        if ((c & 0x80) != 0 && strlen(tokens[i]) == 3) {  // 中文字符
            chinese_pos[chinese_count++] = i;
        }
    }

    // 组合双字词（前后相邻的中文字符）
    for (int i = 0; i < chinese_count - 1 && concept_count < 64; i++) {
        char bigram[7];  // 2个中文字符(3+3) + 终止符
        snprintf(bigram, sizeof(bigram), "%s%s",
                 tokens[chinese_pos[i]], tokens[chinese_pos[i + 1]]);
        if (!is_stop_word(bigram) && !contains_punctuation(bigram)) {
            concepts[concept_count++] = strdup(bigram);
        }
    }

    // 第二步：将概念加入词汇拓扑
    int concept_ids[64];
    int valid_count = 0;
    for (int i = 0; i < concept_count; i++) {
        int id = get_or_create_concept(vocab, concepts[i]);
        if (id >= 0) {
            concept_ids[valid_count++] = id;
        }
    }

    // 第三步：在词汇拓扑中建立共现连接（滑动窗口，窗口=5）
    // 原全连接 O(n^2) → 滑动窗口 O(n×5)，大幅减少噪音边
    #define COOCCUR_WINDOW 5
    for (int i = 0; i < valid_count; i++) {
        int j_max = (i + COOCCUR_WINDOW < valid_count) ? i + COOCCUR_WINDOW : valid_count;
        for (int j = i + 1; j < j_max; j++) {
            int conn_exists = 0;
            ReasoningNode* node_a = vocab->net->nodes[concept_ids[i]];
            if (node_a) {
                for (int k = 0; k < node_a->connection_count; k++) {
                    ReasoningNode* target = node_a->connections[k];
                    if (target && target->node_id == concept_ids[j]) {
                        node_a->connection_weights[k] += 0.1f;
                        if (node_a->connection_weights[k] > 1.0f)
                            node_a->connection_weights[k] = 1.0f;
                        conn_exists = 1;
                        break;
                    }
                }
            }
            if (!conn_exists) {
                huarong_net_add_connection(vocab->net,
                    concept_ids[i], concept_ids[j], 0.5f);
            }
        }
    }
    
    // 第四步：在线更新节点 embedding（Hebbian: 共现节点互相拉近）
    float lr = 0.02f;
    for (int i = 0; i < valid_count; i++) {
        ReasoningNode* ni = vocab->net->nodes[concept_ids[i]];
        if (!ni || !ni->features || ni->feature_dim != NODE_FEATURE_DIM) continue;
        for (int j = i + 1; j < valid_count; j++) {
            ReasoningNode* nj = vocab->net->nodes[concept_ids[j]];
            if (!nj || !nj->features || nj->feature_dim != NODE_FEATURE_DIM) continue;
            hebbian_update(ni->features, nj->features, NODE_FEATURE_DIM, lr);
        }
    }

    // 清理
    for (int i = 0; i < token_count; i++) {
        free(tokens[i]);
    }
    for (int i = 0; i < concept_count; i++) {
        free(concepts[i]);
    }
}



