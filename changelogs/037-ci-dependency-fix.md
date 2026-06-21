# 0.4.3 — GitHub CI 依赖不全导致编译失败

> **日期**: 2026-06-21 | **类型**: 修复

## 概述

`ci.yml` 第 15 行 `apt-get install` 缺少 `libcurl4-openssl-dev` 和 `zlib1g-dev`，导致 Makefile 的 `-lcurl -lz` 链接失败。

## 修复

```yaml
# 改前
libsqlite3-dev libssl-dev

# 改后
libsqlite3-dev libssl-dev libcurl4-openssl-dev zlib1g-dev
```

## 改动文件

| 文件 | 变更 |
|------|------|
| `.github/workflows/ci.yml` | 第 15 行加 `libcurl4-openssl-dev zlib1g-dev` |
| `include/pivotmind_version.h` | 0.4.2 → **0.4.3** |
