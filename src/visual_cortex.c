/**
 * @file visual_cortex.c
 * @brief 视觉皮层脑区实现 — 多模态感知 + 跨模态对齐
 *
 * 脑区生命周期:
 *   visual_cortex_create()         → 注册到丘脑(外部调用)
 *   visual_cortex_enqueue()        → 网关入队任务
 *   visual_cortex_tick(throttle)   → 脑干周期调度
 *   visual_cortex_destroy()        → 清理
 *
 * 任务队列模式:
 *   生产者: 网关 POST /media/feed
 *   消费者: 脑干 tick 循环
 *   背压: max_batch_per_tick=1 (每次只处理一个文件)
 */

#include "visual_cortex.h"
#include "thalamus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define mkdir(p,m) _mkdir(p)
#ifndef S_ISDIR
#define S_ISDIR(m) ((m) & _S_IFREG)
#endif
#else
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

/* ==================== 内部常量 ==================== */

#define VC_MAX_FRAMES_PER_VIDEO    400
#define VC_MAX_TOKENS_PER_VIDEO    2000
#define VC_MAX_CROSS_PAIRS         512
#define VC_TASK_QUEUE_SIZE         128     /* 任务队列最大容量 */

/* ==================== 内部数据结构 ==================== */

typedef struct {
    float start_ms;
    float end_ms;
    char  text[512];
} SubtitleEntry;

typedef struct {
    char  word[128];
    float timestamp_ms;
    int   vocab_node_id;
} TimedToken;

/**
 * 任务队列条目
 */
typedef struct {
    char filepath[1024];
    char mode[32];   /* "subtitle" | "visual" */
} VCTask;

struct VisualCortex {
    MasterTopology*        topology;
    SubTopology*           vocab_topo;
    SubTopology*           visual_topo;
    VisualCortexConfig     cfg;

    /* ── 内部工具 ── */
    MediaReader*           media_reader;

    /* ── 任务队列 ── */
    VCTask*                tasks;
    int                    task_head;         /* 取任务位置 */
    int                    task_tail;         /* 放任务位置 */
    int                    task_count;
    int                    task_capacity;
    pthread_mutex_t        task_lock;

    /* ── 当前批次帧存储 ── */
    VisualFrame*           frames;
    int                    frame_count;
    int                    frame_capacity;

    /* ── 丘脑 ── */
    Thalamus*              thalamus;

    /* ── 编码器 ── */
    VisualFeatureEncoder   encoder_fn;
    void*                  encoder_ctx;

    /* ── 调度状态 ── */
    int                    idle_cooldown;      /* 队列空时的冷却计数 */

    /* ── 统计 ── */
    long  total_frames_processed;
    long  total_files_processed;
    int   total_visual_nodes;
    int   total_cross_modal_edges;
};

/* ==================== 任务队列 (环形缓冲区) ==================== */

static int vc_queue_push(VisualCortex* vc, const char* filepath, const char* mode) {
    pthread_mutex_lock(&vc->task_lock);

    if (vc->task_count >= vc->task_capacity) {
        pthread_mutex_unlock(&vc->task_lock);
        if (vc->cfg.verbose) fprintf(stderr, "[visual] 任务队列满 (%d)\n", vc->task_capacity);
        return -1;
    }

    VCTask* t = &vc->tasks[vc->task_tail];
    strncpy(t->filepath, filepath, sizeof(t->filepath) - 1);
    strncpy(t->mode, mode, sizeof(t->mode) - 1);
    vc->task_tail = (vc->task_tail + 1) % vc->task_capacity;
    vc->task_count++;

    pthread_mutex_unlock(&vc->task_lock);
    return 0;
}

static int vc_queue_pop(VisualCortex* vc, VCTask* out) {
    pthread_mutex_lock(&vc->task_lock);

    if (vc->task_count <= 0) {
        pthread_mutex_unlock(&vc->task_lock);
        return -1;
    }

    *out = vc->tasks[vc->task_head];
    vc->task_head = (vc->task_head + 1) % vc->task_capacity;
    vc->task_count--;

    pthread_mutex_unlock(&vc->task_lock);
    return 0;
}

/* ==================== 帧提取 ==================== */

