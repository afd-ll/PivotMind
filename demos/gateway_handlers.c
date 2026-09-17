/**
 * @file gateway_handlers.c
 * @brief PivotMind HTTP Gateway — REST 请求处理 (handle_*)
 *
 * 由 demos/pivotmind_gateway.c 拆分而来（v0.5.25 P2-6）。
 * 共享类型与原型见 gateway_internal.h。
 */

#include "gateway_internal.h"
#include "lang.h"
#include "generation_feedback.h"   /* v0.6.3：GET /reach 读数 */

// ==================== 请求处理 ====================

/* ============================================================================
 * [BG-19] 喂料前剥离生成端「格式骨架」
 *
 * 病灶：/chat 会把 **AI 回复整段** 喂进学习路径（本文件 `learn_queue_push(response,…)`
 * 与同步回退分支的 `_learn_tokens(vocab, response,…)`），而回复里混着生成端的
 * 排版构件（字面量见 `src/prefrontal_executive.c:1108/1117/1137`）：
 *   · "为了回答这个问题，我分步进行了思考："
 *   · "N. <子问题> → <答案> (置信度: NN%)"
 *   · "综合以上分析："  · "（推理模式: …）"
 * ⇒ 这些构件被 PMI 词发现当词建进 vocab（线上实测：**1075 / 4606 节点含下划线**）。
 *
 * 处置：喂料前剥骨架、**保留语义文字**（子问题与答案都留）。
 * ⚠️ 只作用于「喂给学习的那份副本」，**不影响返回给用户的回复本身**。
 * 返回新分配的缓冲区，调用方负责 free；分配失败返回 NULL（调用方回退用原文）。
 * ============================================================================ */
static char* bg19_strip_scaffold(const char* src) {
    if (!src) return NULL;
    size_t n = strlen(src);
    char* out = (char*)malloc(n + 1);
    if (!out) return NULL;

    /* ① 整段丢弃的固定构件 */
    static const char* LEAD[] = {
        "为了回答这个问题，我分步进行了思考：",
        "综合以上分析：",
    };
    /* ② 变长构件前缀 → 一直吃到闭括号 */
    static const char* TAIL_OPEN[] = { "（推理模式", "(推理模式" };

    size_t w = 0;
    const char* p = src;

    while (*p) {
        int handled = 0;
        unsigned int cp = 0;
        int b = 0;

        for (size_t i = 0; i < sizeof(LEAD) / sizeof(LEAD[0]); i++) {
            size_t L = strlen(LEAD[i]);
            if (strncmp(p, LEAD[i], L) == 0) { p += L; handled = 1; break; }
        }
        if (handled) continue;

        for (size_t i = 0; i < sizeof(TAIL_OPEN) / sizeof(TAIL_OPEN[0]); i++) {
            size_t L = strlen(TAIL_OPEN[i]);
            if (strncmp(p, TAIL_OPEN[i], L) == 0) {
                p += L;
                while (*p && *p != ')' && *p != '\n') {
                    b = pm_utf8_decode(p, &cp);
                    if (b <= 0) b = 1;
                    p += b;
                }
                if (*p == ')') p++;
                handled = 1;
                break;
            }
        }
        if (handled) continue;

        /* ③ 箭头 "→"（U+2192，3 字节） */
        if ((unsigned char)p[0] == 0xE2 && (unsigned char)p[1] == 0x86 &&
            (unsigned char)p[2] == 0x92) {
            p += 3;
            continue;
        }

        /* ④ "(置信度: NN%)" / "（置信度: NN%）" 整段（含半/全角括号） */
        if (strncmp(p, "置信度", 9) == 0) {
            /* 向前吃掉已写入的开括号 */
            if (w >= 1 && out[w - 1] == '(') {
                w -= 1;
            } else if (w >= 3 && (unsigned char)out[w - 3] == 0xEF &&
                       (unsigned char)out[w - 2] == 0xBC &&
                       (unsigned char)out[w - 1] == 0x88) {   /* "（" = EF BC 88 */
                w -= 3;
            }
            if (p[9] == ':') p += 10; else p += 9;
            while (*p && *p != ')' && *p != '\n') {
                b = pm_utf8_decode(p, &cp);
                if (b <= 0) b = 1;
                p += b;
            }
            if (*p == ')') p++;
            continue;
        }

        out[w++] = *p++;
    }
    out[w] = '\0';
    return out;
}

