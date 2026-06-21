# 0.0.4 — 跨拓扑连接状态文件读写断裂修复

> **日期**: 2026-05-25 | **类型**: 修复

## 概述

`master_save_state` 写入跨连接无分隔标记，导致 `master_load_state` 将跨连接字节当作节点数据吞噬。后果：跨拓扑连接数=0，联想引擎限在词汇拓扑内。

## 修复内容

**`master_save_state`**：节点区写完后插入 `sentinel=-1` + `magic=0xDEADBEEF` + 实际连接数。

**`master_load_state`**：
- 节点循环中检测 sentinel（`topo_type == -1`）终止
- 跨连接循环前验证魔数，兼容旧格式回退
