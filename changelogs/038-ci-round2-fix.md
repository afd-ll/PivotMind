# 0.4.4 — CI 第二轮修复：跳过 Windows 专属测试 + ARM libcurl 交叉编译

> **日期**: 2026-06-21 | **类型**: 修复

## 概述

修复 CI 两个剩余问题：(1) `test-chinese` 依赖 `windows.h` 在 Ubuntu 无法编译；(2) ARM 交叉编译缺少 libcurl。

## 修复

### 1. 跳过 Windows 专属测试
`test-chinese` 需要 `<windows.h>`，从 `make test` 中剥离为显式测试列表：
```yaml
make test-tensor test-model test-metrics test-trainer test-io test-cc test-web-fetch
```

### 2. ARM libcurl 交叉编译
在 `Build ARM` 前新增 `Build libcurl for ARM` 步骤，交叉编译 curl-8.12.1 到 `/tmp/curl-arm`，ARM LDFLAGS 补 `-L/tmp/curl-arm/lib -lcurl -lz`。

## 改动文件

| 文件 | 变更 |
|------|------|
| `.github/workflows/ci.yml` | x86_64: `make test`→显式测试列表；ARM: +libcurl 交叉编译 + LDFLAGS/CFLAGS 更新 |
| `include/pivotmind_version.h` | 0.4.3 → **0.4.4** |