// POST /chat - 对话
void handle_chat(GatewaySystem* gw, int fd, const char* body) {
    char msg[2048] = {0};
    if (!json_extract_string(body, "msg", msg, sizeof(msg)) || strlen(msg) == 0) {
        http_json(fd, 400, "{\"error\":\"missing or empty 'msg' field\"}");
        return;
    }

    /* ── v0.3 Phase 2: 复杂问题走 PFE 推理编排 ── */
    int use_pfe = 0;
    char* response = NULL;
    if (gw->qa_memory) { const char* qa = qa_memory_query(gw->qa_memory, msg); if (qa) { response = strdup(qa); } }

    /* v0.4.3: 对话中自动学习 — 将输入 token 注册到词汇拓扑
     * 这是端到端对话质量最大的瓶颈：不学习新词则扩散引擎 active_count=0
     * v0.5.20 B4: 改走学习队列（单 worker 消费）+ flush 等待，保当轮新词可用。 */
    {
        SubTopology* vocab = NULL;
        for (int t = 0; t < gw->topology->sub_topo_count; t++) {
            if (gw->topology->sub_topologies[t] &&
                gw->topology->sub_topologies[t]->type == TOPO_VOCABULARY)
                { vocab = gw->topology->sub_topologies[t]; break; }
        }
        if (vocab && vocab->net) {
#if LEARN_ASYNC_CHAT
            LearnTask* t = learn_queue_push(msg, "", 1);   /* 入队 + flush 等待 */
            if (t) {
                learn_task_wait(t);
            } else {
                /* C3 配套: 队列未初始化/已关闭时 push 返回 NULL。旧版此处直接
                 * 静默跳过，这一轮的学习（当轮新词可用性）就丢了。退化为同步
                 * 学习，语义与 LEARN_ASYNC_CHAT=0 分支一致。
                 * 安全性：本路径能跑起来说明连接线程尚未 join，即 gw_system_shutdown
                 * 还没开始拆拓扑（关闭顺序：先 join 连接线程，再 shutdown）。 */
                EmergentPOS* ep = (gw->prefrontal && gw->prefrontal->controller)
                                  ? gw->prefrontal->controller->emergent_pos : NULL;
                int prev_id = -1;
                int learned = _learn_tokens(vocab, msg, &prev_id, ep);
                if (learned > 0)
                    printf("[gateway] 对话中学习(同步降级): +%d 个新词\n", learned);
            }
#else
            EmergentPOS* ep = (gw->prefrontal && gw->prefrontal->controller)
                              ? gw->prefrontal->controller->emergent_pos : NULL;
            int prev_id = -1;
            int learned = _learn_tokens(vocab, msg, &prev_id, ep);
            if (learned > 0)
                printf("[gateway] 对话中学习: +%d 个新词\n", learned);
#endif
        }
    }

    if (gw->pfe) {
        int complexity = pfe_assess_complexity(gw->pfe, msg);
        /* v0.6 测试：PFE 门槛恢复 >0 验证（diffusion 降级路径已话题化，
         * PFE 的 answer_text 应随 diffusion 修复而修复） */
        if (complexity > 0) {
            /* 中高复杂度 → PFE 推理管线 */
            char pfe_answer[GW_MAX_RESPONSE];
            int pfe_ok = pfe_reason(gw->pfe, msg, pfe_answer, sizeof(pfe_answer));
            if (pfe_ok == 0 && strlen(pfe_answer) > 10) {
                response = strdup(pfe_answer);
                use_pfe   = 1;
                printf("[gateway] PFE推理完成 (复杂度=%d, 周期=%d, 满意度=%.2f)\n",
                       complexity, pfe_cycle_count(gw->pfe),
                       (double)pfe_avg_satisfaction(gw->pfe));
            }
        }
    }

    /* 回退到旧路径：简单问题或 PFE 失败
     * v0.6: 禁用上一轮回复注入——历史串扰会让词锚定命中旧回复的
     * 词（"衣服"回复拼进"历史"输入 → 输出"衣服历史"），话题性
     * 阶段输入必须是纯当前话题。等上下文机制重做后再启用。 */
    if (!response) {
        response = prefrontal_chat(gw->prefrontal, msg);
    }

    /* 最终兜底：QA 记忆检索（扩散和联想推理都无产出） */
    if (!response && gw->qa_memory) {
        const char* qa_answer = qa_memory_query(gw->qa_memory, msg);
        if (qa_answer) {
            response = strdup(qa_answer);
            printf("[gateway] QA记忆命中\n");
        }
    }

    /* 语言一致性兜底（v0.6）：中文输入不应返回纯英文回复。
     * PFE/graph 合成路径可能混入英文节点名，此处统一拦截，
     * 丢弃后走 prefrontal_chat / 默认回复路径。 */
    if (response) {
        /* v0.5.35：改走语种 SSOT。原实现按「有无非 ASCII 字节」判中文 ——
         * emoji / 假名 / 带音标的拉丁字母都会被算成中文。现按【存在中文码点】判。 */
        if (pm_has_zh(msg) && !pm_has_zh(response)) { free(response); response = NULL; }
    }

    /* 功能词兜底（v0.6）：回复若只有"很+X"类功能词组合（很大/很快）
     * 或纯标点/单字重复 → 替换为礼貌默认。扩散组装在无实义词时
     * 会选 ADJ 组合当主语，这是"很大。"泛滥的根源。 */
    if (response) {
        int cjk_cnt = 0, punct_cnt = 0;
        for (const char* p = response; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (pm_is_nonascii(p)) {
                /* CJK 标点按 UTF-8 序列识别（。、，！）；不能用多字节字符常量
                 * 与单字节 char 比较（恒为 false）。cjk_cnt 仍逐字节计数，
                 * 除以 3 得汉字数（含标点容差），与旧逻辑一致。 */
                if (strncmp(p, "。", 3) == 0 || strncmp(p, "、", 3) == 0 ||
                    strncmp(p, "，", 3) == 0 || strncmp(p, "！", 3) == 0)
                    punct_cnt++;
                cjk_cnt++;
            }
            else if (c == ' ' || c == '.' || c == ',')
                punct_cnt++;
        }
        int wordish = (cjk_cnt / 3);  /* 汉字数（含标点容差，如"很大。"=3） */
        int is_void = (wordish <= 3);                /* <=1 个实义词（后 5 个 strstr 子句均被本条件蕴含，已删） */
        if (is_void) { free(response); response = strdup("好的。"); }
    }

    if (response) {
        char escaped[GW_MAX_RESPONSE];
        json_escape(response, escaped, sizeof(escaped));

        int total_nodes = 0;
        for (int t = 0; t < gw->topology->sub_topo_count; t++) {
            if (gw->topology->sub_topologies[t] && gw->topology->sub_topologies[t]->net)
                total_nodes += gw->topology->sub_topologies[t]->net->node_count;
        }

        /* P1-3: total_dialogs 是 64 个连接线程共享的计数器，旧版"读出来 +1
         * 写进 JSON"再 `gw->total_dialogs++` 两处都非原子 → 统计丢失/重复。
         * 用 __sync 一次原子取号（对照 gateway_learn.c:231 的用法）。 */
        long dialog_no = (long)__sync_add_and_fetch(&gw->total_dialogs, 1);

        char json[GW_MAX_RESPONSE];
        snprintf(json, sizeof(json),
            "{\"reply\":\"%s\",\"nodes\":%d,\"dialogs\":%lld%s}",
            escaped, total_nodes, (long long)dialog_no,
            use_pfe ? ",\"reasoning\":\"pfe\"" : "");

        http_json(fd, 200, json);

        /* v0.4.3: 保存本轮回复到多轮对话上下文 */
        if (response && response[0]) {
            int alen = (int)strlen(response);
            if (alen > 1023) alen = 1023;
            memcpy(gw->last_answer, response, (size_t)alen);
            gw->last_answer[alen] = '\0';
            gw->dialog_context_ready = 1;
        }

        /* v0.4.3: AI回复也纳入词汇拓扑 — 双向在线学习
         * v0.5.20 B4: fire-and-forget 入队（无当轮依赖）。 */
        {
            SubTopology* vocab = NULL;
            for (int t = 0; t < gw->topology->sub_topo_count; t++) {
                if (gw->topology->sub_topologies[t] &&
                    gw->topology->sub_topologies[t]->type == TOPO_VOCABULARY)
                    { vocab = gw->topology->sub_topologies[t]; break; }
            }
            if (vocab && vocab->net && response) {
                /* [BG-19] 喂料前剥骨架：只把语义文字喂进学习路径，避免生成端
                 * 排版构件（引导句 / 箭头 / (置信度: NN%)）被当词建进 vocab。
                 * ⚠️ 只改「喂给学习的副本」，回复本身不受影响。 */
                char* feed = bg19_strip_scaffold(response);
                const char* feed_text = feed ? feed : response;
#if LEARN_ASYNC_CHAT
                learn_queue_push(feed_text, "", 0);   /* fire-and-forget，忽略返回值 */
#else
                EmergentPOS* ep = (gw->prefrontal && gw->prefrontal->controller)
                                  ? gw->prefrontal->controller->emergent_pos : NULL;
                int prev_id = -1;
                int learned = _learn_tokens(vocab, feed_text, &prev_id, ep);
                if (learned > 0)
                    printf("[gateway] 回复中学习: +%d 个新词\n", learned);
#endif
                free(feed);
            }
        }

        /* 海马体记下这次对话 — 巩固时自动建 QA 连接 */
        if (gw->hippocampus) hippocampus_log_dialog(gw->hippocampus, msg, response);

        /* ── v0.3 Phase 2: IdeaArena 胜者反馈回流 ── */
        if (use_pfe && gw->arena && gw->topology) {
            arena_feedback_to_master(gw->arena, gw->topology);
        }

        /* 模板构建已由 Broca 自主 tick 调度（brainstem → broca_tick），
         * 对话路径不再重复调用 */

        free(response);
    } else {
        http_json(fd, 200, "{\"reply\":\"(无回应)\",\"nodes\":0}");
    }
}

