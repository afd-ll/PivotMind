# 0.0.5 — 编译错误修复

> **日期**: 2026-05-26 | **类型**: 修复

## 概述

全项目编译错误扫描与修复。

## 修复内容

| 文件 | 修复 |
|------|------|
| `src/cross_edge_io.c` | 补 `typedef struct TopoPairConfig` 声明头，删重复 `to_type` 字段 |
| `src/tensor.c` | `atomic_store` 语法修复 |
| `include/layer.h` | 添加 `xavier_init`/`glorot_init` 声明 |
| `src/layer.c` | 添加 Xavier uniform 初始化实现 + glorot 别名 |
| `src/attention.c` | 添加 `#include "layer.h"` |

✅ 0 error 0 warning。