static int frame_diff_score(const float* prev, const float* cur, int dim) {
    if (!prev || !cur) return 0;
    float sum_sq = 0.0f;
    for (int i = 0; i < dim && i < 64; i++) {
        float d = prev[i] - cur[i];
        sum_sq += d * d;
    }
    return (int)(sqrtf(sum_sq) * 100.0f);
}

/* v0.5.1: 修正 — 使用 ffprobe 而非 ffmpeg 进行帧元数据提取 */
static const char* vc_get_ffprobe_path(VisualCortex* vc) {
    if (vc->cfg.ffprobe_path[0] != '\0')
        return vc->cfg.ffprobe_path;
    static char derived[256];
    strncpy(derived, vc->cfg.ffmpeg_path, sizeof(derived) - 1);
    derived[sizeof(derived) - 1] = '\0';
    char* needle = strstr(derived, "ffmpeg");
    if (needle) memcpy(needle, "ffprobe", 7);
    else strncpy(derived, "ffprobe", sizeof(derived) - 1);
    return derived;
}

static int vc_extract_frames(VisualCortex* vc, const char* filepath) {
    const char* probe = vc_get_ffprobe_path(vc);
    char cmd[1536];
    int interval_secs = vc->cfg.frame_interval_ms / 1000;
    if (interval_secs < 1) interval_secs = 1;

    snprintf(cmd, sizeof(cmd),
             "\"%s\" -v error -select_streams v:0 "
             "-skip_frame nokey "
             "-show_entries frame=pkt_pts_time,pict_type,width,height "
             "-of csv=p=0 "
             "\"%s\"",
             probe, filepath);

#ifdef _WIN32
    FILE* fp = _popen(cmd, "r");
#else
    FILE* fp = popen(cmd, "r");
#endif
    if (!fp) return -1;

    int extracted = 0;
    float prev_pts = -999.0f;

    char line[512];
    while (fgets(line, sizeof(line), fp) && extracted < vc->frame_capacity) {
        float pts;
        char pict_type[16];
        if (sscanf(line, "%f,%15[^,]", &pts, pict_type) < 2) continue;
        if (prev_pts >= 0 && (pts - prev_pts) < (float)interval_secs) continue;

        VisualFrame* vf = &vc->frames[extracted];
        memset(vf, 0, sizeof(VisualFrame));
        vf->frame_index   = extracted;
        vf->timestamp_ms  = pts * 1000.0f;
        vf->is_keyframe   = (strchr(pict_type, 'I') != NULL) ? 1 : 0;
        vf->feature_dim   = vc->cfg.feature_dim;

        if (vc->cfg.feature_dim > 0) {
            vf->features = (float*)calloc((size_t)vc->cfg.feature_dim, sizeof(float));
        }

        snprintf(vf->frame_file, sizeof(vf->frame_file),
                 "%s/frame_%05d.png", vc->cfg.frame_output_dir, extracted);

        prev_pts = pts;
        extracted++;

        if (vc->cfg.max_frames_per_video > 0 && extracted >= vc->cfg.max_frames_per_video)
            break;
    }
#ifdef _WIN32
    _pclose(fp);
#else
    pclose(fp);
#endif

    /* 场景切换后处理 */
    if (vc->cfg.scene_threshold > 0 && extracted > 1) {
        for (int i = 1; i < extracted; i++) {
            if (frame_diff_score(vc->frames[i-1].features,
                                vc->frames[i].features,
                                vc->cfg.feature_dim) >= vc->cfg.scene_threshold) {
                vc->frames[i].scene_change = 1;
            }
        }
    }

    return extracted;
}

/* ==================== 字幕提取 ==================== */

static int vc_parse_ts(const char* buf, float* start_ms, float* end_ms) {
    int h1, m1, s1, ms1, h2, m2, s2, ms2;
    if (sscanf(buf, "%d:%d:%d,%d --> %d:%d:%d,%d",
               &h1, &m1, &s1, &ms1, &h2, &m2, &s2, &ms2) == 8) {
        *start_ms = (float)(h1 * 3600000 + m1 * 60000 + s1 * 1000 + ms1);
        *end_ms   = (float)(h2 * 3600000 + m2 * 60000 + s2 * 1000 + ms2);
        return 1;
    }
    return 0;
}

static int vc_is_index_line(const char* line) {
    if (!line || !*line) return 0;
    for (const char* p = line; *p; p++)
        if (!isdigit((unsigned char)*p)) return 0;
    return 1;
}