/* P1-3: 网关层共享可变状态的互斥——限流计数（last_learn_time/learn_burst）
 * 与 qa_memory 懒初始化都由 64 个连接线程并发触碰，旧版全无保护。
 * 文件级 static 即可：进程内只有一个 GatewaySystem（g_gw）。 */
static pthread_mutex_t g_gw_mutex = PTHREAD_MUTEX_INITIALIZER;

// POST /learn - 主动学习
void handle_learn(GatewaySystem* gw, int fd, const char* body) {
    char msg[2048] = {0};
    if (!json_extract_string(body, "msg", msg, sizeof(msg)) || strlen(msg) == 0) {
        http_json(fd, 400, "{\"error\":\"missing or empty 'msg' field\"}");
        return;
    }

    /* P1-3: 限流状态是 gw 上的共享可变字段，/learn 可被 64 个连接线程并发
     * 进入；旧版 "读 + 改 + 写" 无锁 → 计数丢失、限流形同虚设。
     * 整个判定与更新放在一把互斥内，判定结果在锁内算出、锁外应答。 */
    int over_limit = 0;
    pthread_mutex_lock(&g_gw_mutex);
    time_t now = time(NULL);
    if (now == gw->last_learn_time) {
        over_limit = (++gw->learn_burst > 500);
    } else {
        gw->last_learn_time = now;
        gw->learn_burst = 0;
    }
    pthread_mutex_unlock(&g_gw_mutex);
    if (over_limit) {
        http_json(fd, 429, "{\"error\":\"rate limit\"}");
        return;
    }

    /* v0.5.9: 异步学习——入队（满丢最旧），固定 worker 消费。
     * 不再每请求 spawn detached 线程（08-07 线程堆积雪崩实锤）。
     * v0.5.20 B4: 入队逻辑抽出为 learn_queue_push（handle_chat 复用）。 */
    char domain[64] = {0};
    json_extract_string(body, "domain", domain, sizeof(domain));
    LearnTask* task = learn_queue_push(msg, domain, 0);   /* fire-and-forget */
    if (!task) { http_json(fd, 500, "{\"error\":\"oom\"}"); return; }

    http_json(fd, 202, "{\"result\":\"accepted\"}");
}

