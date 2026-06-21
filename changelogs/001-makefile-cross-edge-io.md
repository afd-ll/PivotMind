# 0.0.2 — Makefile 编译标准 + cross_edge_io 恢复

> **日期**: 2026-05-25 | **类型**: 修复

## 概述

Makefile 从 c11 退回 gnu99（项目依赖 POSIX/GNU 扩展），恢复被误修改的 cross_edge_io.c。

## 修复内容

| 文件 | 改动 |
|------|------|
| `Makefile` | `-std=c11` → `-std=gnu99`（CFLAGS + DEBUG_CFLAGS） |
| `src/cross_edge_io.c` | 从 .bak 恢复，修复 include 路径和结构体损坏 |