static int vc_extract_subtitles(VisualCortex* vc, const char* filepath,
                                SubtitleEntry* entries, int max_entries) {
    char cmd[1536];
    char tmpfile[512];
    int count = 0;

#ifdef _WIN32
    snprintf(tmpfile, sizeof(tmpfile), "pm_vc_subs_%d.srt", (int)GetCurrentProcessId());
#else
    snprintf(tmpfile, sizeof(tmpfile), "/tmp/pm_vc_subs_%d.srt", (int)getpid());
#endif

    snprintf(cmd, sizeof(cmd),
             "\"%s\" -y -v quiet -i \"%s\" -map 0:s:0 -c:s srt \"%s\"",
             vc->cfg.ffmpeg_path, filepath, tmpfile);

    if (system(cmd) != 0) { remove(tmpfile); return -1; }

    FILE* fp = fopen(tmpfile, "r");
    remove(tmpfile);
    if (!fp) return -1;

    char line[1024];
    int in_entry = 0, expect_text = 0;

    while (fgets(line, sizeof(line), fp) && count < max_entries) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n')) line[--len] = '\0';
        if (len == 0) { in_entry = 0; continue; }

        if (!in_entry) {
            if (vc_is_index_line(line)) { in_entry = 1; expect_text = 0; continue; }
            float s, e;
            if (vc_parse_ts(line, &s, &e)) {
                entries[count].start_ms = s;
                entries[count].end_ms   = e;
                entries[count].text[0]  = '\0';
                expect_text = 1;
                in_entry = 1;
            }
        } else if (expect_text) {
            char clean[512]; int ci = 0, in_tag = 0;
            for (size_t i = 0; i < len && ci < (int)sizeof(clean) - 1; i++) {
                if (line[i] == '<') { in_tag = 1; continue; }
                if (line[i] == '>') { in_tag = 0; continue; }
                if (!in_tag) clean[ci++] = line[i];
            }
            clean[ci] = '\0';
            if (ci == 0 || clean[0] == '♪' || clean[0] == '♫') continue;
            if (clean[0] == '[' && strchr(clean, ']')) continue;
            strncpy(entries[count].text, clean, sizeof(entries[count].text) - 1);
            count++; expect_text = 0;
        }
    }
    fclose(fp);
    return count;
}

/* ==================== 分词 ==================== */

static int vc_tokenize(const SubtitleEntry* entries, int entry_count,
                       TimedToken* tokens, int max_tokens) {
    int tok_count = 0;
    for (int e = 0; e < entry_count && tok_count < max_tokens; e++) {
        float mid_ts = (entries[e].start_ms + entries[e].end_ms) * 0.5f;
        const char* p = entries[e].text;
        while (*p) {
            while (*p && (isspace((unsigned char)*p) || ispunct((unsigned char)*p))) p++;
            if (!*p) break;
            const char* start = p;
            if (isalnum((unsigned char)*p)) {
                while (*p && isalnum((unsigned char)*p)) p++;
            } else if ((unsigned char)*p >= 0x80) {
                int nb = 1;
                unsigned char c = (unsigned char)*p;
                if ((c & 0xE0) == 0xC0)      nb = 2;
                else if ((c & 0xF0) == 0xE0) nb = 3;
                else if ((c & 0xF8) == 0xF0) nb = 4;
                p += nb;
            } else { p++; }
            size_t tl = (size_t)(p - start);
            if (tl >= sizeof(tokens[tok_count].word)) tl = sizeof(tokens[tok_count].word) - 1;
            memcpy(tokens[tok_count].word, start, tl);
            tokens[tok_count].word[tl] = '\0';
            tokens[tok_count].timestamp_ms = mid_ts;
            tokens[tok_count].vocab_node_id = -1;
            tok_count++;
            if (tok_count >= max_tokens) break;
        }
    }
    return tok_count;
}

/* ==================== 视觉拓扑 ==================== */

static SubTopology* vc_ensure_visual_topo(VisualCortex* vc) {
    if (vc->visual_topo) return vc->visual_topo;

    SubTopology* vt = master_get_sub_topology_by_type(vc->topology, TOPO_VISUAL);
    if (!vt) {
        int topo_id = master_add_sub_topology(vc->topology, TOPO_VISUAL,
                                              "视觉拓扑",
                                              vc->cfg.visual_topo_capacity,
                                              6);
        if (topo_id < 0) {
            fprintf(stderr, "[visual] TOPO_VISUAL 创建失败\n");
            return NULL;
        }
        vt = master_get_sub_topology(vc->topology, topo_id);
    }
    vc->visual_topo = vt;
    return vt;
}