// POST /feedback - 反馈
void handle_feedback(GatewaySystem* gw, int fd, const char* body) {
    char msg[2048] = {0};
    char rating[64] = {0};

    if (!json_extract_string(body, "msg", msg, sizeof(msg)) || strlen(msg) == 0) {
        http_json(fd, 400, "{\"error\":\"missing 'msg' field\"}");
        return;
    }
    if (!json_extract_string(body, "rating", rating, sizeof(rating)) || strlen(rating) == 0) {
        http_json(fd, 400, "{\"error\":\"missing 'rating' field (correct/wrong)\"}");
        return;
    }

    // 处理反馈
    float confidence = 0.5f;
    if (strcmp(rating, "correct") == 0 || strcmp(rating, "对") == 0) {
        confidence = 0.95f;
    } else if (strcmp(rating, "wrong") == 0 || strcmp(rating, "错") == 0) {
        confidence = 0.2f;
    } else {
        http_json(fd, 400, "{\"error\":\"rating must be 'correct' or 'wrong'\"}");
        return;
    }

    // 存入记忆
    char key[512];
    snprintf(key, sizeof(key), "feedback:%s", msg);
    memory_store(gw->memory, key, (void*)rating, strlen(rating) + 1, MEMORY_TYPE_STRING, confidence);

    http_json(fd, 200, "{\"result\":\"ok\"}");
}

// ==================== v0.5 多模态媒体投喂 API ====================

