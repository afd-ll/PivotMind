# 002: Makefile 改回 gnu99 + cross_edge_io.c 恢复

> 日期: 2026-05-25 | 关联审查: 003A-code-review.md

## 修改概览

| 文件 | 操作 | 说明 |
|------|------|------|
| Makefile | 修改 | -std=c11 → -std=gnu99（CFLAGS + DEBUG_CFLAGS） |
| src/cross_edge_io.c | 恢复 | 从 .bak 恢复，修复 include 路径和结构体损坏 |

## 详细说明

### 1. Makefile 编译标准回退
- 002B 中将 `-std=gnu99` 改为 `-std=c11`
- 但项目代码依赖 GNU C 扩展（跨文件变量声明等），c11 模式编译失败
- 改回 `-std=gnu99`

### 2. cross_edge_io.c 恢复
- 001B 中 include 路径统一时遗漏此文件，仍使用 `#include "../include/xxx.h"`
- 结构体定义被损坏（两行合并成一行，`//` 注释吞了字段）
- 从 `cross_edge_io.c.bak` 恢复正确版本