static SubTopology* vc_get_vocab_topo(VisualCortex* vc) {
    if (!vc->vocab_topo)
        vc->vocab_topo = master_get_sub_topology_by_type(vc->topology, TOPO_VOCABULARY);
    return vc->vocab_topo;
}

static int vc_find_or_create_visual_node(VisualCortex* vc, const char* word) {
    SubTopology* vt = vc_ensure_visual_topo(vc);
    if (!vt || !vt->net) return -1;

    char vis_name[256];
    snprintf(vis_name, sizeof(vis_name), "%s_视觉", word);

    int node_id = huarong_net_find_concept(vt->net, vis_name);
    if (node_id >= 0) return node_id;

    NodeHashTable* hash = vt->node_hash;
    ReasoningNode* node = huarong_net_find_or_create_node(vt->net, vis_name, NULL, 0, hash);
    if (!node) return -1;

    if (!node->features && vc->cfg.feature_dim > 0) {
        node->features = (float*)calloc((size_t)vc->cfg.feature_dim, sizeof(float));
        if (node->features) node->feature_dim = vc->cfg.feature_dim;
    }
    node->node_type = NODE_TYPE_VISUAL;
    vc->total_visual_nodes++;
    return node->node_id;
}

/* ==================== 跨模态对齐 ==================== */

static int vc_perform_alignment(VisualCortex* vc,
                                VisualFrame* frames, int frame_count,
                                TimedToken* tokens, int token_count) {
    SubTopology* vocab  = vc_get_vocab_topo(vc);
    SubTopology* visual = vc_ensure_visual_topo(vc);
    if (!vocab || !visual || frame_count <= 0) return 0;

    float window_ms = vc->cfg.alignment_window_ms;
    int edges_created = 0;

    /* 共现表: 开放寻址哈希 */
    typedef struct { int vn, vi, cnt, used; } CE;
    int csz = 1;
    while (csz < token_count * 2) csz *= 2;
    CE* cooc = (CE*)calloc((size_t)csz, sizeof(CE));
    if (!cooc) return 0;
    int cmask = csz - 1;

    for (int t = 0; t < token_count; t++) {
        if (tokens[t].word[0] == '\0') continue;

        int vn_id = -1;
        if (vocab->net) vn_id = huarong_net_find_concept(vocab->net, tokens[t].word);
        if (vn_id < 0) continue;
        tokens[t].vocab_node_id = vn_id;

        float t_ms = tokens[t].timestamp_ms;
        int framed = 0;
        for (int f = 0; f < frame_count; f++) {
            if (!frames[f].features) continue;
            float f_ms = frames[f].timestamp_ms;
            if (f_ms >= t_ms - window_ms && f_ms <= t_ms + window_ms) framed++;
        }
        if (framed == 0) continue;

        int vis_id = vc_find_or_create_visual_node(vc, tokens[t].word);
        if (vis_id < 0) continue;

        uintptr_t h = ((uintptr_t)vn_id * 31 + (uintptr_t)vis_id);
        for (int slot = 0; slot <= cmask; slot++) {
            int idx = (int)((h + (uintptr_t)slot) & (uintptr_t)cmask);
            if (!cooc[idx].used) {
                cooc[idx].vn = vn_id; cooc[idx].vi = vis_id;
                cooc[idx].cnt = framed; cooc[idx].used = 1;
                break;
            } else if (cooc[idx].vn == vn_id && cooc[idx].vi == vis_id) {
                cooc[idx].cnt += framed; break;
            }
        }
    }

    int vtopo_id = vocab->topo_id;
    int ttopo_id = visual->topo_id;
    for (int i = 0; i <= cmask; i++) {
        if (!cooc[i].used || cooc[i].cnt < vc->cfg.min_cooccurrence) continue;
        if (cross_link_exists(vc->topology, vtopo_id, cooc[i].vn, ttopo_id, cooc[i].vi))
            continue;

        int ret = master_add_cross_link(vc->topology, vtopo_id, cooc[i].vn,
                                        ttopo_id, cooc[i].vi,
                                        vc->cfg.visual_edge_weight, "visual_anchor");
        if (ret >= 0) {
            edges_created++;
            if (vc->thalamus) {
                BrainSignal sig; memset(&sig, 0, sizeof(sig));
                sig.type = THAL_SIG_CROSS_MODAL_EDGE;
                sig.source = THAL_VISUAL_CORTEX;
                sig.target = -1;
                sig.data.consolidate.node_id = cooc[i].vi;
                sig.data.consolidate.topo_id = ttopo_id;
                thalamus_send_signal(vc->thalamus, -1, &sig);
            }
        }
    }

    vc->total_cross_modal_edges += edges_created;
    free(cooc);
    return edges_created;
}