// POST /media/feed - 入队视频文件到视觉皮层任务队列
void handle_media_feed(GatewaySystem* gw, int fd, const char* body) {
    char path[1024] = {0};
    char mode[64] = "visual";  /* 默认: 视觉皮层对齐模式 */

    if (!json_extract_string(body, "path", path, sizeof(path)) || strlen(path) == 0) {
        http_json(fd, 400, "{\"error\":\"missing 'path' field\"}");
        return;
    }
    json_extract_string(body, "mode", mode, sizeof(mode));

    /* v0.5.1: 诊断模式 — 列出视频的所有轨道信息 */
    if (strcmp(mode, "diagnose") == 0) {
        if (!gw->visual_cortex) {
            http_json(fd, 500, "{\"error\":\"visual cortex not initialized\"}");
            return;
        }
        MediaReader* mr = visual_cortex_get_media_reader(gw->visual_cortex);
        if (!mr) {
            http_json(fd, 500, "{\"error\":\"media reader not available\"}");
            return;
        }
        int tracks = media_diagnose_tracks(mr, path);
        char resp[512];
        snprintf(resp, sizeof(resp),
                 "{\"result\":\"%s\",\"tracks\":%d,\"has_subtitle\":%s}",
                 (tracks > 0) ? "ok" : "no_tracks",
                 tracks,
                 (tracks > 0) ? "check_server_stdout" : "none");
        http_json(fd, 200, resp);
        return;
    }

    if (!gw->visual_cortex) {
        http_json(fd, 500, "{\"error\":\"visual cortex not initialized\"}");
        return;
    }

    int enqueued = 0;

    /* 检查是否为目录 */
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        char recursive[8] = "0";
        int rec = 0;
        if (json_extract_string(body, "recursive", recursive, sizeof(recursive))) {
            rec = (strcmp(recursive, "1") == 0 || strcmp(recursive, "true") == 0) ? 1 : 0;
        }
        enqueued = visual_cortex_enqueue_directory(gw->visual_cortex, path, NULL, rec);
    } else {
        int ret = visual_cortex_enqueue(gw->visual_cortex, path, mode);
        if (ret == 0) enqueued = 1;
    }

    int queue_size = visual_cortex_queue_size(gw->visual_cortex);

    char resp[512];
    snprintf(resp, sizeof(resp),
             "{\"result\":\"enqueued\",\"mode\":\"%s\",\"enqueued\":%d,\"queue_size\":%d}",
             mode, enqueued, queue_size);
    http_json(fd, (enqueued > 0) ? 200 : 500, resp);
}

// GET /media/status - 查询多模态管道状态
void handle_media_status(GatewaySystem* gw, int fd) {
    char resp[1024];

    int qsize = 0;
    long vc_frames = 0;
    int vc_vis = 0, vc_xmod = 0;
    if (gw->visual_cortex)
        visual_cortex_get_stats(gw->visual_cortex, &qsize, &vc_frames, &vc_vis, &vc_xmod);

    /* 内部 MediaReader 统计 */
    long mr_files = 0, mr_lines = 0, mr_words = 0;
    if (gw->visual_cortex) {
        MediaReader* mr = visual_cortex_get_media_reader(gw->visual_cortex);
        if (mr) media_reader_get_stats(mr, &mr_files, &mr_lines, &mr_words);
    }

    /* 视觉拓扑统计 */
    int vis_topo_nodes = 0;
    SubTopology* vt = master_get_sub_topology_by_type(gw->topology, TOPO_VISUAL);
    if (vt && vt->net) vis_topo_nodes = vt->net->node_count;

    snprintf(resp, sizeof(resp),
             "{\"queue_size\":%d,"
             "\"media_reader\":{\"files\":%ld,\"lines\":%ld,\"words\":%ld},"
             "\"visual_cortex\":{\"frames\":%ld,\"visual_nodes\":%d,\"cross_modal_edges\":%d,\"topo_visual_nodes\":%d}}",
             qsize,
             mr_files, mr_lines, mr_words,
             vc_frames, vc_vis, vc_xmod, vis_topo_nodes);
    http_json(fd, 200, resp);
}

// GET /status - 状态查询
void handle_status(GatewaySystem* gw, int fd) {
    int total_nodes = 0;
    int template_nodes = 0;
    for (int t = 0; t < gw->topology->sub_topo_count; t++) {
        if (gw->topology->sub_topologies[t] && gw->topology->sub_topologies[t]->net)
            total_nodes += gw->topology->sub_topologies[t]->net->node_count;
    }
    SubTopology* tpl = master_get_sub_topology_by_type(gw->topology, TOPO_TEMPLATE);
    if (tpl && tpl->net) template_nodes = tpl->net->node_count;

    long long uptime = (long long)(time(NULL) - gw->start_time);
    char real_time_buf[32];
    const char* real_time = "unknown";
    if (gw->brainstem) {
        real_time = brainstem_get_real_time(gw->brainstem, real_time_buf, sizeof(real_time_buf));
    }
    int clock_ticks = gw->brainstem ? brainstem_tick_count(gw->brainstem) : 0;
    float circadian = gw->brainstem ? brainstem_get_circadian(gw->brainstem) : 0.5f;
    const char* circadian_phase = gw->brainstem ? brainstem_get_circadian_phase(gw->brainstem) : "unknown";
    long cache_frozen = gw->brain_cache ? gw->brain_cache->total_freezes : 0;
    long cache_thawed = gw->brain_cache ? gw->brain_cache->total_thaws : 0;

    char json[2048];
    snprintf(json, sizeof(json),
        "{"
        "\"status\":\"running\","
        "\"real_time\":\"%s\","
        "\"uptime\":%lld,"
        "\"clock_ticks\":%d,"
        "\"circadian\":%.2f,"
        "\"circadian_phase\":\"%s\","
        "\"dialogs\":%lld,"
        "\"learn_calls\":%lld,"
        "\"total_nodes\":%d,"
        "\"template_nodes\":%d,"
        "\"template_voting\":%s,"
        "\"brain_frozen\":%ld,"
        "\"brain_thawed\":%ld,"
        "\"topologies\":%d,"
        "\"port\":%d,"
        "\"version\":\"%s\""
        "}",
        real_time ? real_time : "unknown",
        uptime,
        clock_ticks,
        (double)circadian,
        circadian_phase,
        (long long)gw->total_dialogs,
        (long long)gw->total_learning_cycles,
        total_nodes,
        template_nodes,
        gw->topology->use_template_voting ? "true" : "false",
        cache_frozen,
        cache_thawed,
        gw->topology->sub_topo_count,
        gw->port, PIVOTMIND_VERSION);

    http_json(fd, 200, json);
}

