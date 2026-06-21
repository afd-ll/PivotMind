# 0.4.2 — domain_throttle() 整数溢出修复

> **日期**: 2026-06-21 | **类型**: 修复

## 概述

修复 `web_fetch.c:domain_throttle()` 中首次访问域名时因 `last_request=0` 导致的 32 位 int 乘法溢出，造成网关挂死数十秒。

## 根因

```c
int elapsed_ms = (int)((now - rec->last_request) * 1000);  // L201
```

`rec->last_request` 初始值为 0。首次访问时 `(now - 0) * 1000` ≈ 1.78×10¹²，远超 int 上限 2.14×10⁹。ARM 平台上补码溢出截断为负值，导致 `wait_ms = delay - 负数 = 巨大正数`，`msleep()` 挂死。

## 修复

在速率限制段前加首次请求守卫：

```c
if (rec->last_request == 0) {
    rec->last_request = now;
    DOMAIN_UNLOCK();
    return;
}
```

零行类型变更，零结构变更，精准切入溢出点。

## 改动文件

| 文件 | 变更 |
|------|------|
| `src/web_fetch.c` | `domain_throttle()` L200 前加首次请求守卫（+6 行） |
| `include/pivotmind_version.h` | 0.4.1 → **0.4.2** |
