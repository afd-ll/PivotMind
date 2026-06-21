# 0.0.1 — 字符串安全 + 原子操作 + UTF-8 编码统一

> **日期**: 2026-05-25 | **类型**: 修复

## 概述

全面修复 src/ 目录的安全隐患：23 处 strcpy/strcat 缓冲区溢出风险、thread_pool.c volatile 竞态条件、54 个 .c 文件编码混杂问题。

## 修复内容

| 文件 | 改动 |
|------|------|
| `src/dialog_system.c` | 17 处 strcat → strncat（4096 字节边界） |
| `src/chinese.c` | 2 处 strcpy → strncpy（len+1 边界） |
| `src/vocab.c` | 3 处 strcpy → strncpy（max_len/sizeof 边界） |
| `src/causal_reasoning.c` | 1 处 strcpy → strncpy（sizeof(buffer) 边界） |
| `src/thread_pool.c` | `volatile int` → `atomic_int`（running/shutdown） |
| `src/*.c`（54 个） | 编码统一为 UTF-8 |
| `tests/test_runner.c` | `prev_failed` → `g_prev_failed` 静态变量 |
| `Makefile` | `-D_USE_MATH_DEFINES` 统一至 CFLAGS |