// GET / - 仪表盘首页
void handle_root(GatewaySystem* gw, int fd) {
    (void)gw;
    const char* html =
        "<!DOCTYPE html><html lang=zh-CN><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\"><title>玄枢 PivotMind</title><style>*{margin:0;padding:0;box-sizing:border-box}"
        "body{background:#0c1220;color:#cbd5e1;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI','PingFang SC','Microsoft YaHei',sans-serif;padding:20px;min-height:100vh}"
        "h1{font-size:20px;font-weight:400;color:#48dbfb;letter-spacing:3px;margin-bottom:2px}"
        ".sub{color:#475569;font-size:12px;margin-bottom:28px;letter-spacing:1px}"
        ".gw{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;margin-bottom:16px}"
        ".rw{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:16px}"
        ".cd{background:#131d2e;border:1px solid #1e2d45;border-radius:8px;padding:14px}"
        ".lb{font-size:10px;color:#475569;margin-bottom:5px;letter-spacing:1px;text-transform:uppercase}"
        ".vl{font-size:22px;font-weight:600;color:#e2e8f0}"
        ".gr{color:#22c55e}.cy{color:#22d3ee}.yw{color:#eab308}.bl{color:#60a5fa}"
        ".dt{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:5px;background:#22c55e}"
        ".tg{display:inline-block;padding:2px 10px;border-radius:14px;font-size:11px;background:#1e2d45;color:#48dbfb;border:1px solid #2a3f5a}"
        ".br{height:3px;border-radius:2px;background:#1e2d45;margin:8px 0 5px;overflow:hidden}"
        ".fl{height:100%;border-radius:2px;background:#48dbfb;transition:width .8s}"
        ".sg{display:grid;grid-template-columns:1fr 1fr;gap:5px;margin-top:5px}"
        ".si{text-align:center;padding:5px;background:rgba(0,0,0,.25);border-radius:5px}"
        ".sn{font-size:16px;font-weight:600}"
        ".sl{font-size:9px;color:#475569;margin-top:2px;letter-spacing:.5px}"
        ".er{color:#ef4444;font-size:11px;text-align:center;margin-top:12px;word-break:break-all}"
        "@media(max-width:640px){body{padding:10px}.rw{grid-template-columns:1fr}}</style></head><body>"
        "<h1>玄枢</h1><div class=sub>PivotMind v" PIVOTMIND_VERSION "</div>"
        "<div class=gw id=ca></div>"
        "<div class=rw>"
        "<div class=cd><div class=lb>学习调度器</div><div id=s><div class=cy vl>加载中...</div></div></div>"
        "<div class=cd><div class=lb>训练模式</div><div id=t><div class=cy vl>未激活</div></div></div>"
        "</div>"
        "<div class=rw>"
        "<div class=cd><div class=lb>脑区索引</div><div id=b><div class=cy vl>加载中...</div></div></div>"
        "<div class=cd><div class=lb>知识拓扑</div><div id=p><div class=cy vl>加载中...</div></div></div>"
        "</div>"
        "<div id=er class=er></div>"
        "<script>"
        "var er=document.getElementById('er');"
        "function $(i,h){var e=document.getElementById(i);if(e)e.innerHTML=h}"
        "function L(){"
        "fetch('/status').then(function(r){return r.json()}).then(function(s){"
        "$('ca','<div class=cd><div class=lb>状态</div><div class=vl><span class=dt></span>'+s.status+'</div></div>'"
        "+'<div class=cd><div class=lb>运行</div><div class=vl>'+Math.floor(s.uptime/3600)+'h '+Math.floor(s.uptime%3600/60)+'m</div></div>'"
        "+'<div class=cd><div class=lb>节点</div><div class=\"vl gr\">'+s.total_nodes.toLocaleString()+'</div></div>'"
        "+'<div class=cd><div class=lb>版本</div><div class=\"vl bl\">'+s.version+'</div></div>');"
        "$('p','<div class=sg>'"
        "+'<div class=si><div class=\"sn gr\">'+s.template_nodes+'</div><div class=sl>模板</div></div>'"
        "+'<div class=si><div class=\"sn bl\">'+s.topologies+'</div><div class=sl>拓扑层</div></div>'"
        "+'<div class=si><div class=\"sn cy\">'+s.brain_frozen+'</div><div class=sl>冷冻</div></div>'"
        "+'<div class=si><div class=\"sn yw\">'+s.brain_thawed+'</div><div class=sl>解冻</div></div></div>');"
        "$('er','');"
        "}).catch(function(){});"
        "fetch('/scheduler').then(function(r){return r.json()}).then(function(c){"
        "var bw=c.self_learn_mods>0?100:10;"
        "$('s','<span class=tg>'+(c.phase||'-')+'</span> <span style=font-size:11px;color:#475569>'+c.phase_elapsed_s+'s</span>'"
        "+'<div class=br><div class=fl style=width:'+bw+'%></div></div>'"
        "+'<div class=sg>'"
        "+'<div class=si><div class=\"sn gr\">'+(c.total_loops||0)+'</div><div class=sl>闭环</div></div>'"
        "+'<div class=si><div class=\"sn cy\">'+(c.self_learn_cycles||0)+'</div><div class=sl>周期</div></div>'"
        "+'<div class=si><div class=\"sn yw\">'+(c.self_learn_mods||0)+'</div><div class=sl>修正</div></div>'"
        "+'<div class=si><div class=\"sn bl\">'+(c.eval_freeze_candidates||0)+'</div><div class=sl>候选</div></div></div>');"
        "}).catch(function(){});"
        "fetch('/train/status').then(function(r){return r.json()}).then(function(t){"
        "if(t.state==='idle'||t.state==='completed'){"
        "$('t','<span class=tg>'+(t.state||'idle')+'</span> <span style=color:#475569;font-size:12px>已喂 '+(t.total_fed||0)+' 条</span>');"
        "}else{"
        "var pct=t.total_lines>0?Math.min(100,(t.current_line/t.total_lines*100)):0;"
        "$('t','<span class=tg>'+(t.state||'?')+'</span> <span style=font-size:11px;color:#475569>第'+(t.current_round||0)+'/'+(t.total_rounds||1)+'轮</span>'"
        "+'<div class=br><div class=fl style=width:'+pct+'%></div></div>'"
        "+'<div style=font-size:18px;font-weight:600;color:#22c55e;margin:4px 0>'+pct+'%</div>'"
        "+'<div class=sg>'"
        "+'<div class=si><div class=\"sn gr\">'+(t.total_added_nodes||0)+'</div><div class=sl>新节点</div></div>'"
        "+'<div class=si><div class=\"sn yw\">'+(t.total_added_edges||0)+'</div><div class=sl>新边</div></div></div>');"
        "}}).catch(function(){});"
        "fetch('/brain').then(function(r){return r.json()}).then(function(b){"
        "if(b.error){$('b','<span style=color:#475569;font-size:13px>未激活</span>');}else{"
        "$('b','<div class=sg>'"
        "+'<div class=si><div class=\"sn gr\">'+(b.entries||0)+'</div><div class=sl>已分类</div></div>'"
        "+'<div class=si><div class=\"sn cy\">'+(b.updates||0)+'</div><div class=sl>EMA</div></div>'"
        "+'<div class=si><div class=\"sn yw\">'+(b.migrations||0)+'</div><div class=sl>迁移</div></div>'"
        "+'<div class=si><div class=\"sn bl\">9+1</div><div class=sl>脑区</div></div></div>');"
        "}}).catch(function(){});"
        "setTimeout(L,5000)}"
        "L()"
        "</script></body></html>"
    ;
    http_send(fd, 200, "text/html; charset=utf-8", html);
}

