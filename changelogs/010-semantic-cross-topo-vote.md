# 0.0.11 — 语义拓扑跨拓扑投票接入

> **日期**: 2026-05-27 | **类型**: 新增

## 概述

`topology_walk_greedy()` 的路径回溯只在词汇拓扑内部查边，不参考语义拓扑的连接。本次接入语义拓扑作为额外投票源。

## 修复内容

| 文件 | 改动 |
|------|------|
| `include/multi_topology.h` | 签名新增 `MasterTopology* master` 参数（NULL 退化为原行为） |
| `src/multi_topology.c` | 路径回溯增加语义跨拓扑投票段，遍历 `cross_links` 找 `TOPO_SEMANTIC` 连接 |
| 评分公式 | `path_ctx_norm * 0.6 + semantic_cross_norm * 0.4` |
| 三处调用点 | 全部更新传 `master` |
| 测试工具 | 传 `NULL` 保持原行为 |

✅ 编译通过，0 error 0 warning。
