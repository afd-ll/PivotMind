# 0.0.15 — 编码损坏 + 代码整洁

> **日期**: 2026-05-27 | **类型**: 修复

## 概述

014 修复后的深度审查，发现换行符编码损坏等 3 项问题。

## 修复内容

| 优先级 | 文件 | 改动 |
|--------|------|------|
| **P0** | `src/autonomic_learner.c` | 修复 `extract_ordered_chars` 中 `\t`、`\n`、`\r` 转义序列被展开为实际控制字符的问题 |
| P1 | `src/feature_learn.c` | 删除未使用变量 `src_id` |
| P2 | `src/multi_topology.c` | Beam 候选数组加边界检查（保留 1 槽防溢出） |

✅ 0 error 0 warning。
