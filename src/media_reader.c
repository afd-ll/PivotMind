/**
 * @file media_reader.c
 * @brief 媒体阅读器实现 — SRT 字幕提取 + 文本管道
 *
 * 依赖：系统需安装 ffmpeg 命令行工具
 * 编译：gcc -c media_reader.c -I../include
 */

#include "media_reader.h"
#include "thalamus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define popen  _popen
#define pclose _pclose
#define stat   _stat
#define S_ISDIR(m) ((m) & _S_IFDIR)
#define S_ISREG(m) ((m) & _S_IFREG)
#else
#include <dirent.h>
#include <unistd.h>
#endif

/* ==================== 内部结构 ==================== */

struct MediaReader {
    MasterTopology*    topology;
    ArticleReader*     article_reader;
    ArticleReaderConfig ar_cfg;

    /* ffmpeg 配置 */
    char  ffmpeg_path[256];
    int   subtitle_track;

    /* 丘脑信号总线 (可选) */
    Thalamus* thalamus;

    /* 内部缓冲 */
    int   flush_interval;         /* 覆盖 batch_size 的值或0=默认 */
    int   line_count;             /* 当前累计行数 */
    int   verbose;

    /* 统计 */
    long  total_files_processed;
    long  total_lines_fed;
    long  total_words_discovered;
};

/* ==================== SRT 时间戳解析 ==================== */

/**
 * 解析 SRT 时间戳行: "00:01:23,456 --> 00:01:25,789"
 * 返回文本行开始位置在 buf 中的偏移，0 = 不是时间戳行
 */
static int srt_parse_timestamp_line(const char* buf, float* start_ms, float* end_ms) {
    int h1, m1, s1, ms1, h2, m2, s2, ms2;
    int n = 0;

    if (sscanf(buf, "%d:%d:%d,%d --> %d:%d:%d,%d%n",
               &h1, &m1, &s1, &ms1, &h2, &m2, &s2, &ms2, &n) == 8 && n > 0) {
        *start_ms = (float)(h1 * 3600000 + m1 * 60000 + s1 * 1000 + ms1);
        *end_ms   = (float)(h2 * 3600000 + m2 * 60000 + s2 * 1000 + ms2);
        return n;
    }
    return 0;
}

/**
 * 检查行是否为 SRT 序号 (纯数字)
 */
static int srt_is_index_line(const char* line) {
    if (!line || !*line) return 0;
    for (const char* p = line; *p; p++) {
        if (!isdigit((unsigned char)*p)) return 0;
    }
    return 1;
}

/* ==================== ffmpeg 字幕提取 ==================== */

/**
 * 检测文件中是否存在字幕轨道
 * 通过调用 ffprobe 检测
 * @return 1=有字幕, 0=无, -1=ffprobe 不可用
 */
static int media_probe_subtitle(MediaReader* mr, const char* filepath) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "\"%s\" -v quiet -print_format json -show_streams \"%s\"",
             mr->ffmpeg_path, filepath);

    FILE* fp = popen(cmd, "r");
    if (!fp) {
        if (mr->verbose) fprintf(stderr, "[media] ffprobe 执行失败: %s\n", strerror(errno));
        return -1;
    }

    char buf[8192];
    size_t total = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[total] = '\0';
    int ret = pclose(fp);

    if (ret != 0) return -1;

    /* 简单检测: JSON 中包含 "subtitle" 字样 */
    if (strstr(buf, "\"codec_type\"") && strstr(buf, "subtitle"))
        return 1;

    /* 也检查 "codec_type\": \"subtitle\" 精确模式 */
    const char* p = buf;
    while ((p = strstr(p, "\"codec_type\"")) != NULL) {
        if (strstr(p, "subtitle")) return 1;
        p++;
    }

    return 0;
}

/**
 * 调用 ffmpeg 提取字幕文本到缓冲区
 * @param mr        媒体阅读器
 * @param filepath  视频文件路径
 * @param out_text  输出缓冲区 (需调用者释放)
 * @param out_len   输出文本长度
 * @return 0=成功, -1=失败
 */
