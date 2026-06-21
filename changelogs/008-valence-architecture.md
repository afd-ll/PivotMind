# 0.0.9 — 效价-因果-内感受三层分离

> **日期**: 2026-05-26 | **类型**: 重构

## 概述

将效价、因果、内感受从"并列加权"重构为"上游→中游→下游"三层流水线。核心原则：
- **效价 = 内驱力**（上游，决定"想要什么"）
- **因果 = 逻辑约束**（中游，确保"想得合理"）
- **内感受 = 效价检验**（下游，判断结果是否满足内驱力）

## 修复内容

| 文件 | 改动 |
|------|------|
| `src/multi_topology.c` | `topology_walk_greedy` 加入 `context_valence` 情感基调 EMA 约束 |
| `src/cognitive_controller.c` | `causal_consistency_check` → `causal_path_score`（移到中游）|
| `src/cognitive_controller.c` | `evaluate_draft` 五维→三维：质量40%+连贯30%+效价30%-矛盾扣分 |
| `include/cognitive_controller.h` | 新增 `causal_path_score` API，加 `causal_graph`/`concept_hierarchy` 字段 |
| `src/dialog_generate.c` | walk 循环中加入 `causal_path_score < 0.25` 筛选 |
| `src/dialog_system.c` | 注入因果图和概念层次 |

同时撤回了之前引入的 Seq2Seq 外部模型方案。