// GET /scheduler - 学习调度器状态
void handle_scheduler(GatewaySystem* gw, int fd) {
    if (!gw->scheduler) {
        http_json(fd, 404, "{\"error\":\"scheduler not initialized\"}");
        return;
    }

    const char* phase_name = "idle";
    int total_loops = 0;
    int sel_cycles = 0, sel_mods = 0;
    int batch_nodes = 0, batch_edges = 0;
    int eval_candidates = 0;
    long phase_elapsed = 0;

    learning_scheduler_get_stats(gw->scheduler,
        &total_loops, &sel_cycles, &sel_mods,
        &batch_nodes, &batch_edges, &eval_candidates,
        &phase_name, &phase_elapsed);

    char json[1024];
    snprintf(json, sizeof(json),
        "{"
        "\"phase\":\"%s\","
        "\"phase_elapsed_s\":%ld,"
        "\"total_loops\":%d,"
        "\"self_learn_cycles\":%d,\"self_learn_mods\":%d,"
        "\"batch_nodes\":%d,\"batch_edges\":%d,"
        "\"eval_freeze_candidates\":%d"
        "}",
        phase_name, phase_elapsed,
        total_loops, sel_cycles, sel_mods,
        batch_nodes, batch_edges, eval_candidates);

    http_json(fd, 200, json);
}