static int media_extract_subtitle(MediaReader* mr, const char* filepath,
                                  char** out_text, size_t* out_len) {
    char cmd[1536];
    char tmpfile[512];

    /* 生成临时文件名 */
#ifdef _WIN32
    snprintf(tmpfile, sizeof(tmpfile), "pm_media_subs_%d.srt", (int)GetCurrentProcessId());
#else
    snprintf(tmpfile, sizeof(tmpfile), "/tmp/pm_media_subs_%d.srt", (int)getpid());
#endif

    /* ffmpeg: 提取字幕轨道到临时 SRT 文件 */
    if (mr->subtitle_track >= 0) {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -y -v quiet -i \"%s\" -map 0:s:%d -c:s srt \"%s\"",
                 mr->ffmpeg_path, filepath, mr->subtitle_track, tmpfile);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -y -v quiet -i \"%s\" -map 0:s:0 -c:s srt \"%s\"",
                 mr->ffmpeg_path, filepath, tmpfile);
    }

    int ret = system(cmd);
    if (ret != 0) {
        /* 清理临时文件 */
        remove(tmpfile);
        return -1;
    }

    /* 读取临时文件到内存 */
    FILE* fp = fopen(tmpfile, "rb");
    if (!fp) {
        remove(tmpfile);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    if (fsize <= 0) {
        fclose(fp);
        remove(tmpfile);
        return -1;
    }
    fseek(fp, 0, SEEK_SET);

    char* text = (char*)malloc((size_t)fsize + 1);
    if (!text) {
        fclose(fp);
        remove(tmpfile);
        return -1;
    }

    size_t read_bytes = fread(text, 1, (size_t)fsize, fp);
    text[read_bytes] = '\0';
    fclose(fp);
    remove(tmpfile);

    *out_text = text;
    *out_len  = read_bytes;
    return 0;
}

/* ==================== SRT 文本解析 + 喂料 ==================== */

/**
 * 解析 SRT 文本内容，逐行喂入 article_process_line
 *
 * SRT 格式:
 *   1
 *   00:00:01,000 --> 00:00:04,000
 *   这是苹果
 *
 *   2
 *   00:00:04,500 --> 00:00:08,000
 *   它是红色的
 *
 * @return 喂入的文本行数
 */
static int srt_feed_lines(MediaReader* mr, const char* srt_text, size_t srt_len) {
    int fed = 0;
    const char* p = srt_text;
    const char* end = srt_text + srt_len;
    int in_subtitle = 0;

    while (p < end) {
        /* 跳过行首空白 */
        while (p < end && (*p == '\r' || *p == '\n')) p++;
        if (p >= end) break;

        /* 找到行尾 */
        const char* line_start = p;
        while (p < end && *p != '\r' && *p != '\n') p++;
        size_t line_len = (size_t)(p - line_start);

        /* 跳过空白行 */
        if (line_len == 0) {
            in_subtitle = 0;
            continue;
        }

        /* 复制到 buffer, strip trailing \r */
        char line_buf[2048];
        if (line_len >= sizeof(line_buf)) line_len = sizeof(line_buf) - 1;
        memcpy(line_buf, line_start, line_len);
        line_buf[line_len] = '\0';
        /* trim trailing spaces */
        while (line_len > 0 && isspace((unsigned char)line_buf[line_len - 1]))
            line_buf[--line_len] = '\0';

        /* 跳过空行 */
        if (line_len == 0) {
            in_subtitle = 0;
            continue;
        }

        /* 跳过序号行 */
        if (srt_is_index_line(line_buf)) {
            in_subtitle = 1;
            continue;
        }

        /* 跳过时间戳行 */
        float t1, t2;
        int ts_offset = srt_parse_timestamp_line(line_buf, &t1, &t2);
        if (ts_offset > 0) {
            in_subtitle = 1;
            continue;
        }

        /* 文本行：只有在字幕条目内才喂料 */
        if (in_subtitle) {
            /* 跳过 HTML 标签 <i> </i> <b> <font...> 等 */
            char clean[2048];
            int ci = 0, in_tag = 0;
            for (size_t i = 0; i < line_len && ci < (int)sizeof(clean) - 1; i++) {
                unsigned char c = (unsigned char)line_buf[i];
                if (c == '<') { in_tag = 1; continue; }
                if (c == '>') { in_tag = 0; continue; }
                if (!in_tag) clean[ci++] = (char)c;
            }
            clean[ci] = '\0';

            /* 跳过空字符串和纯标点 */
            int has_text = 0;
            for (int i = 0; clean[i]; i++) {
                if ((unsigned char)clean[i] > 0x20 && clean[i] != ' ' && clean[i] != '\t') {
                    has_text = 1;
                    break;
                }
            }
            if (!has_text) continue;

            /* 跳过常见的非对话字幕标记 (如 ♪ 歌词符号, [音乐] 等) */
            if (clean[0] == '♪' || clean[0] == '♫' ||
                (clean[0] == '[' && strchr(clean, ']'))) {
                continue;
            }

            /* 展开多行字幕为一整句，按标点分句 */
            /* 策略: 直接喂整行, 让 PMI 管道自己处理分词 */
            article_process_line(mr->article_reader, clean);
            fed++;
            mr->line_count++;
            mr->total_lines_fed++;

            if (mr->verbose && (fed % 50 == 0)) {
                printf("  [media] 已喂入 %d 行...\n", fed);
            }

            /* 达到 flush 间隔 → 触发词发现 */
            int fi = (mr->flush_interval > 0) ? mr->flush_interval
                                              : 200; /* 默认 batch_size */
            if (mr->line_count >= fi) {
                int added = article_flush(mr->article_reader, NULL);
                mr->total_words_discovered += (added > 0 ? added : 0);
                mr->line_count = 0;
                if (mr->verbose && added > 0) {
                    printf("  [media] flush: +%d 新词\n", added);
                }
            }
        }
    }

    /* 强制最终 flush */
    if (mr->line_count > 0) {
        int added = article_flush(mr->article_reader, NULL);
        mr->total_words_discovered += (added > 0 ? added : 0);
        mr->line_count = 0;
        if (mr->verbose && added > 0) {
            printf("  [media] 最终 flush: +%d 新词\n", added);
        }
    }

    return fed;
}

