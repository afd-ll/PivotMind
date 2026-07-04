/**
 * @file media_reader.h
 * @brief 媒体阅读器 — SRT 字幕提取 + 文本管道输送到拓扑网络
 *
 * 原理：
 *   视频/音频文件 → ffmpeg 提取字幕轨道 (SRT/VTT/ASS) → 解析文本
 *   → 时间顺序拼接为句子 → 逐句喂入 ArticleReader PMI 管道
 *   → PMI 词发现 → 新建词汇节点 + 相邻词建边 → 拓扑网络增长
 *
 * 支持格式：
 *   - SRT (SubRip): ffmpeg 自动提取软字幕
 *   - VTT (WebVTT): ffmpeg 自动转换
 *   - ASS/SSA: ffmpeg 自动提取
 *   - 无字幕视频: 报错返回 (ASR 语音转写由视觉皮层模块提供)
 *
 * 设计理念：
 *   早教片/动画片的字幕天然包含大量"问→答"对话轮次，
 *   且语言简洁、重复性高，是结构化对话语料的理想来源。
 *   不做角色分离，纯文本字幕直接流入现有 PMI 管道，
 *   依靠共现统计自然涌现词语边界。
 */

#ifndef MEDIA_READER_H
#define MEDIA_READER_H

#include "multi_topology.h"
#include "article_reader.h"

/* Forward declaration for optional thalamus signal bus binding */
typedef struct Thalamus Thalamus;

// ==================== 配置 ====================

typedef struct {
    /* ffmpeg 可执行文件路径 (用于转码/提取) */
    char  ffmpeg_path[256];

    /* ffprobe 可执行文件路径 (用于流检测; 空字符串 = 自动从 ffmpeg_path 推导) */
    char  ffprobe_path[256];

    /* 字幕轨道号 (-1 = 自动选择第一个字幕轨) */
    int   subtitle_track;

    /* 每处理 N 行文本后触发一次词发现 flush (0 = 使用 article_reader 默认 200) */
    int   flush_interval;

    /* 是否递归处理子目录 */
    int   recursive;

    /* 详细输出 */
    int   verbose;
} MediaReaderConfig;

#define MEDIA_READER_DEFAULT_CONFIG { \
    "ffmpeg", "", -1, 0, 0, 0 \
}

// ==================== 公共 API ====================

typedef struct MediaReader MediaReader;

/**
 * 创建媒体阅读器
 * @param topology  主拓扑（用于注入 article_process_line 管道）
 * @param cfg       配置（NULL = 使用默认值）
 * @return          媒体阅读器实例，失败返回 NULL
 */
MediaReader*  media_reader_create(MasterTopology* topology,
                                  const MediaReaderConfig* cfg);

/** 销毁媒体阅读器 */
void media_reader_destroy(MediaReader* mr);

/**
 * 处理单个视频/音频文件
 * 1. ffprobe 检测字幕轨道
 * 2. ffmpeg 提取字幕文本
 * 3. 解析 SRT 时间轴，按时间顺序拼接文本行
 * 4. 逐行喂入 article_process_line()
 * 5. 每 flush_interval 行自动触发 article_flush()
 *
 * @param mr        媒体阅读器实例
 * @param filepath  视频文件路径 (支持 mp4/mkv/avi/webm/flv/mov)
 * @return          提取并喂入的行数, -1 为出错, 0 为无字幕
 */
int media_process_file(MediaReader* mr, const char* filepath);

/**
 * 处理整个目录下的媒体文件
 * @param mr        媒体阅读器实例
 * @param dirpath   目录路径
 * @param extensions 逗号分隔的扩展名过滤器 (如 "mp4,mkv,avi" 或 NULL=全部)
 * @return          成功处理的文件数, -1 为目录遍历失败
 */
int media_process_directory(MediaReader* mr, const char* dirpath,
                            const char* extensions);

/**
 * 获取统计信息
 * @param mr
 * @param out_files       输出: 累计处理的媒体文件数
 * @param out_lines       输出: 累计提取的文本行数
 * @param out_words       输出: 已发现的词数
 */
void media_reader_get_stats(MediaReader* mr,
                            long* out_files,
                            long* out_lines,
                            long* out_words);

/**
 * 绑定丘脑信号总线（可选）
 * 绑定后每次处理完文件自动通过丘脑发送 THAL_SIG_MEDIA_FILE_DONE 信号
 */
void media_reader_set_thalamus(MediaReader* mr, Thalamus* th);

/**
 * 诊断: 列出视频文件中所有可用的轨道信息 (v0.5.1)
 * 打印视频/音频/字幕轨道到 stdout，帮助用户确认媒体文件是否包含字幕。
 *
 * @param mr        媒体阅读器
 * @param filepath  视频文件路径
 * @return          找到的轨道总数, -1 为出错
 */
int media_diagnose_tracks(MediaReader* mr, const char* filepath);

#endif /* MEDIA_READER_H */