/* ==================== 处理单个视频 ==================== */

static int vc_process_one_video(VisualCortex* vc, const char* filepath, const char* mode) {
    if (strcmp(mode, "subtitle") == 0) {
        /* 仅字幕模式: 通过 MediaReader 文本管道 */
        if (!vc->media_reader) return -1;
        int ret = media_process_file(vc->media_reader, filepath);
        vc->total_files_processed++;
        return ret;
    }

    /* 视觉模式: 帧提取 + 跨模态对齐 */
    if (vc->cfg.frame_output_dir[0]) {
#ifdef _WIN32
        _mkdir(vc->cfg.frame_output_dir);
#else
        mkdir(vc->cfg.frame_output_dir, 0755);
#endif
    }

    vc->frame_count = vc_extract_frames(vc, filepath);
    if (vc->frame_count <= 0) {
        fprintf(stderr, "[visual] 帧提取失败 (ffprobe=%s): %s\n",
                vc_get_ffprobe_path(vc), filepath);
        fprintf(stderr, "[visual] 提示: 请确认 ffprobe 可用 + 视频文件完整\n");
        return -1;
    }
    vc->total_frames_processed += vc->frame_count;
    if (vc->cfg.verbose) printf("[visual]   提取 %d 帧\n", vc->frame_count);

    SubtitleEntry* subs = (SubtitleEntry*)calloc(VC_MAX_TOKENS_PER_VIDEO, sizeof(SubtitleEntry));
    if (!subs) return -1;

    int sub_count = vc_extract_subtitles(vc, filepath, subs, VC_MAX_TOKENS_PER_VIDEO);
    if (sub_count <= 0) {
        if (vc->cfg.verbose) printf("[visual]   无字幕，跳过对齐\n");
        free(subs);
        vc->total_files_processed++;
        return 0;
    }
    if (vc->cfg.verbose) printf("[visual]   提取 %d 条字幕\n", sub_count);

    int max_tok = sub_count * 8;
    TimedToken* tokens = (TimedToken*)calloc((size_t)max_tok, sizeof(TimedToken));
    if (!tokens) { free(subs); return -1; }
    int tok_count = vc_tokenize(subs, sub_count, tokens, max_tok);

    /* 注册词汇节点 */
    SubTopology* vocab = vc_get_vocab_topo(vc);
    if (vocab && vocab->net) {
        for (int t = 0; t < tok_count; t++) {
            if (tokens[t].word[0] == '\0') continue;
            ReasoningNode* node = huarong_net_find_or_create_node(
                vocab->net, tokens[t].word, NULL, 0, vocab->node_hash);
            if (node) {
                tokens[t].vocab_node_id = node->node_id;
                if (!node->features && vc->cfg.feature_dim > 0) {
                    node->features = (float*)calloc((size_t)vc->cfg.feature_dim, sizeof(float));
                    if (node->features) node->feature_dim = vc->cfg.feature_dim;
                }
            }
        }
    }

    int edges = vc_perform_alignment(vc, vc->frames, vc->frame_count, tokens, tok_count);
    if (vc->cfg.verbose) printf("[visual]   跨模态对齐: +%d 边\n", edges);

    /* 编码器回调 */
    if (vc->encoder_fn) {
        for (int f = 0; f < vc->frame_count; f++)
            if (vc->frames[f].features)
                vc->encoder_fn(vc->frames[f].frame_file, vc->cfg.feature_dim,
                              vc->frames[f].features, vc->encoder_ctx);
    }

    /* 清理帧 */
    for (int f = 0; f < vc->frame_count; f++) { free(vc->frames[f].features); vc->frames[f].features = NULL; }
    vc->frame_count = 0;

    free(tokens);
    free(subs);
    vc->total_files_processed++;

    /* 完成信号 */
    if (vc->thalamus) {
        BrainSignal sig; memset(&sig, 0, sizeof(sig));
        sig.type = THAL_SIG_MEDIA_FILE_DONE;
        sig.source = THAL_VISUAL_CORTEX;
        sig.target = -1;
        sig.data.feedback.consolidated = edges;
        thalamus_send_signal(vc->thalamus, -1, &sig);
    }

    return edges;
}