/* ==================== 文件/目录处理 ==================== */

int media_process_file(MediaReader* mr, const char* filepath) {
    if (!mr || !filepath) return -1;

    /* 检查文件扩展名是否为视频格式 */
    const char* ext = strrchr(filepath, '.');
    if (!ext) return -1;

    /* 支持的视频扩展名 */
    const char* video_exts = ".mp4.mkv.avi.webm.flv.mov.wmv.ts.m4v";
    char ext_lower[16];
    int i;
    for (i = 0; ext[i] && i < 15; i++)
        ext_lower[i] = (char)tolower((unsigned char)ext[i]);
    ext_lower[i] = '\0';

    if (!strstr(video_exts, ext_lower)) {
        if (mr->verbose) fprintf(stderr, "[media] 跳过非视频文件: %s\n", filepath);
        return 0;
    }

    printf("[media] 处理: %s\n", filepath);

    /* 检测字幕轨道 */
    int has_sub = media_probe_subtitle(mr, filepath);
    if (has_sub <= 0) {
        if (mr->verbose)
            fprintf(stderr, "[media] 无字幕轨道 (has_sub=%d): %s\n", has_sub, filepath);
        return (has_sub < 0) ? -1 : 0;
    }

    /* 提取字幕 */
    char* srt_text = NULL;
    size_t srt_len = 0;
    if (media_extract_subtitle(mr, filepath, &srt_text, &srt_len) < 0) {
        fprintf(stderr, "[media] 字幕提取失败: %s\n", filepath);
        return -1;
    }

    if (!srt_text || srt_len == 0) {
        free(srt_text);
        return 0;
    }

    /* 解析并喂料 */
    int fed = srt_feed_lines(mr, srt_text, srt_len);
    free(srt_text);

    mr->total_files_processed++;
    printf("[media] 完成: %s → +%d 行文本\n", filepath, fed);

    /* 通过丘脑发送完成信号 */
    if (mr->thalamus) {
        BrainSignal sig;
        memset(&sig, 0, sizeof(sig));
        sig.type   = THAL_SIG_MEDIA_FILE_DONE;
        sig.source = THAL_PERCEPTION;
        sig.target = -1;
        sig.data.feedback.consolidated = fed;
        thalamus_send_signal(mr->thalamus, -1, &sig);
    }

    return fed;
}

