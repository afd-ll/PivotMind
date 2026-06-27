# 0.4.8 — JSON Unicode 转义修复 + 状态文件清理工具

> **日期**: 2026-06-27 | **类型**: 修复 + 新增

## 概述

修复两个 JSON 解析器对 `\uXXXX` Unicode 转义序列的处理缺陷，这两处缺陷导致中文概念名被存储为 literal "uXXXX" 格式（如 `u9a71u52a8u7684` 而非真正的 UTF-8 `駆动的`），造成词汇拓扑中 32% 的节点（9,689 个）被膨化为整句长度的脏概念。新增 `clean_unicode_escapes.py` 工具修复存量状态文件。

## 根因分析

**gateway.c json_extract_string**：`\u` 被当作"反斜杠 + u 字符"处理——反斜杠被消耗，`u` 走 `default` 分支原样写入 buffer，4 位 hex 逐字节复制。结果 `\u9a71` → `u9a71`。

**train_mode.c json_read_string_literal**：`\u` 被识别但丢弃 4 位 hex，用 `?` 占位。中文直接丢失。

**连锁效应**：`_cjk_insert_spaces` 只对 3 字节 UTF-8 插入空格。`uXXXX` 全是 ASCII，跳过。`strtok` 按空格/中文标点切分无匹配，整句话成为一个 token → `_learn_tokens` 创建整句长度概念节点。

## 核心变更

### 1. gateway.c — json_extract_string 增加 \uXXXX 解码

**文件**: `demos/pivotmind_gateway.c:171-217`

- 新增 `<ctype.h>` 依赖
- 遇到 `\u` 时读取 4 位 hex → 重组 Unicode 码点 → UTF-8 编码写入 buffer
- 支持 BMP 范围 (`U+0000`–`U+FFFF`)，3 字节 UTF-8 编码
- `continue` 跳过外层 `pos++`，精确定位到 hex 尾部之后
- 新增边界保护（`i + N >= buf_size`）

### 2. train_mode.c — json_read_string_literal 替换占位符

**文件**: `src/train_mode.c:99-137`

- 将 `?` 占位替换为 UTF-8 解码
- 从 `FILE*` 流中逐字节读取 4 位 hex → 验证 → UTF-8 编码输出
- 无效 hex 时保留 `?` 占位（降级行为）

### 3. convert_state.py — 提升 concept_len 限制

**文件**: `tools/convert_state.py:64`

- `concept_len` 上限从 1024 → 4096，匹配 C 代码实际限制
- 之前文档训练产生的长概念（1256 字节整句）会触发误报 WARN

### 4. clean_unicode_escapes.py — 存量状态文件修复工具

**文件**: `tools/clean_unicode_escapes.py`

- 读取 `.dat` 二进制状态文件 → 解码所有 `uXXXX` 格式概念名 + 边目标名 → 写回
- 预处理代理对：`uD83DuDE80` → `🚀` (4 字节 UTF-8)
- 支持 `--dry-run` 模式（仅统计不写入）
- 兼容现有 C 二进制格式 v5

## 修复效果

| 指标 | 修复前 | 修复后 |
|------|--------|--------|
| 概念名格式 | `u9a71u52a8u7684` (15 字节) | `駆动的` (9 字节 UTF-8) |
| 脏节点数 (词汇拓扑) | 12,725 个 | 0 |
| 脏边目标名 | 12,078 个 | 0 |
| 状态文件大小 | 70,648 KB | ~68,870 KB (节省 ~1.78 MB) |
| train_mode 中文丢失 | 全中文 → `?` | 全中文 → UTF-8 |

## 验证

```bash
# 状态文件清理
python3 tools/clean_unicode_escapes.py pivotmind_state.dat --dry-run
python3 tools/clean_unicode_escapes.py pivotmind_state.dat -o pivotmind_state.cleaned.dat

# 格式转换工具（验证修复后的 concept_len 限制）
python3 tools/convert_state.py pivotmind_state.cleaned.dat --info
```

## 改动文件

| 文件 | 类型 |
|------|------|
| `demos/pivotmind_gateway.c` | 修复 — `json_extract_string` \uXXXX UTF-8 解码 |
| `src/train_mode.c` | 修复 — `json_read_string_literal` \uXXXX UTF-8 解码 |
| `tools/convert_state.py` | 修复 — concept_len 限制 1024 → 4096 |
| `tools/clean_unicode_escapes.py` | 新增 — 状态文件 Unicode 转义清理工具 |
| `tools/test_unicode_escape.py` | 新增 — \uXXXX 解码单元测试 |
| `include/pivotmind_version.h` | 版本号 0.4.7 → 0.4.8 |
| `ARCHITECTURE.md` | 版本号 + JSON Unicode 修复 |
| `README.md` | 版本号 |
| `README.ja.md` | 版本号 |
| `README.zh-CN.md` | 版本号 |
| `README.ko.md` | 版本号 |
| `README.ru.md` | 版本号 |
| `changelogs/046-unicode-escape-json-fix.md` | 本文档 |
