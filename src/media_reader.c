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
#include <limits.h>

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
#include <sys/wait.h>
#include <fcntl.h>
#endif

/* ==================== 内部结构 ==================== */

struct MediaReader {
    MasterTopology*    topology;
    ArticleReader*     article_reader;
    ArticleReaderConfig ar_cfg;

    /* ffmpeg / ffprobe 配置 */
    char  ffmpeg_path[256];
    char  ffprobe_path[256];      /* v0.5.1: 独立 ffprobe 路径 (空=自动推导) */
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

/* ==================== ffprobe / ffmpeg 工具路径 ==================== */

/**
 * 获取 ffprobe 路径: 如果配置了就用配置的，否则从 ffmpeg_path 推导
 * (把路径中最后的 "ffmpeg" 替换为 "ffprobe")
 */
static const char* media_get_ffprobe_path(MediaReader* mr) {
    if (mr->ffprobe_path[0] != '\0')
        return mr->ffprobe_path;

    /* 自动推导: "ffmpeg" → "ffprobe", "D:\tools\ffmpeg.exe" → "D:\tools\ffprobe.exe" */
    static char derived[256];
    strncpy(derived, mr->ffmpeg_path, sizeof(derived) - 1);
    derived[sizeof(derived) - 1] = '\0';

    char* needle = strstr(derived, "ffmpeg");
    if (needle) {
        /* 替换 "ffmpeg" → "ffprobe" (长度相同: 6 chars) */
        memcpy(needle, "ffprobe", 7);
    } else {
        /* 路径中不含 ffmpeg，直接用 ffprobe */
        strncpy(derived, "ffprobe", sizeof(derived) - 1);
    }
    return derived;
}

/* ==================== 子进程执行 (C1 安全修复: 去 shell 解释层) ====================
 *
 * 用 fork + execvp 替代 popen/system，参数数组直接传值，杜绝 shell 命令注入。
 * 两个关键点:
 *   - 用 execvp 而非 execv: probe/ffmpeg 可能是裸名 "ffprobe" (靠 PATH 查找)。
 *   - popen 替换必须先 drain 管道再 waitpid，否则 64KB 管道写满会父子互相死锁。
 */

#ifndef _WIN32

/* 执行 prog 并捕获 stdout 到 out_buf (最多保留 out_cap-1 字节，其余读空丢弃防死锁)。
 * 返回子进程退出码；-1 = fork/pipe/wait 失败，127 = execvp 失败。 */
static int run_capture(const char* prog, char* const argv[],
                       char* out_buf, size_t out_cap, size_t* out_len) {
    int pipefd[2];
    if (out_len) *out_len = 0;
    if (pipe(pipefd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        /* 子进程: stdout → 管道, stderr → /dev/null */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0) { dup2(nullfd, STDERR_FILENO); close(nullfd); }
        execvp(prog, argv);
        _exit(127);
    }

    /* 父进程: 先 drain 管道到 EOF，再 waitpid (防 64KB 管道满死锁) */
    close(pipefd[1]);
    size_t total = 0;
    char sink[4096];
    ssize_t n;
    while ((n = read(pipefd[0], sink, sizeof(sink))) > 0) {
        if (out_buf && out_cap > 1) {
            size_t room = out_cap - 1 - total;
            if (room > 0) {
                size_t copy = ((size_t)n < room) ? (size_t)n : room;
                memcpy(out_buf + total, sink, copy);
                total += copy;
            }
        }
    }
    close(pipefd[0]);
    if (out_buf && out_cap > 0) {
        out_buf[(total < out_cap) ? total : (out_cap - 1)] = '\0';
    }
    if (out_len) *out_len = total;

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/* 执行 prog，丢弃 stdout/stderr (替代 system)，返回退出码；-1=fork/wait 失败, 127=exec 失败 */
static int run_silent(const char* prog, char* const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0) {
            dup2(nullfd, STDOUT_FILENO);
            dup2(nullfd, STDERR_FILENO);
            close(nullfd);
        }
        execvp(prog, argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

#endif /* !_WIN32 */

/* ==================== 媒体路径安全校验 (C1 安全修复: 灭 SSRF) ==================== */

/* 默认媒体目录白名单（环境变量 PIVOTMIND_MEDIA_DIRS 可覆盖，冒号分隔） */
static const char* media_default_dirs = "/mnt/sdcard/media:/mnt/sdcard";

/* 判断 path 是否位于 dir 目录内 (dir 本身或 dir/ 前缀)，防 "/mnt/sdcard2" 误匹配 */
static int path_within_dir(const char* path, const char* dir) {
    size_t dlen = strlen(dir);
    if (dlen == 0) return 0;
    if (strncmp(path, dir, dlen) != 0) return 0;
    if (path[dlen] == '\0') return 1;   /* 精确等于 dir */
    if (path[dlen] == '/')  return 1;   /* dir/ 前缀 */
    return 0;
}

int media_validate_media_path(const char* filepath, char* resolved_out, size_t resolved_cap) {
#ifdef _WIN32
    /* Windows 分支: 非目标平台，仅做基础存在性检查 (realpath 为 POSIX) */
    struct stat st;
    if (!filepath || !*filepath || stat(filepath, &st) != 0 || !S_ISREG(st.st_mode))
        return -1;
    if (resolved_out && resolved_cap > 0) {
        strncpy(resolved_out, filepath, resolved_cap - 1);
        resolved_out[resolved_cap - 1] = '\0';
    }
    return 0;
#else
    if (!filepath || !*filepath) return -1;

    /* 1. realpath: 解析符号链接 + 规范化 + 校验存在性 */
    char resolved[PATH_MAX];
    if (!realpath(filepath, resolved)) {
        fprintf(stderr, "[media] 路径校验失败 (不存在/无法解析): %s (%s)\n",
                filepath, strerror(errno));
        return -1;
    }

    /* 2. 必须是常规文件 (拒绝目录/设备/管道) */
    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "[media] 路径校验失败 (非常规文件): %s\n", filepath);
        return -1;
    }

    /* 3. 媒体目录白名单: 解析后的真实路径必须位于允许目录内 (灭 SSRF) */
    const char* dirs = getenv("PIVOTMIND_MEDIA_DIRS");
    if (!dirs || !*dirs) dirs = media_default_dirs;

    char dirs_copy[PATH_MAX];
    strncpy(dirs_copy, dirs, sizeof(dirs_copy) - 1);
    dirs_copy[sizeof(dirs_copy) - 1] = '\0';

    int allowed = 0;
    char* saveptr = NULL;
    for (char* tok = strtok_r(dirs_copy, ":", &saveptr);
         tok; tok = strtok_r(NULL, ":", &saveptr)) {
        if (path_within_dir(resolved, tok)) { allowed = 1; break; }
    }
    if (!allowed) {
        fprintf(stderr, "[media] 路径校验失败 (不在媒体目录白名单内): %s\n", filepath);
        return -1;
    }

    if (resolved_out && resolved_cap > 0) {
        strncpy(resolved_out, resolved, resolved_cap - 1);
        resolved_out[resolved_cap - 1] = '\0';
    }
    return 0;
#endif
}

/* ==================== ffmpeg 字幕提取 ==================== */

/**
 * 检测文件中是否存在字幕轨道
 * 使用 ffprobe (非 ffmpeg) 进行流检测
 * @return 1=有字幕, 0=无, -1=ffprobe 不可用
 */
static int media_probe_subtitle(MediaReader* mr, const char* filepath) {
    const char* probe = media_get_ffprobe_path(mr);

    /* C1 修复: 路径安全校验 (灭 SSRF) + 用规范化后的真实路径执行 */
    char resolved[PATH_MAX];
    if (media_validate_media_path(filepath, resolved, sizeof(resolved)) != 0)
        return -1;

    if (mr->verbose)
        printf("[media] 探测字幕: %s %s\n", probe, resolved);

#ifdef _WIN32
    /* Windows 分支: 非目标平台，保留旧 shell 实现 (execvp 为 POSIX) */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "\"%s\" -v error -select_streams s -show_entries "
             "stream=index:stream=codec_name:stream_tags=language "
             "-of csv=p=0 \"%s\"",
             probe, resolved);