int media_process_directory(MediaReader* mr, const char* dirpath,
                            const char* extensions) {
    if (!mr || !dirpath) return -1;

    int processed = 0;

#ifdef _WIN32
    WIN32_FIND_DATA fd;
    char search_path[1024];
    snprintf(search_path, sizeof(search_path), "%s\\*", dirpath);

    HANDLE hFind = FindFirstFile(search_path, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[media] 目录遍历失败: %s\n", dirpath);
        return -1;
    }

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;

        char fullpath[1024];
        snprintf(fullpath, sizeof(fullpath), "%s\\%s", dirpath, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (mr->flush_interval > 0) /* recursive 复用 flush_interval 标志 */
                media_process_directory(mr, fullpath, extensions);
        } else {
            int ret = media_process_file(mr, fullpath);
            if (ret > 0) processed++;
        }
    } while (FindNextFile(hFind, &fd));
    FindClose(hFind);
#else
    DIR* dir = opendir(dirpath);
    if (!dir) {
        fprintf(stderr, "[media] 目录打开失败: %s (%s)\n", dirpath, strerror(errno));
        return -1;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char fullpath[1024];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, entry->d_name);

        struct stat st;
        if (stat(fullpath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (mr->flush_interval > 0) { /* recursive flag */
                int sub = media_process_directory(mr, fullpath, extensions);
                if (sub >= 0) processed += sub;
            }
        } else if (S_ISREG(st.st_mode)) {
            /* 扩展名过滤 */
            if (extensions) {
                const char* fext = strrchr(entry->d_name, '.');
                if (!fext) continue;
                if (!strstr(extensions, fext)) continue;
            }
            int ret = media_process_file(mr, fullpath);
            if (ret > 0) processed++;
        }
    }
    closedir(dir);
#endif

    return processed;
}

/* ==================== 生命周期 ==================== */

MediaReader* media_reader_create(MasterTopology* topology,
                                 const MediaReaderConfig* cfg) {
    if (!topology) return NULL;

    MediaReader* mr = (MediaReader*)calloc(1, sizeof(MediaReader));
    if (!mr) return NULL;

    mr->topology = topology;

    /* 配置 ffmpeg */
    if (cfg && cfg->ffmpeg_path[0]) {
        strncpy(mr->ffmpeg_path, cfg->ffmpeg_path, sizeof(mr->ffmpeg_path) - 1);
    } else {
        strncpy(mr->ffmpeg_path, "ffmpeg", sizeof(mr->ffmpeg_path) - 1);
    }

    mr->subtitle_track = (cfg) ? cfg->subtitle_track : -1;
    mr->flush_interval = (cfg) ? cfg->flush_interval : 0;
    mr->verbose        = (cfg) ? cfg->verbose : 0;

    /* 创建 ArticleReader */
    mr->ar_cfg = ARTICLE_READER_DEFAULT_CONFIG;
    if (cfg && cfg->flush_interval > 0) {
        mr->ar_cfg.batch_size = cfg->flush_interval;
    }
    mr->ar_cfg.verbose = mr->verbose;

    mr->article_reader = article_reader_create(topology, &mr->ar_cfg);
    if (!mr->article_reader) {
        free(mr);
        return NULL;
    }

    printf("[media] 媒体阅读器就绪\n");
    return mr;
}

void media_reader_destroy(MediaReader* mr) {
    if (!mr) return;

    if (mr->article_reader) {
        /* 强制最终 flush */
        if (mr->line_count > 0) {
            article_flush(mr->article_reader, NULL);
        }
        article_reader_destroy(mr->article_reader);
    }

    printf("[media] 媒体阅读器已销毁 (文件:%ld, 行:%ld, 词:%ld)\n",
           mr->total_files_processed, mr->total_lines_fed,
           mr->total_words_discovered);

    free(mr);
}

void media_reader_get_stats(MediaReader* mr,
                            long* out_files,
                            long* out_lines,
                            long* out_words) {
    if (!mr) return;
    if (out_files) *out_files = mr->total_files_processed;
    if (out_lines) *out_lines = mr->total_lines_fed;
    if (out_words) *out_words = mr->total_words_discovered;
}

void media_reader_set_thalamus(MediaReader* mr, Thalamus* th) {
    if (!mr) return;
    mr->thalamus = th;
    if (mr->article_reader)
        article_reader_set_thalamus(mr->article_reader, th);
}
