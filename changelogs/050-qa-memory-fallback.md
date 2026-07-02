# 0.4.10 — QA 记忆检索接入 /chat 兜底管线

> **日期**: 2026-07-01 | **类型**: 新增

## 概述

将 QA 记忆检索模块（QAMemory）接入 gateway `/chat` 管线，作为扩散引擎和联想推理之后的最终兜底。当 PFE → prefrontal_chat（扩散+联想）全部无产出时，自动检索预存 QA 对返回匹配答案。

## 核心变更

### 1. 初始化：加载 QA 语料

**文件**: `demos/pivotmind_gateway.c` (`gw_system_init()`)

在 Prefrontal 创建完成后加载 QA 语料文件：

```c
gw->qa_memory = qa_memory_create("corpus/xiaohuangji_pipe.txt", 500000);
```

- 语料路径: `corpus/xiaohuangji_pipe.txt` (~50万对)
- 加载失败非致命，仅打印警告继续运行

### 2. 兜底逻辑：handle_chat 最终 fallback

**文件**: `demos/pivotmind_gateway.c` (`handle_chat()`)

在 `prefrontal_chat()` 返回 NULL 之后插入：

```c
if (!response && gw->qa_memory) {
    const char* qa_answer = qa_memory_query(gw->qa_memory, msg);
    if (qa_answer) {
        response = strdup(qa_answer);
    }
}
```

检索基于 token 交集评分 (`match_count / sqrt(Q_token_count + 1)`)，命中即返回预设答案。

### 3. 清理：destroy QA 记忆

**文件**: `demos/pivotmind_gateway.c` (`gw_system_shutdown()`)

```c
if (gw->qa_memory) qa_memory_destroy(gw->qa_memory);
```

## /chat 回复完整 fallback 链

```
PFE 推理
  ├─ 成功 → 返回
  └─ 失败 ↓
prefrontal_chat (扩散 + ACC 门控)
  ├─ 成功 → 返回
  └─ 失败 ↓
联想推理 (associate + topology_walk)
  ├─ 成功 → 返回
  └─ 失败 ↓
QA 记忆检索 ← 本次新增
  ├─ 命中 → 返回预设答案
  └─ 未命中 → "(无回应)"
```

## 改动文件

| 文件 | 变更 |
|------|------|
| `demos/pivotmind_gateway.c` | 修改 — 初始化 (6行) + 兜底查询 (7行) + 清理 (1行) |

## 编译验证

```bash
make clean && make gateway
```

零错误，零新增警告。

## 测试验证

| 测试 | 结果 |
|------|------|
| `test_diffusion_unit` | 3/3 PASS |
| `test_model` | 10/10 PASS |
| `test_metrics` | 14/14 PASS |
| `test_chinese` | PASS |