    FILE* fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "[media] ffprobe 启动失败 (路径=%s): %s\n",
                probe, strerror(errno));
        return -1;
    }

    char buf[4096];
    size_t total = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[total] = '\0';
    int ret = pclose(fp);
#else
    /* Linux 分支: fork + execvp 传参，去 shell 解释层，杜绝命令注入 */
    char* argv[] = {
        (char*)probe, "-v", "error", "-select_streams", "s",
        "-show_entries", "stream=index:stream=codec_name:stream_tags=language",
        "-of", "csv=p=0", (char*)resolved, NULL
    };

    char buf[4096];
    size_t total = 0;
    int ret = run_capture(probe, argv, buf, sizeof(buf), &total);
#endif

    if (ret != 0) {
        fprintf(stderr, "[media] ffprobe 返回错误码 %d (路径=%s)\n"
                "[media] 提示: 请确认视频文件存在且 ffprobe 已安装 (%s)\n",
                ret, probe, filepath);
        return -1;
    }

    /* 有输出行 = 有字幕轨道 */
    for (size_t i = 0; i < total; i++) {
        if (buf[i] != '\n' && buf[i] != '\r' && buf[i] != ' ' && buf[i] != '\t') {
            if (mr->verbose) {
                printf("[media] 检测到字幕轨道:\n");
                /* 逐行打印轨道信息 */
                char* line = strtok(buf, "\n");
                while (line) {
                    printf("  %s\n", line);
                    line = strtok(NULL, "\n");
                }
            }
            return 1;
        }
    }

    /* 无输出 → 无字幕轨道。回退检查: 是否 ffprobe 本身不可用 */
    {
#ifdef _WIN32
        char test_cmd[512];
        snprintf(test_cmd, sizeof(test_cmd), "\"%s\" -version > NUL 2>&1", probe);
        int has_probe = (system(test_cmd) == 0);
#else
        /* C1 修复: 原 "> NUL 2>&1" 是 Windows-ism，Linux 下会生成字面 NUL 文件。
         * 改为 execvp 直接看退出码，stdout/stderr 已在 run_silent 内重定向到 /dev/null。 */
        char* version_argv[] = { (char*)probe, "-version", NULL };
        int has_probe = (run_silent(probe, version_argv) == 0);
#endif
        if (!has_probe) {
            fprintf(stderr, "[media] %s 不可用！请安装 ffmpeg/ffprobe 并将其加入 PATH\n", probe);
            fprintf(stderr, "[media] 下载: https://ffmpeg.org/download.html\n");
            return -1;
        }
    }

    if (mr->verbose)
        printf("[media] 视频无软字幕轨道 (可能为硬字幕/烧录字幕，需 ASR 后备)\n");
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
    char tmpfile[512];

    /* 生成临时文件名 */
