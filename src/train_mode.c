/**
 * @file train_mode.c
 * @brief 内置训练模式实现 — 不停机喂料
 *
 * 无外部依赖，JSON解析使用内置轻量解析器。
 */

#include "train_mode.h"
#include "huarong_topology.h"
#include "feature_io.h"
#include "article_reader.h"
#include "thalamus.h"
#include "error.h"
#include "utf8_tokenizer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include <stdarg.h>
#include <sys/stat.h>

// ==================== 内部函数 ====================

static void train_set_error(TrainMode* tm, const char* fmt, ...);
static void* train_thread_func(void* arg);
static int train_feed_qa_json(TrainMode* tm, const char* path);
static int train_feed_pipe_qa(TrainMode* tm, const char* path);
static int train_feed_plain_text(TrainMode* tm, const char* path);
static int train_feed_article(TrainMode* tm, const char* path);
static int train_feed_one_line(TrainMode* tm, const char* text);
static int train_feed_token_sequence(TrainMode* tm, SubTopology* vocab,
                                      char** tokens, int token_count,
                                      float edge_weight,
                                      int* first_id_out, int* last_id_out);
static void train_do_batch_learn(TrainMode* tm);
static void train_do_auto_save(TrainMode* tm, const char* workdir);
static void train_throttled_delay(TrainMode* tm, long count);
static long train_count_lines(const char* path);

// ==================== 轻量JSON QA解析 ====================

/**
 * 从 [["q","a"], ...] 格式的JSON中流式提取QA对
 * 避免一次性加载整个JSON到内存（RK3399只有3.8GB）
 *
 * 实现方式：逐字符状态机，遇到 ["时进入读取模式
 * 返回值：0=还有更多，1=到达末尾，-1=出错
 */
typedef struct {
    FILE* f;
    int state;       // 0=外层, 1=内层数组, 2=读q, 3=读a
    char q_buf[1024];
    char a_buf[2048];
} JsonQaStream;

static JsonQaStream* json_qa_stream_open(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return NULL;

    JsonQaStream* s = calloc(1, sizeof(JsonQaStream));
    if (!s) { fclose(f); return NULL; }
    s->f = f;
    s->state = 0;
    return s;
}

static void json_qa_stream_close(JsonQaStream* s) {
    if (s) {
        if (s->f) fclose(s->f);
        free(s);
    }
}

/**
 * 读取一个JSON字符串字面量（从当前位置的双引号开始）
 * 写入buf，返回0=成功，-1=失败
 */