// GET /scheduler/stats - 自学习器详细统计
void handle_scheduler_self_stats(GatewaySystem* gw, int fd) {
    if (!gw->scheduler) {
        http_json(fd, 404, "{\"error\":\"scheduler not initialized\"}");
        return;
    }

    LearningPhase phase = learning_scheduler_get_phase(gw->scheduler);
    (void)phase;

    // 从 self_learner 获取统计
    // （通过 scheduler_get_stats 已提供主要数据，此处为兼容更详细的未来扩展）
    http_json(fd, 200, "{\"detail\":\"use /scheduler for summary\"}");
}

// GET /health - 健康检查
void handle_health(GatewaySystem* gw, int fd) {
    if (gw->engine_ready) {
        http_json(fd, 200, "{\"status\":\"ok\"}");
    } else {
        http_json(fd, 503, "{\"status\":\"loading\",\"message\":\"engine initializing\"}");
    }
}

/* GET /reach - 生成端在线反馈 / 内调节 读数（v0.6.3）。
 * 机制未启用（PIVOTMIND_NO_FEEDBACK=1）或未注入 ⇒ 503。
 * ⚠️ 纯诊断读数，不加锁：可能与写者并发，读到的是瞬值（不做一致性保证）。 */
void handle_reach(GatewaySystem* gw, int fd) {
    const GenerationFeedback* fb =
        (gw && gw->topology && gw->topology->feedback_ptr)
            ? (const GenerationFeedback*)gw->topology->feedback_ptr : NULL;
    if (!fb) {
        http_json(fd, 503, "{\"error\":\"feedback not available\"}");
        return;
    }
    char json[512];
    snprintf(json, sizeof(json),
        "{"
        "\"enabled\":%d,"
        "\"samples\":%lu,"
        "\"reach_edges\":%d,"
        "\"reach_words\":%d,"
        "\"reach\":%.4f,"
        "\"reach_mu\":%.4f,"
        "\"reach_sigma\":%.4f,"
        "\"regulation\":%.4f"
        "}",
        fb->enabled,
        fb->samples,
        fb->reach_edges,
        fb->reach_words,
        (double)fb->reach,
        (double)fb->reach_mu,
        (double)fb->reach_sigma,
        (double)fb->regulation);
    http_json(fd, 200, json);
}

void handle_qa(GatewaySystem* gw, int fd, const char* body) {
    /* P1-3: 旧版无锁懒初始化 —— 两个并发 /qa 会各建一个 qa_memory，一个指针
     * 被覆盖并永久泄漏（qa_memory_destroy 只释放后一个）。整个"检查+创建+
     * 取用"放进互斥；本轮请求一律用本地快照 qam，不再二次解引用 gw->qa_memory。 */
    pthread_mutex_lock(&g_gw_mutex);
    if (!gw->qa_memory) gw->qa_memory = qa_memory_create(NULL, 500000);
    QAMemory* qam = gw->qa_memory;
    pthread_mutex_unlock(&g_gw_mutex);
    if (!qam) { http_json(fd, 500, "{\"error\":\"qa failed\"}"); return; }
    int added = 0; const char* p = body;
    while (p && *p) {
        const char* qk = strstr(p, "\"q\""); if (!qk) break;
        const char* qv = strchr(qk+3,':'); if(!qv)break; qv=strchr(qv,'"'); if(!qv)break; qv++;
        const char* qe = strchr(qv,'"'); if(!qe)break;
        const char* ak = strstr(qe,"\"a\""); if(!ak)break;
        const char* av = strchr(ak+3,':'); if(!av)break; av=strchr(av,'"'); if(!av)break; av++;
        const char* ae = strchr(av,'"'); if(!ae)break;
        char q[1024]={0},a[1024]={0}; int ql=qe-qv,al=ae-av;
        if(ql>0&&ql<1024&&al>0&&al<1024){memcpy(q,qv,ql);memcpy(a,av,al); if(qa_memory_add(qam,q,a)==0)added++;}
        p=ae+1;
    }
    char rs[128]; snprintf(rs,sizeof(rs),"{\"result\":\"ok\",\"added\":%d,\"total\":%d}",added,qa_memory_count(qam));
    http_json(fd,200,rs);
}
