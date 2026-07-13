# 0.5.5 — 语义约束管线 + 英文支持

> **日期**: 2026-07-13 | **类型**: 新增

## 概述

本轮核心目标：让走边算法从"随机漫步"变成"语义约束的定向生成"。同时打通英文对话管线。

## 核心变更

### 条件概率组合节点 (`dialog_generate.c`)

- 新增 `CompoundTracker`：统计相邻字符对的条件概率 P(B|A)
- 共现 ≥ 10 次 + 条件概率 ≥ 50% → 自动创建组合节点（"苹"+"果"→"苹果"）
- 组合节点继承源节点出边（共享边取均值，独有边 ×0.7）
- 修复 `contains_punctuation` 老 bug：原实现逐字节检查，CJK 字符的 UTF-8 字节命中标点字节→全被误判为标点→`auto_learn_concepts` 永远 `cjk_count=0`

### 拓扑驱动自举分词 (`dialog_generate.c`)

- 锚点提取时扫描相邻单字对，存在组合节点则合并为一个 token
- 走边时 "苹果" 作为整体匹配，而非 "苹"→"果" 分开走

### 两跳激活扩散 (`diffusion.c`)

- 直接匹配节点 → 一跳邻居 (λ=1.0) → 两跳邻居 (λ=0.4)
- Jaccard 邻接相似度激活重加权：被激活节点与输入锚点集共享邻居越多，激活乘数越高 (0.5x~1.5x)
- 解决 512 维随机特征向量余弦 ≈ 0 的问题——不靠特征向量，直接用拓扑结构衡量语义关联

### 语义场休止 (`multi_topology.c`)

- 走边评分 `node_act` 权重 0.18→0.35，语义约束主导
- 新增硬关：候选节点 `activation < 0.05` 时停止走边——语义场外的节点不再被选中
- 替代之前的固定长度截断

### 英文支持 (`broca.c`)

- Broca 输出包裹器新增英文词间自动空格插入
- 连续 ASCII 词间检测前一个输出字符，末端是 ASCII 则插空格
- 英文 PMI 建边天然适配（空格分词），无需改动拓扑架构

### 语义拓扑自动生长 (`semantic_growth.c/h`, `brainstem.c`)

- 新增管线：词汇拓扑特征向量 → 余弦相似度聚类 → 语义聚类节点 → 跨拓扑链接
- 接入脑干 tick（每 5 tick 触发一次）
- 当前特征向量为 512 维随机初始化，聚类阈值 0.02，待 Hebbian 训练积累后自然产出语义节点

## 效果

| 输入 | 之前 | 现在 |
|---|---|---|
| 苹果是什么颜色 | 作变去久林天地白 | 红色水果甜苹…（首词命中"红色"） |
| pride | (无英文支持) | lively allowance telling… ✅ |
| love | — | nothing . ✅ |
| darcy | — | It grunted a Lobster. ✅ |

## 改动文件

- `src/dialog_generate.c` — 条件概率组合节点 + 自举分词 + `contains_punctuation` 修复
- `src/diffusion.c` — 两跳激活扩散 + Jaccard 重加权
- `src/multi_topology.c` — `node_act` 权重提升 + 语义场休止
- `src/broca.c` — 英文词间自动空格
- `src/semantic_growth.c` — 语义拓扑自动生长（新增）
- `include/semantic_growth.h` — 语义生长头文件（新增）
- `src/brainstem.c` — 语义生长 hook