static int json_read_string_literal(FILE* f, char* buf, int buf_size) {
    int ch = fgetc(f);
    if (ch != '"') return -1;

    int i = 0;
    while (i < buf_size - 1) {
        ch = fgetc(f);
        if (ch == EOF) return -1;
        if (ch == '\\') {
            // 转义字符
            ch = fgetc(f);
            if (ch == EOF) return -1;
            switch (ch) {
                case 'n': buf[i++] = '\n'; break;
                case 't': buf[i++] = '\t'; break;
                case 'r': buf[i++] = '\r'; break;
                case '\\': buf[i++] = '\\'; break;
                case '"': buf[i++] = '"'; break;
                case '/': buf[i++] = '/'; break;
                case 'u':
                    // Unicode转义 \uXXXX → UTF-8
                    {
                        char hex[4] = {0};
                        int valid = 1;
                        for (int u = 0; u < 4; u++) {
                            int h = fgetc(f);
                            if (h == EOF || !isxdigit(h)) valid = 0;
                            hex[u] = (char)h;
                        }
                        if (valid) {
                            unsigned int cp = 0;
                            for (int u = 0; u < 4; u++) {
                                cp = cp * 16 + (unsigned int)(
                                    hex[u] >= '0' && hex[u] <= '9' ? hex[u] - '0' :
                                    hex[u] >= 'a' && hex[u] <= 'f' ? hex[u] - 'a' + 10 :
                                    hex[u] >= 'A' && hex[u] <= 'F' ? hex[u] - 'A' + 10 : 0);
                            }
                            if (cp > 0) {
                                if (cp < 0x80) {
                                    if (i + 1 < buf_size) buf[i++] = (char)cp;
                                } else if (cp < 0x800) {
                                    if (i + 2 < buf_size) {
                                        buf[i++] = (char)(0xC0 | (cp >> 6));
                                        buf[i++] = (char)(0x80 | (cp & 0x3F));
                                    }
                                } else {
                                    if (i + 3 < buf_size) {
                                        buf[i++] = (char)(0xE0 | (cp >> 12));
                                        buf[i++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                                        buf[i++] = (char)(0x80 | (cp & 0x3F));
                                    }
                                }
                            }
                        } else {
                            buf[i++] = '?';  // 无效 hex 用占位符
                        }
                    }
                    break;
                default: buf[i++] = ch; break;
            }
        } else if (ch == '"') {
            break;
        } else {
            buf[i++] = ch;
        }
    }
    buf[i] = 0;
    return 0;
}

/**
 * 读取下一个QA对
 * 返回 1=成功读到, 0=没有更多, -1=出错
 */
static int json_qa_stream_next(JsonQaStream* s) {
    int ch;
    // 跳到下一个 ["  开始
    while (1) {
        ch = fgetc(s->f);
        if (ch == EOF) return 0;
        if (ch == ']') {
            // 可能是外层数组结束
            // 检查后面是不是还有
            while ((ch = fgetc(s->f)) != EOF) {
                if (ch == '[') { ungetc(ch, s->f); break; }
                if (ch == ']') return 0; // 真结束了
            }
            if (ch == EOF) return 0;
            continue;
        }
        if (ch == '[') {
            // 读第一个字符串 (q)
            // 跳过空白
            while ((ch = fgetc(s->f)) != EOF && isspace(ch));
            if (ch == '"') {
                ungetc(ch, s->f);
                if (json_read_string_literal(s->f, s->q_buf, sizeof(s->q_buf)) != 0)
                    continue;

                // 跳过分隔符到第二个字符串
                while ((ch = fgetc(s->f)) != EOF) {
                    if (ch == '"') {
                        ungetc(ch, s->f);
                        break;
                    }
                    if (ch == ']') {
                        // 只有一个元素的数组，跳过
                        s->q_buf[0] = 0;
                        goto next_item;
                    }
                }
                if (ch == EOF) return 0;

                if (json_read_string_literal(s->f, s->a_buf, sizeof(s->a_buf)) != 0)
                    continue;

                // 跳到内层数组结束 ]
                while ((ch = fgetc(s->f)) != EOF && ch != ']');
                return 1;
            }
            next_item:
            // 跳到内层数组结束
            while ((ch = fgetc(s->f)) != EOF && ch != ']');
        }
    }
}


// ==================== 创建/销毁 ====================

TrainMode* train_mode_create(MasterTopology* topology,
                              MemorySystem* memory,
                              ActiveLearner* learner,
                              TrainConfig config) {
    if (!topology || !memory) return NULL;

    TrainMode* tm = calloc(1, sizeof(TrainMode));
    if (!tm) return NULL;

    tm->topology = topology;
    tm->memory = memory;
    tm->learner = learner;
    tm->config = config;

    // 默认值
    if (config.rounds <= 0) tm->config.rounds = 1;
    if (config.speed <= 0) tm->config.speed = 20;
    tm->thalamus = NULL;
    if (config.batch_learn_interval <= 0) tm->config.batch_learn_interval = 100;
    if (config.save_interval <= 0) tm->config.save_interval = 5000;

    tm->progress.state = TRAIN_IDLE;
    tm->progress.total_rounds = tm->config.rounds;

    return tm;
}

void train_mode_destroy(TrainMode* tm) {
    if (!tm) return;
    train_mode_stop(tm);
    // 用 pthread_join 等线程退出（不用 detach 了）
    if (tm->is_running) {
        LOG_INFO("[训练] 等待训练线程退出...");
        pthread_join(tm->thread, NULL);
        LOG_INFO("[训练] 训练线程已退出");
    }
    tm->is_running = 0;
    free(tm);
}

// ==================== 控制 ====================

int train_mode_start(TrainMode* tm) {
    if (!tm || tm->is_running) return -1;

    tm->should_stop = 0;
    tm->should_pause = 0;
    tm->progress.state = TRAIN_RUNNING;
    tm->progress.start_time = time(NULL);

    if (pthread_create(&tm->thread, NULL, train_thread_func, tm) != 0) {
        train_set_error(tm, "无法创建训练线程");
        return -1;
    }
    // 不 detach! 在 destroy 里用 pthread_join 等线程退出
    // pthread_detach(tm->thread);

    printf("[训练] 后台训练线程已启动 (语料=%s, 轮数=%d, 速度=%d条/秒)\n",
           tm->config.corpus_path, tm->config.rounds, tm->config.speed);
    return 0;
}

void train_mode_pause(TrainMode* tm) {
    if (tm && tm->progress.state == TRAIN_RUNNING) {
        tm->should_pause = 1;
        tm->progress.state = TRAIN_PAUSED;
        printf("[训练] 已暂停 (第%d轮, 已喂%ld条)\n",
               tm->progress.current_round, tm->progress.total_fed);
    }
}

void train_mode_resume(TrainMode* tm) {
    if (tm && tm->progress.state == TRAIN_PAUSED) {
        tm->should_pause = 0;
        tm->progress.state = TRAIN_RUNNING;
        printf("[训练] 已继续 (第%d轮)\n", tm->progress.current_round);
    }
}

void train_mode_stop(TrainMode* tm) {
    if (!tm) return;
    tm->should_stop = 1;
    tm->should_pause = 0;  // 解除暂停让线程退出
    if (tm->progress.state == TRAIN_RUNNING || tm->progress.state == TRAIN_PAUSED) {
        tm->progress.state = TRAIN_COMPLETED;
    }
}

TrainProgress train_mode_get_progress(TrainMode* tm) {
    TrainProgress p = {0};
    if (tm) p = tm->progress;
    return p;
}

// ==================== 内部：训练线程 ====================

static void* train_thread_func(void* arg) {
    TrainMode* tm = (TrainMode*)arg;
    tm->is_running = 1;

    const char* path = tm->config.corpus_path;

    // 先统计语料规模
    if (tm->config.format == CORPUS_JSON_QA) {
        // JSON用文件大小估算
        struct stat st;
        if (stat(path, &st) == 0) {
            tm->progress.total_lines = st.st_size / 80;  // 平均80字节一条
        }
    } else {
        tm->progress.total_lines = train_count_lines(path);
    }

    printf("[训练] 语料: %s (约%ld条), 格式: %s\n",
           path, tm->progress.total_lines,
           tm->config.format == CORPUS_JSON_QA ? "JSON QA" :
           tm->config.format == CORPUS_PIPE_QA ? "管道QA" :
           tm->config.format == CORPUS_ARTICLE ? "文章阅读" : "纯文本");

    for (int round = 1; round <= tm->config.rounds && !tm->should_stop; round++) {
        tm->progress.current_round = round;
        tm->progress.current_line = 0;
        tm->progress.total_fed = 0;

        printf("[训练] === 第 %d/%d 轮开始 ===\n", round, tm->config.rounds);

        int ok = 0;
        switch (tm->config.format) {
            case CORPUS_JSON_QA:   ok = train_feed_qa_json(tm, path); break;
            case CORPUS_PIPE_QA:   ok = train_feed_pipe_qa(tm, path); break;
            case CORPUS_PLAIN_TEXT: ok = train_feed_plain_text(tm, path); break;
            case CORPUS_ARTICLE:   ok = train_feed_article(tm, path); break;
        }

        if (ok < 0) {
            tm->progress.state = TRAIN_ERROR;
            break;
        }

        printf("[训练] === 第 %d 轮完成, 喂了 %ld 条 ===\n", round, tm->progress.total_fed);

        // 轮间触发一次主动学习
        if (tm->learner) {
            printf("[训练] 触发主动学习...\n");
            learn_from_memory(tm->learner);
            // discover_new_relations: 轮间也不调用，全部延迟到训练结束
        }
    }

    // 训练结束
    if (!tm->should_stop) {
        printf("[训练] 全部完成! 累计喂料 %ld 条, 新增节点 %ld, 新建边 %ld\n",
               tm->progress.total_fed, tm->progress.total_added_nodes,
               tm->progress.total_added_edges);

        // 通过丘脑报告本轮训练完成
        if (tm->thalamus) {
            thalamus_send_feedback(tm->thalamus, THAL_HIPPOCAMPUS,
                                   (int)(tm->progress.total_added_nodes +
                                         tm->progress.total_learned), 0, 0);
        }

        // 增量关系发现：训练结束后执行一次全图关系发现
        // 之前禁用的 discover_new_relations 在此安全执行
        if (tm->learner) {
            printf("[训练] 执行增量关系发现...\n");
            discover_new_relations(tm->learner);
        }

        tm->progress.state = TRAIN_COMPLETED;
    } else {
        printf("[训练] 用户停止训练\n");
    }

    tm->is_running = 0;
    return NULL;
}

// ==================== JSON QA 流式喂料 ====================

static int train_feed_qa_json(TrainMode* tm, const char* path) {
    JsonQaStream* stream = json_qa_stream_open(path);
    if (!stream) {
        train_set_error(tm, "无法打开语料: %s", path);
        return -1;
    }

    long count = 0;
    int ret;

    while ((ret = json_qa_stream_next(stream)) > 0 && !tm->should_stop) {
        // 暂停检查
        while (tm->should_pause && !tm->should_stop) {
            usleep(200000); // 200ms
        }
        if (tm->should_stop) break;

        if (strlen(stream->q_buf) == 0) continue;

        // Q→A 直接强连接：Q和A作为独立概念节点，建一条强边
        // 不再拼接成句子再分词，避免虚词连接和序列噪音
        {
            SubTopology* vocab = NULL;
            for (int t = 0; t < tm->topology->sub_topo_count; t++) {
                if (tm->topology->sub_topologies[t] && tm->topology->sub_topologies[t]->type == TOPO_VOCABULARY) {
                    vocab = tm->topology->sub_topologies[t];
                    break;
                }
            }
            if (vocab && vocab->net) {
                int qid = huarong_net_find_concept(vocab->net, stream->q_buf);
                if (qid < 0 && vocab->net->node_count < vocab->net->max_nodes)
                    qid = huarong_net_dynamic_add_node(vocab->net, stream->q_buf, NULL, 0);
                int aid = huarong_net_find_concept(vocab->net, stream->a_buf);
                if (aid < 0 && vocab->net->node_count < vocab->net->max_nodes)
                    aid = huarong_net_dynamic_add_node(vocab->net, stream->a_buf, NULL, 0);

                if (qid >= 0 && aid >= 0 && qid != aid) {
                    vocab->net->nodes[qid]->activation += 0.2f;
                    vocab->net->nodes[aid]->activation += 0.2f;
                    ReasoningNode* prev = vocab->net->nodes[qid];
                    int exists = node_conn_find(prev, vocab->net->nodes[aid]);
                    huarong_net_add_connection(vocab->net, qid, aid, 0.8f);
                    if (exists < 0) {
                        tm->progress.total_added_edges++;
                    }
                }
                tm->progress.total_fed++;
            }
        }
        count++;
        tm->progress.current_line = count;

        // 限速（认知感知版）
        train_throttled_delay(tm, count);

        // 批量学习
        if (tm->progress.total_fed > 0 &&
            tm->progress.total_fed % tm->config.batch_learn_interval == 0) {
            train_do_batch_learn(tm);
        }

        // 自动存盘
        if (tm->progress.total_fed > 0 &&
            tm->progress.total_fed % tm->config.save_interval == 0) {
            train_do_auto_save(tm, ".");
        }

        // 进度日志
        if (count % 1000 == 0) {
            printf("[训练] 第%d轮 进度: %ld条, 新增节点=%ld, 新建边=%ld\n",
                   tm->progress.current_round, count,
                   tm->progress.total_added_nodes, tm->progress.total_added_edges);
        }
    }

    json_qa_stream_close(stream);
    tm->progress.total_lines = count;  // 更新实际条数
    return 0;
}

// ==================== 辅助：逐字token序列建节点+序贯边 ====================

#define TRAIN_MAX_TOKENS 2048

/**
 * 将 utf8_tokenize 产出的 token 数组逐个建节点，相邻 token 间拉序贯边。
 * 返回新增节点数；first_id_out / last_id_out 输出首尾节点 id（用于跨句连接）。
 */
static int train_feed_token_sequence(TrainMode* tm, SubTopology* vocab,
                                      char** tokens, int token_count,
                                      float edge_weight,
                                      int* first_id_out, int* last_id_out) {
    int prev_id = -1;
    int first_id = -1;
    int last_id = -1;
    int added = 0;

    for (int i = 0; i < token_count; i++) {
        int nid = huarong_net_find_concept(vocab->net, tokens[i]);
        if (nid < 0 && vocab->net->node_count < vocab->net->max_nodes) {
            nid = huarong_net_dynamic_add_node(vocab->net, tokens[i], NULL, 0);
            if (nid >= 0) {
                added++;
                tm->progress.total_added_nodes++;
            }
        }
        if (nid >= 0) {
            int lk = nid & (PM_NODE_LOCK_COUNT - 1);
            pthread_mutex_lock(&vocab->net->node_locks[lk]);
            vocab->net->nodes[nid]->activation += 0.1f;
            pthread_mutex_unlock(&vocab->net->node_locks[lk]);
            if (first_id < 0) first_id = nid;
            if (prev_id >= 0 && prev_id != nid) {
                ReasoningNode* prev_node = vocab->net->nodes[prev_id];
                int exists = node_conn_find(prev_node, vocab->net->nodes[nid]);
                huarong_net_add_connection(vocab->net, prev_id, nid, edge_weight);
                if (exists < 0) tm->progress.total_added_edges++;
            }
            prev_id = nid;
            last_id = nid;
        }
    }

    if (first_id_out)  *first_id_out  = first_id;
    if (last_id_out)   *last_id_out   = last_id;
    return added;
}

// ==================== 管道QA ====================

static int train_feed_pipe_qa(TrainMode* tm, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        train_set_error(tm, "无法打开语料: %s", path);
        return -1;
    }

    long line_no = 0;
    while (fgets(tm->line_buf, sizeof(tm->line_buf), f) && !tm->should_stop) {
        while (tm->should_pause && !tm->should_stop) usleep(200000);
        if (tm->should_stop) break;

        line_no++;
        int slen = strlen(tm->line_buf);
        while (slen > 0 && (tm->line_buf[slen-1] == '\n' || tm->line_buf[slen-1] == '\r'))
            tm->line_buf[--slen] = 0;
        if (slen == 0) continue;

        char* pipe = strchr(tm->line_buf, '|');
        if (!pipe) continue;
        *pipe = 0;

        char* q = tm->line_buf;
        char* a = pipe + 1;
        if (strlen(q) == 0) continue;

        // 逐字建节点 + 序贯边 + Q→A跨连接
        {
            SubTopology* vocab = NULL;
            for (int t = 0; t < tm->topology->sub_topo_count; t++) {
                if (tm->topology->sub_topologies[t] && tm->topology->sub_topologies[t]->type == TOPO_VOCABULARY) {
                    vocab = tm->topology->sub_topologies[t];
                    break;
                }
            }
            if (vocab && vocab->net) {
                char* q_tokens[TRAIN_MAX_TOKENS];
                char* a_tokens[TRAIN_MAX_TOKENS];

                int q_count = utf8_tokenize(q, q_tokens, TRAIN_MAX_TOKENS);
                int a_count = utf8_tokenize(a, a_tokens, TRAIN_MAX_TOKENS);

                int q_last = -1, a_first = -1, a_last = -1;

                if (q_count > 0)
                    train_feed_token_sequence(tm, vocab, q_tokens, q_count,
                                              0.35f, NULL, &q_last);
                if (a_count > 0)
                    train_feed_token_sequence(tm, vocab, a_tokens, a_count,
                                              0.35f, &a_first, &a_last);

                // Q尾 → A首 强连接
                if (q_last >= 0 && a_first >= 0 && q_last != a_first) {
                    ReasoningNode* qn = vocab->net->nodes[q_last];
                    int exists = node_conn_find(qn, vocab->net->nodes[a_first]);
                    huarong_net_add_connection(vocab->net, q_last, a_first, 0.8f);
                    if (exists < 0) tm->progress.total_added_edges++;
                }

                // 释放 token 内存
                for (int i = 0; i < q_count; i++) free(q_tokens[i]);
                for (int i = 0; i < a_count; i++) free(a_tokens[i]);

                tm->progress.total_fed++;
            }
        }
        tm->progress.current_line = line_no;

        // 限速（认知感知版）
        train_throttled_delay(tm, line_no);

        if (tm->progress.total_fed > 0 &&
            tm->progress.total_fed % tm->config.batch_learn_interval == 0) {
            train_do_batch_learn(tm);
        }

        if (tm->progress.total_fed > 0 &&
            tm->progress.total_fed % tm->config.save_interval == 0) {
            train_do_auto_save(tm, ".");
        }

        if (line_no % 1000 == 0) {
            printf("[训练] 第%d轮 进度: %ld行, 新增节点=%ld, 新建边=%ld\n",
                   tm->progress.current_round, line_no,
                   tm->progress.total_added_nodes, tm->progress.total_added_edges);
        }
    }

    fclose(f);
    return 0;
}

