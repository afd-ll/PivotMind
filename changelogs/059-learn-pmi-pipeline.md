# 0.5.4 — /learn 接入 PMI 词共现管线

> **日期**: 2026-07-12 | **类型**: 新增

## 概述

`/learn` 端点之前只做词汇拓扑节点创建（`_learn_tokens`），不建立词间关联边。本次改动将 `/learn` 接入 article_reader 的 PMI 管线，每次学习同时建立词共现频率数据，为自动学习者建边提供输入。

## 核心变更

### perception_feed_learn_text (`perception.c`, `perception.h`)

- 新增函数，将学习文本按句号分句后逐行喂入 `article_process_line`
- 每 5 次调用触发一次 `article_flush`，将字符对统计写入拓扑
- 与感知皮层的搜索学习管线共享同一个 article_reader 实例

### handle_learn 增强 (`pivotmind_gateway.c`)

- 在原有 `_learn_tokens`（词汇节点创建）之后，调用 `perception_feed_learn_text`
- 每次 `/learn` 同时执行：拆词存节点 + PMI 词共现统计

## 效果验证

- PMI 管线成功发现复合词：如"今天天气"被识别为单个词汇节点
- 词汇拓扑从 30,400 增长至 30,577+
- 扩散引擎激活词数从 1-2 提升至 4-5
- 对话输出从全"人很大。"变为多词变化（"大地好小子"等）

## 改动文件

| 文件 | 变更 |
|------|------|
| `demos/pivotmind_gateway.c` | +5 行：handle_learn 调用 perception_feed_learn_text |
| `src/perception.c` | +35 行：perception_feed_learn_text 实现 |
| `include/perception.h` | +6 行：函数声明 |
| `include/pivotmind_version.h` | 版本 0.5.2 → 0.5.4 |

## 编译验证

`make test` — 全部单元测试通过 ✅
