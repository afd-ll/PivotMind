# 0.0.8 — 语义-因果双重约束输出

> **日期**: 2026-05-26 | **类型**: 重构

## 概述

摒弃 Seq2Seq 外部模型方案，改用语义拓扑 + 因果拓扑内生约束。全在拓扑体系内闭环。

## 改动

### 撤回（删除 Seq2Seq）
- 删除 `include/seq2seq.h`、`src/seq2seq.c`
- `dialog_system.h/c`、`dialog_generate.c` 移除所有 Seq2Seq 引用

### 新增
- `src/cognitive_controller.c`：`causal_consistency_check()` — 路径相邻概念对→因果图查询→一致性评分
- `self_contradiction_check()` — 情绪效价互斥检测 + 概念层次跳跃检测
- `evaluate_draft` 升级 v2：五维评分（效价20% + 连贯25% + 激活20% + 因果25% + 语义10%）

### 增强
- `CognitiveController` 新增 `causal_graph` 和 `concept_hierarchy` 字段
- `dialog_system_create` 注入因果图和概念层次
- `topology_walk_greedy` 语义权重动态增强

**架构**：语义拓扑管"往哪走"，因果拓扑管"对不对"。无外部模型。
