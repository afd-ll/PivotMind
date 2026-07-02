# 0.4.13 — 对话输出质量提升 + 编译警告清零

> **日期**: 2026-07-02 | **类型**: 新增 + 修复 + 清理

## 概述

三项核心改进：(1) 对话输出连接词由 POS 语法关系动态映射，替代硬编码轮换；(2) 走边推理引入边特异性权重，区分语义专一边与均匀 hub 边；(3) 全项目 10 个文件 14 处编译警告全面清零。

## 核心变更

### 1. 连接词 POS 语法关系映射 (dialog_generate.c)

**问题**：硬编码连接词轮换（`""`, `"的"`, `"是"`, `"和"`, `"了"`, `"在"` 每 3 词循环一次），语法关系不准确。

**方案**：三级回退机制：

```
Level1: 模板匹配 (按需)
Level2: pos_connector_map(prev_pos, curr_pos)  — 按词类对返回语法连接词
Level3: 直接拼接（无连接词）
```

`pos_connector_map` 映射表：

| prev_pos | curr_pos | 连接词 | 示例 |
|----------|----------|--------|------|
| N | N | `的` | 人类的语言 |
| Adj | N | `的` | 美丽的风景 |
| Adv | V | `地` | 快速地运行 |
| N | V | `""` (空格) | 太阳升起 |
| V | N | `""` (空格) | 吃苹果 |
| N | Adj | `是` | 天空是蓝色 |
| Adv | Adj | `""` (空格) | 很美丽 |

**效果**：输出从"人好大来没家" → "人类的语言很好"，语法关系自然准确。

### 2. 边特异性权重 (multi_topology.c)

**问题**：扩散引擎走边时所有出边平等对待。均匀 hub 词（如"你"→8000条边）的每个邻居都获得相同权重，导致输出与输入语义脱钩。

**方案**：`topology_walk_greedy` 新增边特异性折扣：

```c
concentration = max_w / sum_w_all;  // 边权重集中度
edge_spec = 0.55 + 0.45 * (concentration / (concentration + expected));
```

- 高集中度（少数强边支配） → 语义专一 → edge_spec → 1.0（不打折）
- 低集中度（均匀分布） → 偶然共现 hub 词 → edge_spec → 0.55（55折）
- 触发条件：当前节点出边 > 4 条时才计算

**效果**：强语义关联边获得更高权重，弱随机共现边被压低，输出相关性提升。

### 3. 全局编译警告清零

在 `-Wall -Wextra -O2 -std=gnu99` 下，全项目编译零警告。

| 文件 | 原警告 | 修复方式 |
|------|--------|---------|
| `brainstem.c` | `localtime_r` 隐式声明 | `#ifdef _WIN32` → `localtime_s` |
| `error.c` | `localtime_r` 隐式声明 | 同上 |
| `perception.c` | `localtime_r` 隐式声明 | 同上 |
| `cingulate.c` | 未使用变量 `mean` | 删除 |
| `cognitive_controller.c` | `str_ends_with` 未使用函数 | `__attribute__((unused))` |
| `json_config.c` | `strchr` 多字符常量 **bug** | `strchr` → `strstr` |
| `json_config.c` | 未使用变量 `closing` | 删除 |
| `node_cache.c` | 未使用参数 `net` | `(void)net;` |
| `prefrontal.c` | 未使用参数 `response` | `(void)response;` |
| `dialog_generate.c` | `PUNCT_CHARS` 未使用数组 | 删除 |
| `web_fetch.c` (3处) | `strncpy` 输出可能截断 | → `snprintf` |
| `batch_learn.c` | 注释内 `/*` 嵌套 | 加空格 |
| `batch_learn.c` | 不兼容指针类型 | 显式类型转换 |

其中 **json_config.c 的 `strchr(key, 'rate')` 是一个真实 bug**：`strchr` 只能查找单个字符，但传入了多字符字面量，应使用 `strstr(key, "rate")` 进行子串匹配。

## 改动文件

| 文件 | 变更 |
|------|------|
| `src/dialog_generate.c` | 重构：POS 语法关系映射 + 删除死代码 PUNCT_CHARS |
| `src/multi_topology.c` | 新增：边特异性权重计算 |
| `src/brainstem.c` | 修复：Windows `localtime_r` → `localtime_s` |
| `src/error.c` | 修复：Windows `localtime_r` → `localtime_s` |
| `src/perception.c` | 修复：Windows `localtime_r` → `localtime_s` |
| `src/cingulate.c` | 清理：删除未使用变量 |
| `src/cognitive_controller.c` | 清理：标记未使用函数 |
| `src/json_config.c` | 修复：`strchr`→`strstr` bug + 删除未使用变量 |
| `src/node_cache.c` | 清理：消除未使用参数警告 |
| `src/prefrontal.c` | 清理：消除未使用参数警告 |
| `src/web_fetch.c` | 修复：`strncpy`→`snprintf` 截断警告 |
| `tools/batch_learn.c` | 修复：注释 + 指针类型转换 |

**总计**: 12 文件, +102 行, -72 行

## 编译验证

```bash
gcc -Wall -Wextra -O2 -std=gnu99 -Iinclude -fopenmp -pthread
```
全项目 86 个源文件编译零警告零错误。
