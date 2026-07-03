# 055 — 多模态管线 v0.5.0

## 版本
`0.5.0`

## 背景
玄枢长期以来只有纯文本输入能力（4 种语料格式：JSON QA、管道 QA、纯文本、ARTICLE）。
对话生成质量受限于单一模态——书籍语料缺乏对话轮次，无法建立"词→视觉实体"的语义锚定。

多模态管线 v0.5 引入两条新数据管线：
1. **字幕管道（MediaReader）** — 从视频中提取 SRT 字幕，经 PMI 词发现创建词汇拓扑节点
2. **视觉皮层脑区（VisualCortex）** — 帧提取 + 时间窗对齐 + 跨模态边，建立"苹果（词）↔ 苹果_视觉"锚定

## 新增脑区

| 脑区 | 枚举值 | 文件 | 行数 |
|------|--------|------|------|
| **视觉皮层 (VisualCortex)** | THAL_VISUAL_CORTEX = 9 | `src/visual_cortex.c` | ~550 |

### 脑区架构

```
网关 POST /media/feed
  → visual_cortex_enqueue()        (生产者: 入队到环形缓冲128)
  → 脑干 tick (每1s)
    → brainstem_tick_perception
      → thalamus_get_region(THAL_VISUAL_CORTEX)
        → visual_cortex_tick(throttle)   (消费者: 丘脑门控)
          → 出队 1 文件 → 帧提取 + SRT字幕 + 时间对齐 + 跨拓扑边
          → THAL_SIG_VISUAL_FRAME / THAL_SIG_CROSS_MODAL_EDGE
```

### 任务队列特性
- **环形缓冲区**: 容量 128，生产者(网关) → 消费者(脑干)
- **背压**: 队列满时 enqueue 返回 -1，避免无限堆积
- **门控**: throttle < 0.1 自动跳过（丘脑认为系统忙）
- **限速**: max_batch_per_tick = 1（每次 tick 只处理一个视频）
- **冷却**: 队列空后 idle_cooldown=60 ticks，减少空转

## 新增工具组件

| 组件 | 文件 | 行数 | 职责 |
|------|------|------|------|
| **媒体阅读器 (MediaReader)** | `src/media_reader.c` | ~400 | ffmpeg SRT 字幕提取 + PMI 词发现管道 |

MediaReader 为 VisualCortex 的内部组件，负责：
- `ffprobe` 检测字幕轨道
- `ffmpeg` 提取 SRT/VTT/ASS 字幕
- SRT 时间戳解析 + HTML 标签清理
- 逐行喂入 `article_process_line()` → PMI 词发现
- 定期 `article_flush()` 触发拓扑建边

## 新增拓扑

| 枚举值 | 拓扑名称 | 节点命名 | 特征向量 |
|--------|---------|---------|---------|
| TOPO_VISUAL = 11 | 视觉拓扑 | `{词}_视觉` | 512-dim (当前零向量占位, CLIP 预留) |

### 跨拓扑连接机制

```
词汇拓扑 "苹果"  ←─ visual_anchor ─→  视觉拓扑 "苹果_视觉"
                                  (基于时间窗共现计数)
```

对齐算法:
1. 字幕分词 → 每个词取中点时间戳
2. 时间窗 [t - 2000ms, t + 2000ms] 内统计共现帧数
3. 共现计数 ≥ min_cooccurrence(2) → 调用 `master_add_cross_link()`
4. 双向边 + `visual_anchor` 关系类型
5. 自动查重（`cross_link_exists`），避免重复建边

## 新增节点类型

| 枚举值 | 类型名 | 用途 |
|--------|--------|------|
| NODE_TYPE_VISUAL = 3 | 视觉概念节点 | 携带 CLIP 编码器的视觉特征向量 |

## 新增丘脑信号

| 信号 | 含义 | 发送时机 |
|------|------|---------|
| `THAL_SIG_VISUAL_FRAME` | 视觉帧处理完成 | visual_cortex_tick 每批处理后 |
| `THAL_SIG_CROSS_MODAL_EDGE` | 跨模态边建立 | 每建一条 vocabs↔visual 边 |
| `THAL_SIG_MEDIA_FILE_DONE` | 媒体文件处理完毕 | media_process_file 返回后 |

## 网关 API

### POST /media/feed
```json
{
  "path": "/data/cartoons/babybus_01.mp4",
  "mode": "visual"          // "visual" | "subtitle"
}
// → {"result":"enqueued","mode":"visual","enqueued":1,"queue_size":1}
```

支持目录批量:
```json
{"path": "/data/cartoons/", "recursive": "1", "mode": "visual"}
```

### GET /media/status
```json
{
  "queue_size": 3,
  "media_reader": {"files": 12, "lines": 3400, "words": 892},
  "visual_cortex": {"frames": 480, "visual_nodes": 56, "cross_modal_edges": 83, "topo_visual_nodes": 56}
}
```

## 运行时配置（pivotmind_config.json）

```json
{
  "brain_regions": {
    "visual_cortex": true    // 可运行时关闭视觉皮层脑区
  }
}
```

## 枚举变更

| 枚举 | 旧值 | 新值 |
|------|------|------|
| `ThalamusSubsystem` THAL_SUBSYSTEM_COUNT | 9 | 10 |
| `TopologyType` 最大值 | TOPO_TEMPLATE (10) | TOPO_VISUAL (11) |
| `NodeType` 最大值 | NODE_TYPE_PROPER_NOUN (2) | NODE_TYPE_VISUAL (3) |
| `BrainRegionConfig` 字段数 | 8 | 9 (+visual_cortex) |

## 修改文件清单

| 文件 | 类别 | 改动 |
|------|------|------|
| `include/multi_topology.h` | 枚举 | +TOPO_VISUAL = 11 |
| `include/huarong_topology.h` | 枚举 | +NODE_TYPE_VISUAL = 3 |
| `include/thalamus.h` | 枚举/信号 | +THAL_VISUAL_CORTEX, +3 信号, SUBSYSTEM_COUNT→10 |
| `include/json_config.h` | 配置 | BrainRegionConfig +visual_cortex |
| `include/media_reader.h` | **新增** | MediaReader API (105 行) |
| `include/visual_cortex.h` | **新增** | VisualCortex 脑区 API (170 行) |
| `src/media_reader.c` | **新增** | SRT 提取 + PMI 管道 (400 行) |
| `src/visual_cortex.c` | **新增** | 脑区实现 + 任务队列 + 对齐 (550 行) |
| `src/multi_topology.c` | 数组 | TOPOLOGY_TYPE_NAMES +"视觉拓扑" |
| `src/brainstem.c` | 调度 | +visual_cortex_tick 调度 |
| `src/json_config.c` | 配置 | +visual_cortex 初始化/解析/模板 |
| `demos/pivotmind_gateway.c` | 网关 | 脑区注册 + /media/feed + /media/status |
| `tests/unit/test_media_reader.c` | **新增** | 16 个测试用例 |
| `tests/unit/test_visual_cortex.c` | **新增** | 17 个测试用例 |

## Phase 2-4 路线图

| Phase | 内容 | 状态 |
|-------|------|------|
| Phase 1 | 帧提取 + 零向量占位 + 跨模态对齐 + 建边 | ✅ 完成 |
| Phase 2 | CLIP ViT-B/32 编码器集成 (512-dim ↔ PM_NODE_FEATURE_DIM) | 编码器接口已预留 |
| Phase 3 | Whisper ASR 后备 (无字幕视频) | 待实现 |
| Phase 4 | 视觉节点聚类 (同类物体多视角 → 统一视觉概念) | 待实现 |
