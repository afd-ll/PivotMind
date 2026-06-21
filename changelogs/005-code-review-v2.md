# 0.0.6 — 代码审查 v2 修复

> **日期**: 2026-05-26 | **类型**: 修复

## 概述

第二轮全项目代码审查，修复多拓扑架构深层问题。

## 修复内容

| 文件 | 问题 | 改动 |
|------|------|------|
| `src/multi_topology.c` | 节点区/跨链接区无分隔符 | save: 加 sentinel + cross_link_count；load: 验证 |
| `src/tensor.c` | 广播运算未实现 | 新增 `broadcast_index()`，四运算全部实现 |
| `src/multi_topology.c` | 魔数 `20` 硬编码 | → `master->sub_topo_count` |
| `tools/batch_learn.c` | 调试清零 | 改为自动 rebuild + 警告 |

**架构阻塞（未修复）**：GRU 反向传播缺 x_t 缓存、LSTM BPTT 缺多时间步状态缓存、cognitive_controller 无 GRU/LSTM 接口。