#ifdef _WIN32
    snprintf(tmpfile, sizeof(tmpfile), "pm_media_subs_%d.srt", (int)GetCurrentProcessId());
#else
    snprintf(tmpfile, sizeof(tmpfile), "/tmp/pm_media_subs_%d.srt", (int)getpid());
#endif

    /* C1 修复: 路径安全校验 (灭 SSRF) + 用规范化后的真实路径执行 */
    char resolved[PATH_MAX];
    if (media_validate_media_path(filepath, resolved, sizeof(resolved)) != 0)
        return -1;

    char track_arg[32];

#ifdef _WIN32
    /* Windows 分支: 非目标平台，保留旧 shell 实现 */
    char cmd[1536];
    if (mr->subtitle_track >= 0) {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -y -v error -i \"%s\" -map 0:s:%d -c:s srt \"%s\"",
                 mr->ffmpeg_path, resolved, mr->subtitle_track, tmpfile);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -y -v error -i \"%s\" -map 0:s:0? -c:s srt \"%s\"",
                 mr->ffmpeg_path, resolved, tmpfile);
    }

    if (mr->verbose)
        printf("[media] 提取字幕: %s\n", cmd);

    int ret = system(cmd);
#else
    /* Linux 分支: fork + execvp 传参，去 shell 解释层 */
    char* argv[16];
    int argc = 0;
    argv[argc++] = (char*)mr->ffmpeg_path;
    argv[argc++] = "-y";
    argv[argc++] = "-v"; argv[argc++] = "error";
    argv[argc++] = "-i"; argv[argc++] = (char*)resolved;
    if (mr->subtitle_track >= 0) {
        snprintf(track_arg, sizeof(track_arg), "0:s:%d", mr->subtitle_track);
        argv[argc++] = "-map"; argv[argc++] = track_arg;
    } else {
        argv[argc++] = "-map"; argv[argc++] = "0:s:0?";
    }
    argv[argc++] = "-c:s"; argv[argc++] = "srt";
    argv[argc++] = (char*)tmpfile;
    argv[argc] = NULL;

    if (mr->verbose)
        printf("[media] 提取字幕: %s -i %s -> %s\n", mr->ffmpeg_path, resolved, tmpfile);

    int ret = run_silent(mr->ffmpeg_path, argv);
#endif

    if (ret != 0) {
        fprintf(stderr, "[media] ffmpeg 字幕提取失败 (exit=%d)\n", ret);
        remove(tmpfile);
        return -1;
    }

    /* 读取临时文件到内存 */
    FILE* fp = fopen(tmpfile, "rb");
    if (!fp) {
        fprintf(stderr, "[media] 无法读取临时字幕文件: %s\n", tmpfile);
        remove(tmpfile);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    if (fsize <= 0) {
        fprintf(stderr, "[media] 字幕文件为空 (可能该轨道无有效字幕数据)\n");
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

    if (mr->verbose)
        printf("[media] 提取字幕: %zu 字节\n", read_bytes);

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
            if ((memcmp(clean, "\xE2\x99\xAA", 3) == 0 ||
                 memcmp(clean, "\xE2\x99\xAB", 3) == 0) ||
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

/* ==================== 轨道诊断 (v0.5.1) ==================== */

int media_diagnose_tracks(MediaReader* mr, const char* filepath) {
    if (!mr || !filepath) return -1;

    const char* probe = media_get_ffprobe_path(mr);

    /* C1 修复: 路径安全校验 (灭 SSRF) + 用规范化后的真实路径执行 */
    char resolved[PATH_MAX];
    if (media_validate_media_path(filepath, resolved, sizeof(resolved)) != 0)
        return -1;

    printf("=== 诊断: %s ===\n", resolved);
    printf("ffprobe 路径: %s\n", probe);

#ifdef _WIN32
    /* Windows 分支: 非目标平台，保留旧 shell 实现 */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "\"%s\" -v error -show_entries "
             "stream=index,codec_type,codec_name:stream_tags=language "
             "-of default=noprint_wrappers=1 \"%s\"",
             probe, resolved);

    FILE* fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "[诊断] 无法执行 ffprobe (%s): %s\n", probe, strerror(errno));
        return -1;
    }

    int track_count = 0;
    char line[512];
    printf("轨道列表:\n");
    while (fgets(line, sizeof(line), fp)) {
        /* 去掉换行 */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len > 0) {
            printf("  %s\n", line);
            track_count++;
        }
    }
    pclose(fp);
