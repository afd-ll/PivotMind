# 020 — 打通四条神经回路

> 日期: 2026-05-30 | 修复人: CodeWhale

## 背景

四个计算法（valence、cognitive_confidence、activation、置信度）已经存在，
但互相之间的回路没通。不需要加新概念，把已有的焊上。

## 回路 1：效价回流（多巴胺标记）

- **文件**: `src/cognitive_controller.c` → `evaluate_draft`
- **机制**: 满意度回写到路径节点的 `valence`（±0.025/次评估）
  - satisfaction > 0.5 → 正标记，下次走边更容易选中
  - satisfaction < 0.5 → 负标记，下次走边被抑制
- **效果**: 系统对自己走过的路做情绪标记，类似多巴胺

## 回路 2：激活竞争决定学习权

- **文件**: `src/autonomic_learner.c` → 核心4：跨拓扑传播
- **机制**: `recent_activation < 0.15` 的拓扑跳过本轮赫布建边
  - 情绪拓扑只在有情绪色彩的对话里长边
  - 语义拓扑只在语义激活高的对话里长边
  - 词汇拓扑始终参与（基础层）
- **效果**: 消除四个拓扑的边结构同质化

## 回路 3：cognitive_confidence EMA 更新

- **文件**: `src/cognitive_controller.c` → `evaluate_draft`
- **机制**: 每次评估后对路径节点的三维置信度做 EMA 更新（90%旧+10%新）
  - `predictive_accuracy`: 满意→0.9，不满意→0.3
  - `user_satisfaction`: 直接用 satisfaction 值
  - `novelty_bonus`: selection_count < 5 → 0.8，否则 → 0.4
- **效果**: 置信度不再是永远 0.5，会随使用反馈演化

## 回路 4：模板候选增量生灭

- **文件**: `src/template_builder.c` → `template_auto_build`
- **机制**: 移除幂等守卫（`if node_count > 0 return 0`）
  - 模板拓扑可以随频率表增长而多次增量构建
  - 模板节点靠 `confidence` 自然生灭，不再是一次性批量聚类
- **效果**: 模板拓扑成为活的、持续生长的认知层

## 设计约束

- 零梯度、零全局优化、零矩阵运算
- 所有学习都是局部的、基于激活的、在线发生的
- 只焊已有的计算法回路，不加新概念
