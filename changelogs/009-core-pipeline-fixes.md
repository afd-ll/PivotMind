# 0.0.10 — 核心管线修复

> **日期**: 2026-05-26 | **类型**: 修复

## 概述

对核心管线 7 个关键文件进行深度审查与修复。

## 修复清单

### P0 — 逻辑缺陷
- **feedback_correct 增加边置信度压制**：ActiveLearner 新增 `last_path_*` 字段，负面反馈时 `conf *= 0.6`
- **dialog_generate global_visited 分配时机修复**：从因果筛选后移到走边循环前
- **autonomic_decay_all 竞争衰减**：三档差异化衰减（赢家几乎不衰减/输家加速衰退/中间正常）

### P1 — 重要修复
- **evaluate_draft drive_score 改为路径效价**：从全局平均改为当前路径节点 `fabsf(valence)` 均值
- **causal_path_score O(1) 反向映射**：循环内查找 O(n)→O(1)
- **learn_from_dialog 精确定位**：`last_path_*` 优先，strstr 全扫描降为兜底
- **dialog_generate 增加内感受评估门**：因果筛选后 `evaluate_draft` 检查

### P2 — 改善
- **boost_connection 字序编码**：相邻字对 weight_mult=1.5，非相邻 1.5/dist
- **删除 dialog_system.c 重复的 dialog_generate**（~190 行死代码）
- **魔数集中到 constants.h**：8 个新常量
- **reader.c 废弃标记**

### 新增模块
- `BpttLearner`：RNN(24→64)→Linear(64→24)，Adam 优化器，与 autonomic_learner 互补
- `autonomic_learn_from_text`：读文本→分句→逐对学习，支持最大 10000 句