#else
    /* Linux 分支: fork + execvp 传参，去 shell 解释层 */
    char* argv[] = {
        (char*)probe, "-v", "error", "-show_entries",
        "stream=index,codec_type,codec_name:stream_tags=language",
        "-of", "default=noprint_wrappers=1", (char*)resolved, NULL
    };

    char outbuf[16384];
    size_t out_len = 0;
    int ret = run_capture(probe, argv, outbuf, sizeof(outbuf), &out_len);
    if (ret == -1) {
        fprintf(stderr, "[诊断] 无法执行 ffprobe (%s): %s\n", probe, strerror(errno));
        return -1;
    }

    int track_count = 0;
    printf("轨道列表:\n");
    const char* p = outbuf;
    const char* buf_end = outbuf + out_len;
    while (p < buf_end) {
        const char* nl = p;
        while (nl < buf_end && *nl != '\n') nl++;
        size_t len = (size_t)(nl - p);
        /* 去掉换行 */
        while (len > 0 && (p[len-1] == '\r' || p[len-1] == '\n')) len--;
        if (len > 0) {
            char line[512];
            if (len >= sizeof(line)) len = sizeof(line) - 1;
            memcpy(line, p, len);
            line[len] = '\0';
            printf("  %s\n", line);
            track_count++;
        }
        p = nl + 1;
    }
#endif

    if (track_count == 0) {
        printf("  (无轨道信息 — 视频文件可能损坏或 ffprobe 不可用)\n");
    }

    printf("总轨道数: %d\n", track_count);
    if (track_count > 0) {
        int has_sub = media_probe_subtitle(mr, filepath);
        printf("字幕轨道: %s\n",
               has_sub == 1 ? "有 ✓" :
               has_sub == 0 ? "无 (视频可能使用硬字幕/烧录字幕)" :
               "检测失败");
    }

    printf("================================\n");
    return track_count;
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
    if (has_sub < 0) {
        fprintf(stderr, "[media] ffprobe 错误 — 请确保 ffmpeg/ffprobe 已安装并在 PATH 中\n");
        fprintf(stderr, "[media] 下载: https://ffmpeg.org/download.html\n");
        fprintf(stderr, "[media] 提示: 运行 media_diagnose_tracks() 诊断具体问题\n");
        return -1;
    }
    if (has_sub == 0) {
        fprintf(stderr, "[media] 视频无软字幕轨道: %s\n", filepath);
        fprintf(stderr, "[media] 提示: 硬字幕/烧录字幕需要 Phase 2 OCR 或 Phase 3 Whisper ASR\n");
        return 0;
    }

    /* 提取字幕 */
    char* srt_text = NULL;
    size_t srt_len = 0;
    if (media_extract_subtitle(mr, filepath, &srt_text, &srt_len) < 0) {
        fprintf(stderr, "[media] 字幕提取失败: %s\n", filepath);
        return -1;
    }

    if (!srt_text || srt_len == 0) {
        fprintf(stderr, "[media] 字幕内容为空: %s\n", filepath);
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

    /* 配置 ffmpeg / ffprobe */
    if (cfg && cfg->ffmpeg_path[0]) {
        strncpy(mr->ffmpeg_path, cfg->ffmpeg_path, sizeof(mr->ffmpeg_path) - 1);
    } else {
        strncpy(mr->ffmpeg_path, "ffmpeg", sizeof(mr->ffmpeg_path) - 1);
    }
    if (cfg && cfg->ffprobe_path[0]) {
        strncpy(mr->ffprobe_path, cfg->ffprobe_path, sizeof(mr->ffprobe_path) - 1);
    } else {
        mr->ffprobe_path[0] = '\0';  /* 空 = 自动从 ffmpeg_path 推导 */
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
