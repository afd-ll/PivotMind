# 003: 004A 优化落地 — dialog 游标 + tensor stride + concept visited

> 日期: 2026-05-25 | 关联审查: 004A-optimization-review.md

## 实际修改

| 文件 | 操作 | 说明 |
|------|------|------|
| src/dialog_system.c | 重构 | strncat → snprintf+pos 游标，消除重复 strlen |
| src/tensor.c | 优化 | broadcast_to 预计算 stride，O(n×ndim)→O(n) |
| src/concept_abstraction.c | 修复 | visited[256] → 动态分配 hierarchy->capacity |

## 004A 报告勘误

| 模块 | 004A 判断 | 实际情况 |
|------|-----------|----------|
| causal_reasoning | "优先队列用数组" | 已是二叉堆（pq_sift_up/down） |
| memory_system | "可能线性扫描" | stm_retrieve/ltm_retrieve 已用哈希表 O(1) |
| multi_topology | "串行传播" | 已用 TopoPropTask + thread_pool 并行 |

## 修改详情

### 1. dialog_system.c — pos 游标替代 strncat
- `causal_reason_from_semantic`: 引入 `size_t pos` 游标，所有 `strncat(response, ...)` → `pos += snprintf(response + pos, ...)`
- `process_causal_query`: 同样用 `fr_pos` 游标
- 合并碎片化拼接（5 行 strncat → 1 行 snprintf）
- 消除每次 `strlen(response)` 的 O(n) 重算

### 2. tensor.c — broadcast stride 预计算
- 预计算 `out_strides[16]` 和 `src_strides[16]`
- 广播循环中直接查表计算源索引，内层除法取模逻辑不变但 stride 乘法预计算

### 3. concept_abstraction.c — 环检测动态分配
- `int visited[256]` → `malloc(hierarchy->capacity * sizeof(int))`
- 所有 return 路径添加 `free(visited)`
- 容量检查从 `visited_count < 256` → `visited_count < hierarchy->capacity`