/* ==================== 脑区 API ==================== */

int visual_cortex_tick(VisualCortex* vc, float throttle) {
    if (!vc) return 0;

    /* 门控: throttle 太低就跳过 */
    if (throttle < 0.1f) return 0;

    /* 检查队列 */
    VCTask task;
    if (vc_queue_pop(vc, &task) < 0) {
        /* 队列空: 冷却后退 */
        vc->idle_cooldown = vc->cfg.idle_cooldown_ticks;
        return 0;
    }

    vc->idle_cooldown = 0;

    /* 限速: 每 tick 最多处理 max_batch_per_tick 个 */
    int batch_max = (vc->cfg.max_batch_per_tick > 0) ? vc->cfg.max_batch_per_tick : 1;
    int processed = 0;

    do {
        if (vc->cfg.verbose)
            printf("[visual] tick: 处理 [%s] mode=%s\n", task.filepath, task.mode);

        int ret = vc_process_one_video(vc, task.filepath, task.mode);
        if (ret >= 0) processed++;

        if (vc->thalamus) {
            BrainSignal sig; memset(&sig, 0, sizeof(sig));
            sig.type = THAL_SIG_VISUAL_FRAME;
            sig.source = THAL_VISUAL_CORTEX;
            sig.target = -1;
            sig.data.search.count = processed;
            thalamus_send_signal(vc->thalamus, -1, &sig);
        }
    } while (processed < batch_max && vc_queue_pop(vc, &task) == 0);

    return processed;
}

int visual_cortex_enqueue(VisualCortex* vc, const char* filepath, const char* mode) {
    if (!vc || !filepath) return -1;
    return vc_queue_push(vc, filepath, mode ? mode : "visual");
}

