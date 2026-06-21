# 0.0.14 — 字序编码 + Beam Search + 特征学习

> **日期**: 2026-05-27 | **类型**: 新增

## 概述

修复"训练 20 轮后仍是乱码"的根因——`extract_unique_chars` 去重毁灭字序。同时实现 Beam Search 走边和图拉普拉斯平滑特征学习。

## 修复内容

| 优先级 | 改动 |
|--------|------|
| **P0** | `extract_unique_chars` → `extract_ordered_chars`（保序不去重），位置衰减 `wmult = 1.5f/dist` 现在作用在真实位置差上 |
| **P1** | `topology_walk_beam` 完整实现（Beam K=3，~215 行），全局最优路径选择 |
| **P2** | 图拉普拉斯平滑特征学习（`feature_learn.c` 97 行）：共现邻居加权平均，features 从"冻结随机数"升级为"编码共现关系的语义向量" |
| **P3** | 三元组链式奖励：走边时对 prev→current→target 链给 +0.05 奖励 |

### 文件
- `src/autonomic_learner.c`：P0 去重→保序 + P2 集成
- `include/feature_learn.h` + `src/feature_learn.c`：新建特征学习模块
- `include/multi_topology.h` + `src/multi_topology.c`：Beam Search + 三元组奖励

✅ 编译通过，0 error 0 warning。
