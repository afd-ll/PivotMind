# 0.4.5 — ARM libcurl 交叉编译 configure 失败修复

> **日期**: 2026-06-21 | **类型**: 修复

## 概述

ARM CI 的 `Build libcurl for ARM` 步骤 configure 失败（exit 1）。

## 根因

libcurl 交叉编译缺少三个关键设置：
1. **缺 `--build`**：configure 不知道构建机的三元组
2. **缺 `CC`**：未显式指定 ARM 交叉编译器
3. **pkg-config 污染**：主机 pkg-config 无法识别 ARM 交叉编译链的 OpenSSL

## 修复

```bash
CC=arm-linux-gnueabihf-gcc \
CPPFLAGS="-I/tmp/openssl-arm/include" \
LDFLAGS="-L/tmp/openssl-arm/lib" \
./configure \
  --build=x86_64-linux-gnu \
  --host=arm-linux-gnueabihf \
  --with-openssl=/tmp/openssl-arm \
  --without-nghttp2 --without-zstd --without-brotli \
  --disable-shared --disable-ldap --without-libpsl
```

加 `PKG_CONFIG_LIBDIR` 隔离主机 pkg-config，删无关依赖（zstd/brotli/ldap/psl）。

## 改动文件

| 文件 | 变更 |
|------|------|
| `.github/workflows/ci.yml` | `Build libcurl for ARM`：+CC, +CPPFLAGS, +LDFLAGS, +--build, +PKG_CONFIG_LIBDIR, 精简依赖 |
| `include/pivotmind_version.h` | 0.4.4 → **0.4.5** |
