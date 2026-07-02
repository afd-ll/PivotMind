# 0.4.10 — 二段联想扩散 fallback 接入 /chat 管线

> **日期**: 2026-07-01 | **类型**: 新增

## 概述

将联想推理引擎（AssociativeEngine）作为扩散引擎的 fallback 接入 `/chat` 回复管线。扩散无产出时，自动回退到"一段联想 + 二段答区走边"流程，利用已有但从未被调用的 `generate_from_associations()` 生成回复。

## 核心变更

### 1. prefrontal_chat 新增联想推理 fallback

**文件**: `src/prefrontal.c` (第 117-131 行)

扩散引擎三次尝试全部失败（`response == NULL`）后：

1. 创建 `AssociativeEngine`
2. 调用 `associate_from_text()` — 一段联想（`max_hops=0` 即动态深度 1-3）
3. 若联想出 ≥2 个候选概念，调用 `generate_from_associations()` — 二段答区走边
4. 走边使用已有 `ctx_activations[]` 作为拓扑意图权重
5. 释放引擎，返回结果

### 2. 复活 generate_from_associations

**文件**: `src/associative_reasoning.c` (第 273 行)

`generate_from_associations()` 此前已定义但从未被调用（死代码），本次接入即复活：
- 从最高激活联想取 top-5 起点
- 优先跨拓扑走边（`topology_walk_cross`），带防回声
- 无跨拓扑连接时回退到单拓扑贪心走边（`topology_walk_greedy`）

### 3. 零栈溢出风险

与上次失败的尝试不同，本次不走 `diffusion_generate()`（无 `DiffusionCandidate[256]` 大数组），完全依赖已有的非递归走边函数 `topology_walk_cross()` 和 `topology_walk_greedy()`，栈使用仅 ~5KB。

## 改动文件

| 文件 | 变更 |
|------|------|
| `src/prefrontal.c` | 修改 — 引入 `associative_reasoning.h` + 扩散 fallback 逻辑 (14 行) |
| `src/associative_reasoning.c` | 无修改（已有函数仅被新调用方激活） |

## 编译验证

```bash
make clean && make -j4
```

零错误、零新增警告。

## 测试验证

| 测试 | 结果 |
|------|------|
| `test_diffusion_unit` | 3/3 PASS |
| `test_model` | 10/10 PASS |
| `test_metrics` | 14/14 PASS |
| `test_chinese` | PASS |