int visual_cortex_enqueue_directory(VisualCortex* vc, const char* dirpath,
                                    const char* extensions, int recursive) {
    if (!vc || !dirpath) return -1;

    int enqueued = 0;
    const char* video_exts = extensions ? extensions : ".mp4.mkv.avi.webm.flv.mov.wmv.ts.m4v";

#ifdef _WIN32
    WIN32_FIND_DATA fd;
    char sp[1024];
    snprintf(sp, sizeof(sp), "%s\\*", dirpath);
    HANDLE h = FindFirstFile(sp, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char fp[1024];
        snprintf(fp, sizeof(fp), "%s\\%s", dirpath, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (recursive) enqueued += visual_cortex_enqueue_directory(vc, fp, extensions, recursive);
        } else {
            const char* ext = strrchr(fd.cFileName, '.');
            if (ext && strstr(video_exts, ext)) {
                if (vc_queue_push(vc, fp, "visual") == 0) enqueued++;
            }
        }
    } while (FindNextFile(h, &fd));
    FindClose(h);
#else
    DIR* dir = opendir(dirpath);
    if (!dir) return -1;
    struct dirent* e;
    while ((e = readdir(dir)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char fp[1024];
        snprintf(fp, sizeof(fp), "%s/%s", dirpath, e->d_name);
        struct stat st;
        if (stat(fp, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (recursive) enqueued += visual_cortex_enqueue_directory(vc, fp, extensions, recursive);
        } else {
            const char* ext = strrchr(e->d_name, '.');
            if (ext && strstr(video_exts, ext)) {
                if (vc_queue_push(vc, fp, "visual") == 0) enqueued++;
            }
        }
    }
    closedir(dir);
#endif
    return enqueued;
}

int visual_cortex_queue_size(VisualCortex* vc) {
    if (!vc) return 0;
    pthread_mutex_lock(&vc->task_lock);
    int sz = vc->task_count;
    pthread_mutex_unlock(&vc->task_lock);
    return sz;
}

void visual_cortex_get_stats(VisualCortex* vc,
                             int*  out_queue,
                             long* out_frames,
                             int*  out_vis_nodes,
                             int*  out_xmod_edges) {
    if (!vc) return;
    if (out_queue)     *out_queue     = visual_cortex_queue_size(vc);
    if (out_frames)    *out_frames    = vc->total_frames_processed;
    if (out_vis_nodes) *out_vis_nodes = vc->total_visual_nodes;
    if (out_xmod_edges) *out_xmod_edges = vc->total_cross_modal_edges;
}

void visual_cortex_set_encoder(VisualCortex* vc,
                               VisualFeatureEncoder encode_fn, void* ctx) {
    if (!vc) return;
    vc->encoder_fn  = encode_fn;
    vc->encoder_ctx = ctx;
}

MediaReader* visual_cortex_get_media_reader(VisualCortex* vc) {
    return vc ? vc->media_reader : NULL;
}

/* ==================== 生命周期 ==================== */

VisualCortex* visual_cortex_create(MasterTopology* topology,
                                   const VisualCortexConfig* cfg) {
    if (!topology) return NULL;

    VisualCortex* vc = (VisualCortex*)calloc(1, sizeof(VisualCortex));
    if (!vc) return NULL;

    vc->topology = topology;
    vc->cfg      = (cfg) ? *cfg : (VisualCortexConfig)VISUAL_CORTEX_DEFAULT_CONFIG;

    if (!vc->cfg.ffmpeg_path[0])
        strncpy(vc->cfg.ffmpeg_path, "ffmpeg", sizeof(vc->cfg.ffmpeg_path) - 1);

    /* 帧存储 */
    vc->frame_capacity = (vc->cfg.max_frames_per_video > 0)
                         ? vc->cfg.max_frames_per_video : VC_MAX_FRAMES_PER_VIDEO;
    vc->frames = (VisualFrame*)calloc((size_t)vc->frame_capacity, sizeof(VisualFrame));
    if (!vc->frames) { free(vc); return NULL; }

    /* 任务队列 */
    vc->task_capacity = VC_TASK_QUEUE_SIZE;
    vc->tasks = (VCTask*)calloc((size_t)vc->task_capacity, sizeof(VCTask));
    if (!vc->tasks) { free(vc->frames); free(vc); return NULL; }
    vc->task_head = 0;
    vc->task_tail = 0;
    vc->task_count = 0;
    pthread_mutex_init(&vc->task_lock, NULL);

    /* 内部 MediaReader — 传入 ffprobe 路径用于字幕探测 */
    MediaReaderConfig mrc = MEDIA_READER_DEFAULT_CONFIG;
    mrc.verbose = vc->cfg.verbose;
    strncpy(mrc.ffmpeg_path, vc->cfg.ffmpeg_path, sizeof(mrc.ffmpeg_path) - 1);
    if (vc->cfg.ffprobe_path[0])
        strncpy(mrc.ffprobe_path, vc->cfg.ffprobe_path, sizeof(mrc.ffprobe_path) - 1);
    vc->media_reader = media_reader_create(topology, &mrc);
    if (!vc->media_reader) {
        fprintf(stderr, "[visual] MediaReader 创建失败\n");
    }

    /* 视觉拓扑 */
    if (!vc_ensure_visual_topo(vc)) {
        visual_cortex_destroy(vc);
        return NULL;
    }

    printf("[visual] 视觉皮层脑区就绪 (拓扑:%d, 队列:%d, dim:%d)\n",
           TOPO_VISUAL, vc->task_capacity, vc->cfg.feature_dim);
    return vc;
}

void visual_cortex_destroy(VisualCortex* vc) {
    if (!vc) return;

    /* 清理帧 */
    for (int i = 0; i < vc->frame_count; i++)
        free(vc->frames[i].features);
    free(vc->frames);

    /* 清理 MediaReader */
    if (vc->media_reader) media_reader_destroy(vc->media_reader);

    /* 清理任务队列 */
    free(vc->tasks);
    pthread_mutex_destroy(&vc->task_lock);

    printf("[visual] 视觉皮层已关闭 (文件:%ld, 帧:%ld, 视觉节点:%d, 跨模态边:%d)\n",
           vc->total_files_processed, vc->total_frames_processed,
           vc->total_visual_nodes, vc->total_cross_modal_edges);

    free(vc);
}