// ==================== 纯文本 ====================

static int train_feed_plain_text(TrainMode* tm, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        train_set_error(tm, "无法打开语料: %s", path);
        return -1;
    }

    long line_no = 0;
    while (fgets(tm->line_buf, sizeof(tm->line_buf), f) && !tm->should_stop) {
        while (tm->should_pause && !tm->should_stop) usleep(200000);
        if (tm->should_stop) break;

        line_no++;
        int slen = strlen(tm->line_buf);
        while (slen > 0 && (tm->line_buf[slen-1] == '\n' || tm->line_buf[slen-1] == '\r'))
            tm->line_buf[--slen] = 0;
        if (slen < 4) continue;

        if (train_feed_one_line(tm, tm->line_buf) > 0) {
            tm->progress.total_fed++;
        }
        tm->progress.current_line = line_no;

        // 限速（认知感知版）
        train_throttled_delay(tm, line_no);

        if (tm->progress.total_fed > 0 &&
            tm->progress.total_fed % tm->config.batch_learn_interval == 0) {
            train_do_batch_learn(tm);
        }

        if (tm->progress.total_fed > 0 &&
            tm->progress.total_fed % tm->config.save_interval == 0) {
            train_do_auto_save(tm, ".");
        }

        if (line_no % 5000 == 0) {
            printf("[训练] 第%d轮 进度: %ld行, 新增节点=%ld, 新建边=%ld\n",
                   tm->progress.current_round, line_no,
                   tm->progress.total_added_nodes, tm->progress.total_added_edges);
        }
    }

    fclose(f);
    return 0;
}

