# 0.0.3 — dialog 游标 + tensor stride + concept visited 优化

> **日期**: 2026-05-25 | **类型**: 优化

## 概述

对话系统和张量运算的热路径优化：消除重复 strlen、修正 stride 计算、visited 数组动态分配。

## 修复内容

| 文件 | 改动 |
|------|------|
| `src/dialog_system.c` | `strncat` → `snprintf+pos` 游标，消除重复 `strlen()` |
| `src/tensor.c` | `broadcast_to` 预计算 stride，O(n×ndim) → O(n) |
| `src/concept_abstraction.c` | `visited[256]` → `malloc(hierarchy->capacity)`，环形检测动态分配 |
