# 0.5.3 — 模板构建诊断与语法种子注入

> **日期**: 2026-07-12 | **类型**: 新增/修复

## 概述

诊断对话质量根因：模板拓扑（语法拓扑）为空导致 Broca 无法组装句子。新增调试端点、聚类降级 fallback、语法种子自动注入机制。加速 broca 自动构建间隔。

## 核心变更

### 1. 模板构建管线诊断 (`template_builder.c`)

- 在 `template_auto_build` 各阶段增加 `fprintf(stderr, ...)` 诊断日志
- 发现瓶颈：余弦聚类相似度 < 0.7 导致 0 簇产出
- 新增无特征/低相似度时的**降级 direct-cluster fallback**，直接将每个三元组作为一个独立簇

### 2. 语法种子注入 (`broca.c`, `broca.h`)

- 新增 `broca_seed_grammar()` — 注入 8 条基础中文语法节点到模板拓扑
- 在 `broca_tick` 中自动触发：当自动构建无产出且模板节点 < 20 时，每 30 tick 尝试注入
- 语法种子涵盖：ADJ+NOUN:的、NOUN+ADJ、NOUN+VERB、VERB+ADJ:得、ADV+VERB:地、NOUN+NOUN:和、VERB+NOUN、ADJ+ADJ

### 3. 网关增强 (`pivotmind_gateway.c`)

- `GET /debug` — 暴露 vocab_nodes、freq_entries、total_nodes、template_nodes、sub_topos
- `GET /force_templates` — 手动触发模板构建（绕过 tick 调度）
- `handle_learn` 末尾自动注入语法种子（每 10 次 learn 检查一次）

### 4. Broca 构建加速

- `build_interval_ticks` 从 300 降至 10，加速自动语法发现节奏

## 改动文件

| 文件 | 变更 |
|------|------|
| `demos/pivotmind_gateway.c` | +42 行：/debug、/force_templates 端点，handle_learn 语法注入 |
| `src/template_builder.c` | +30 行：诊断日志，降级 direct-cluster fallback |
| `src/broca.c` | +75 行：broca_seed_grammar()，broca_tick 自动触发 |
| `include/broca.h` | +1 行：broca_seed_grammar 声明 |

## 诊断发现

- 模板构建四道关全部通过（vocab≥500 ✓, freq≥50 ✓, 不可分解分析 ✓, 三元组分组 ✓）
- 余弦聚类因特征向量相似度 < 0.7 全部产生 0 簇 → 修复后产出 10 簇
- `template_build_nodes` 在 ARM 上执行过慢（30K 节点 × 10 簇），阻塞 HTTP 响应
- 语法种子注入后 template_nodes 从 0 升至 10，Broca 进入 POS 匹配路径
- **对话质量未显著提升**：扩散引擎仅激活 2-3 词，ACC 评分常 < 阈值

## 编译验证

`make test` — 全部单元测试通过 ✅