// ==================== 文章阅读 ====================

static int train_feed_article(TrainMode* tm, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        train_set_error(tm, "无法打开语料: %s", path);
        return -1;
    }

    // 创建文章阅读器
    ArticleReaderConfig ar_cfg = ARTICLE_READER_DEFAULT_CONFIG;
    ar_cfg.verbose = tm->config.verbose;
    ArticleReader* ar = article_reader_create(tm->topology, &ar_cfg);
    if (!ar) {
        train_set_error(tm, "文章阅读器创建失败");
        fclose(f);
        return -1;
    }

    article_reader_set_thalamus(ar, tm->thalamus);

    // 设置进度指针
    article_set_progress_ptr(ar,
                             &tm->progress.total_added_nodes,
                             &tm->progress.total_added_edges);

    long line_no = 0;
    int total_flushes = 0;

    while (fgets(tm->line_buf, sizeof(tm->line_buf), f) && !tm->should_stop) {
        while (tm->should_pause && !tm->should_stop) usleep(200000);
        if (tm->should_stop) break;

        line_no++;
        int slen = strlen(tm->line_buf);
        while (slen > 0 && (tm->line_buf[slen-1] == '\n' || tm->line_buf[slen-1] == '\r'))
            tm->line_buf[--slen] = 0;
        if (slen < 3) continue;  // 跳过空行/短行

        int result = article_process_line(ar, tm->line_buf);
        if (result > 0) {
            total_flushes++;
            tm->progress.total_fed++;
        }

        tm->progress.current_line = line_no;

        // 限速（认知感知版）
        train_throttled_delay(tm, line_no);

        // 进度日志
        if (line_no % 1000 == 0) {
            int nchars, npairs, nwords;
            article_get_stats(ar, &nchars, &npairs, &nwords);
            printf("[文章阅读] 第%d轮 进度: %ld行, 字符=%d, 对=%d, 词典=%d, "
                   "节点=%ld\n",
                   tm->progress.current_round, line_no,
                   nchars, npairs, nwords,
                   tm->progress.total_added_nodes);
        }
    }

    // 处理剩余数据
    int final = article_flush(ar, NULL);
    if (final > 0) total_flushes++;

    // 统计最终结果
    int nchars, npairs, nwords;
    article_get_stats(ar, &nchars, &npairs, &nwords);
    printf("[文章阅读] 完成: %ld行, %d次刷新, 字符=%d, 对=%d, 词典=%d, "
           "新增节点=%ld\n",
           line_no, total_flushes, nchars, npairs, nwords,
           tm->progress.total_added_nodes);

    article_reader_destroy(ar);
    fclose(f);
    tm->progress.total_lines = line_no;
    return 0;
}

