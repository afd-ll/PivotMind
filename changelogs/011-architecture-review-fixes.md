# 0.0.12 — 架构审查全量修复

> **日期**: 2026-05-27 | **类型**: 修复

## 概述

全项目架构审查 + 性能热路径 + 代码整洁。完成 P0-P2 共 10 项修复。

## 修复内容

| 优先级 | 改动 |
|--------|------|
| **P0** | 语义投票从 O(cross_link_count) 线性扫描改为 O(1) cross_adj 邻接表 |
| P1-4 | pruning.c 冒泡排序→快速选择（quickselect_float，四处替换） |
| P1-3 | 递归锁改普通锁（验证无重入调用链） |
| P1-2 | calc_context_activations O(n²)→O(n log n)（qsort+bsearch） |
| P1-1 | 激活传播加 active_set 过滤（只传播 activation≥0.15 节点） |
| P2-2 | 意图向量初始权重区分（词汇=0.20，语义=0.18 等） |
| P2-1 | 拓扑边剪枝机制：`trace_wisdom_net_prune_edges` + `master_prune_cross_links` |
| P2-3 | global_visited 跨拓扑语义验证（无实际问题） |
| P3 | 备份文件清理 + 跨平台注释修复 + parallel_mode 死代码标记 |

✅ 编译通过，0 error 0 warning。
