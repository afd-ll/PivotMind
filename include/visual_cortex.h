/**
 * @file visual_cortex.h
 * @brief 视觉皮层脑区 — 多模态感知引擎 (v0.5)
 *
 * 大脑类比：
 *   视觉皮层(V1-V5/IT)接收来自丘脑外侧膝状体的视觉信号，
 *   提取层级特征，与角回协作完成跨模态语义锚定。
 *
 * 系统映射：
 *   - 脑区地位：与海马体/杏仁核平级，由脑干调度 + 丘脑门控
 *   - 内部管线：MediaReader(SRT字幕) + ffmpeg帧提取 + 跨模态对齐
 *   - 子拓扑：TOPO_VISUAL = 11 (视觉概念节点)
 *   - 归属：THAL_PERCEPTION 感觉系统群
 *
 * 处理流程:
 *   网关 POST /media/feed → 入队 task_queue
 *   → 脑干 tick → visual_cortex_tick(throttle)
 *     → 逐个出队处理: 帧提取 + SRT字幕 + 时间对齐 + 跨拓扑边
 *     → THAL_SIG_VISUAL_FRAME / THAL_SIG_CROSS_MODAL_EDGE
 *
 * 阶段：
 *   Phase 1 (已实现): 帧提取 + 零向量占位 + 跨模态对齐 + 建边
 *   Phase 2 (预留): CLIP ViT-B/32 编码器集成 (512-dim)
 *   Phase 3 (预留): Whisper ASR 后备 (无字幕视频)
 */

#ifndef VISUAL_CORTEX_H
#define VISUAL_CORTEX_H

#include "multi_topology.h"
#include "article_reader.h"
#include "media_reader.h"

/* Forward declaration for thalamus */
typedef struct Thalamus Thalamus;

// ==================== 配置 ====================

typedef struct {
    /* ── 帧提取 ── */
    char  ffmpeg_path[256];
    char  ffprobe_path[256];         /* ffprobe 路径 (空=自动从 ffmpeg_path 推导) */
    int   frame_interval_ms;         /* 帧采样间隔 (默认 500ms) */
    int   keyframe_only;             /* 1=仅关键帧, 0=均匀间隔采样 */
    int   scene_threshold;           /* 场景切换检测阈值 (0=关闭, 建议 30) */
    char  frame_output_dir[512];     /* 帧图片输出目录 (""=临时) */
    int   max_frames_per_video;      /* 单视频最大帧数 (0=无限制) */

    /* ── 视觉编码器 ── */
    int   feature_dim;               /* 输出特征维度 (默认 512) */
    int   use_clip_encoder;          /* 1=CLIP编码器 (Phase 2), 0=零向量占位 */
    char  clip_model_path[512];      /* CLIP 模型路径 (Phase 2) */

    /* ── 跨模态对齐 ── */
    float alignment_window_ms;       /* 时间对齐窗口 (默认 2000ms) */
    int   min_cooccurrence;          /* 最少共现次数才建边 (默认 2) */
    float visual_edge_weight;        /* 跨拓扑边初始权重 (默认 0.6) */

    /* ── 视觉拓扑 ── */
    int   visual_topo_capacity;      /* 视觉拓扑初始容量 (默认 5000) */

    /* ── 脑区调度 ── */
    int   max_batch_per_tick;        /* 每 tick 最多处理几个文件 (默认 1) */
    int   idle_cooldown_ticks;       /* 队列空后等多少 tick 再检查 (默认 60) */

    /* ── 输出 ── */
    int   verbose;
} VisualCortexConfig;

#define VISUAL_CORTEX_DEFAULT_CONFIG { \
    "ffmpeg", "", 500, 1, 30, "", 200, \
    512, 0, "", \
    2000.0f, 2, 0.6f, \
    5000, 1, 60, 0 \
}

// ==================== 视觉帧数据结构 ====================

typedef struct {
    int   frame_index;
    float timestamp_ms;
    int   is_keyframe;
    int   scene_change;
    float* features;
    int   feature_dim;
    char  frame_file[512];
} VisualFrame;

// ==================== 公共 API ====================

typedef struct VisualCortex VisualCortex;

/**
 * 创建视觉皮层脑区
 * @param topology  主拓扑（自动创建 TOPO_VISUAL）
 * @param cfg       配置（NULL = 默认）
 * @return          视觉皮层实例
 */
VisualCortex* visual_cortex_create(MasterTopology* topology,
                                   const VisualCortexConfig* cfg);

/** 销毁视觉皮层 */
void visual_cortex_destroy(VisualCortex* vc);

/**
 * 脑区 tick — 由脑干周期性调用（替代 void* 指针调用链）
 *
 * 内部流程:
 *   1. 检查 throttle: 低于 0.1 直接跳过
 *   2. 检查任务队列: 如空则递减 cooldown 计数器
 *   3. 从队列取出文件 → 帧提取 + 字幕 + 对齐 + 建边
 *   4. 通过丘脑发送 THAL_SIG_VISUAL_FRAME / THAL_SIG_CROSS_MODAL_EDGE
 *
 * @param vc       视觉皮层实例
 * @param throttle 丘脑分配的时间片 (0.0~1.0)
 * @return         本次实际处理的文件数 (0=跳过或空闲)
 */
int visual_cortex_tick(VisualCortex* vc, float throttle);

/**
 * 入队一个媒体文件到处理队列
 * 由网关 /media/feed API 调用
 *
 * @param vc       视觉皮层实例
 * @param filepath 视频文件路径
 * @param mode     "subtitle"=仅字幕, "visual"=帧+对齐
 * @return         0=成功入队, -1=队列满或出错
 */
int visual_cortex_enqueue(VisualCortex* vc, const char* filepath, const char* mode);

/**
 * 批量入队目录
 * @return 成功入队的文件数
 */
int visual_cortex_enqueue_directory(VisualCortex* vc, const char* dirpath,
                                    const char* extensions, int recursive);

/** 获取队列中待处理的文件数 */
int visual_cortex_queue_size(VisualCortex* vc);

/**
 * 获取统计信息
 * @param vc
 * @param out_queue     输出: 队列待处理数
 * @param out_frames    输出: 累计帧数
 * @param out_vis_nodes 输出: 视觉节点数
 * @param out_xmod_edges 输出: 跨模态边数
 */
void visual_cortex_get_stats(VisualCortex* vc,
                             int*  out_queue,
                             long* out_frames,
                             int*  out_vis_nodes,
                             int*  out_xmod_edges);

/**
 * 设置 CLIP 编码器回调（Phase 2 集成）
 */
typedef int (*VisualFeatureEncoder)(const char* image_path, int feature_dim,
                                    float* out_features, void* ctx);
void visual_cortex_set_encoder(VisualCortex* vc,
                               VisualFeatureEncoder encode_fn, void* ctx);

/**
 * 获取内部 MediaReader 引用（用于运行时统计查询）
 */
MediaReader* visual_cortex_get_media_reader(VisualCortex* vc);

#endif /* VISUAL_CORTEX_H */