// ==================== 核心：喂一行到引擎 ====================

static int train_feed_one_line(TrainMode* tm, const char* text) {
    MasterTopology* m = tm->topology;
    if (!m) return 0;

    SubTopology* vocab = NULL;
    for (int t = 0; t < m->sub_topo_count; t++) {
        if (m->sub_topologies[t] && m->sub_topologies[t]->type == TOPO_VOCABULARY) {
            vocab = m->sub_topologies[t];
            break;
        }
    }
    if (!vocab || !vocab->net) return 0;

    char copy[4096];
    strncpy(copy, text, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = 0;

    char* tokens[TRAIN_MAX_TOKENS];
    int token_count = utf8_tokenize(copy, tokens, TRAIN_MAX_TOKENS);

    int added = train_feed_token_sequence(tm, vocab, tokens, token_count,
                                          0.4f, NULL, NULL);

    for (int i = 0; i < token_count; i++) free(tokens[i]);

    return added;
}

// ==================== 批量学习 & 存盘 ====================

/**
 * 根据丘脑认知状态调整限速延迟
 * 前额叶活跃（对话中） → 增大延迟，让出 CPU
 * 空闲时 → 减小延迟，快速学习
 */
static void train_throttled_delay(TrainMode* tm, long count) {
    if (tm->config.speed <= 0) return;
    if (count % tm->config.speed != 0) return;

    int base_us = 1000000; // 1 秒基准
    if (tm->thalamus) {
        // 前额叶 throttle 表示对话活跃度：高 → 认知繁忙，低 → 空闲
        float pfc = thalamus_get_throttle(tm->thalamus, THAL_PREFRONTAL);
        // 对海马体的 throttle：高 → 空闲可训练，低 → 应减慢
        float hip = thalamus_get_throttle(tm->thalamus, THAL_HIPPOCAMPUS);

        // 综合因子：前额叶活跃时 ≥1.5x 延迟，海马体受抑制时 ≥2x 延迟
        float pfc_factor = 0.5f + pfc * 1.5f;            // [0.5, 2.0]
        float hip_factor = 0.3f + (1.0f - hip) * 1.5f;   // [0.3, 1.8]
        float factor = pfc_factor * hip_factor;
        if (factor < 0.3f) factor = 0.3f;
        if (factor > 3.0f) factor = 3.0f;
        base_us = (int)(base_us * factor);
    }
    usleep(base_us);
}

static void train_do_batch_learn(TrainMode* tm) {
    if (tm->learner) {
        printf("[训练] 触发主动学习 (已喂%ld条)...\n", tm->progress.total_fed);
        learn_from_memory(tm->learner);
        tm->progress.total_learned++;

        // 通过丘脑报告学习工作量
        if (tm->thalamus) {
            thalamus_send_feedback(tm->thalamus, THAL_HIPPOCAMPUS,
                                   (int)tm->progress.total_learned, 0, 0);
        }
    }
}

static void train_do_auto_save(TrainMode* tm, const char* workdir) {
    (void)workdir;
    if (!tm->topology) return;
    printf("[训练] 强制存盘 (已喂%ld条)...\n", tm->progress.total_fed);
    int saved = master_save_state(tm->topology, "pivotmind_state.dat");
    if (saved > 0) printf("[训练]   已保存 %d 节点\n", saved);
}

// ==================== 辅助函数 ====================

static void train_set_error(TrainMode* tm, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tm->progress.error_msg, sizeof(tm->progress.error_msg), fmt, ap);
    va_end(ap);
    tm->progress.state = TRAIN_ERROR;
    printf("[训练] 错误: %s\n", tm->progress.error_msg);
}

static long train_count_lines(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    long count = 0;
    int ch;
    while ((ch = fgetc(f)) != EOF) {
        if (ch == '\n') count++;
    }
    fclose(f);
    return count;
}

// ==================== 格式检测 ====================

CorpusFormat train_detect_format(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return CORPUS_PLAIN_TEXT;

    char head[256];
    size_t n = fread(head, 1, sizeof(head) - 1, f);
    fclose(f);
    head[n] = 0;

    char* p = head;
    while (*p && isspace((unsigned char)*p)) p++;

    if (*p == '[') return CORPUS_JSON_QA;
    if (strchr(p, '|')) return CORPUS_PIPE_QA;

    return CORPUS_PLAIN_TEXT;
}

// ==================== 命令行解析 ====================

TrainConfig train_config_from_args(int argc, char* argv[]) {
    TrainConfig cfg = {
        .corpus_path = NULL,
        .format = CORPUS_JSON_QA,
        .rounds = 1,
        .speed = 20,
        .batch_learn_interval = 100,
        .save_interval = 5000,
        .verbose = 0,
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--corpus") == 0 && i + 1 < argc) {
            cfg.corpus_path = argv[++i];
        } else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            const char* fmt = argv[++i];
            if (strcmp(fmt, "json") == 0 || strcmp(fmt, "qa") == 0)
                cfg.format = CORPUS_JSON_QA;
            else if (strcmp(fmt, "pipe") == 0)
                cfg.format = CORPUS_PIPE_QA;
            else if (strcmp(fmt, "text") == 0 || strcmp(fmt, "plain") == 0)
                cfg.format = CORPUS_PLAIN_TEXT;
            else if (strcmp(fmt, "article") == 0)
                cfg.format = CORPUS_ARTICLE;
        } else if (strcmp(argv[i], "--rounds") == 0 && i + 1 < argc) {
            cfg.rounds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--speed") == 0 && i + 1 < argc) {
            cfg.speed = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--batch") == 0 && i + 1 < argc) {
            cfg.batch_learn_interval = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--save-interval") == 0 && i + 1 < argc) {
            cfg.save_interval = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            cfg.verbose = 1;
        }
    }

    // 自动检测格式
    if (cfg.corpus_path) {
        cfg.format = train_detect_format(cfg.corpus_path);
    }

    return cfg;
}

void train_config_print_defaults(void) {
    printf("训练模式配置:\n");
    printf("  --corpus PATH        语料文件路径\n");
    printf("  --format json|pipe|text|article  语料格式 (默认自动检测)\n");
    printf("  --rounds N           训练轮数 (默认1)\n");
    printf("  --speed N            每秒喂料条数 (默认20)\n");
    printf("  --batch N            每N条触发主动学习 (默认100)\n");
    printf("  --save-interval N    每N条自动存盘 (默认5000)\n");
    printf("  --verbose            详细输出\n");
}

void train_mode_set_thalamus(TrainMode* tm, Thalamus* th) {
    if (tm) tm->thalamus = th;
}
