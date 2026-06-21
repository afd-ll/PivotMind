# 0.2.3 — 性能优化与内存稳定

> **日期**: 2026-05-31 | **类型**: 优化

## 概要

针对 v0.2.1 存在的性能瓶颈和内存泄漏问题，引入概念哈希 O(1) 查找、扩散静态数组复用、突触缩放自动修剪三项优化，使引擎从"功能可用"进入"性能可用"阶段。

## 动机

v0.2.1 运行观察：
1. **概念查找慢**：`O(n)` 线性扫描，6.7 万节点规模下单次查找 ~200μs，扩散过程调用数百次，延迟显著
2. **内存碎片**：`diffusion.c` 每次调用 `calloc/free`，长时间运行堆碎片严重
3. **连接无限增长**：突触无自动修剪，连接数从 50 万涨到 232 万，RSS 持续增长至 OOM

## 改动

### 1. 概念哈希 O(1) 查找

**文件**：`include/trace_wisdom_topology.h`，`src/trace_wisdom_topology.c`

`TraceWisdomNetwork` 新增 `concept_hash`（开放定址哈希表）：

```c
typedef struct TraceWisdomNetwork {
    ReasoningNode** nodes;           // 原有数组
    int node_count, max_nodes;

    // 新增：概念哈希表
    ReasoningNode** concept_hash;     // 哈希桶
    int concept_hash_mask;            // 桶大小 - 1（2 的幂）
    // ...
} TraceWisdomNetwork;
```

**新增 API**：
```c
// O(1) 按名称查找概念
ReasoningNode* trace_wisdom_net_find_concept(TraceWisdomNetwork* net, const char* concept);

// 内部：插入哈希表
static int _concept_hash_insert(TraceWisdomNetwork* net, ReasoningNode* node);
```

| 指标 | 优化前 | 优化后 |
|------|--------|--------|
| 查找方式 | 线性扫描 `O(n)` | 哈希查找 `O(1)` |
| 6.7 万节点耗时 | ~200μs/次 | ~2μs/次 |
| 扩散总耗时 | ~50ms | ~5ms |

### 2. 扩散静态数组复用

**文件**：`src/diffusion.c`

`DiffusionCtx` 新增 4 个静态数组，避免每次扩散调用 `calloc/free`：

```c
typedef struct DiffusionCtx {
    // 原有字段...

    // 新增：静态复用数组（避免每次 calloc）
    float  _vocab_scores[MAX_VOCAB];    // 词汇层得分
    int    _vocab_topk[MAX_VOCAB];      // 词汇层 Top-K 索引
    float  _semantic_scores[MAX_NODES];  // 语义层得分
    int    _semantic_topk[MAX_NODES];    // 语义层 Top-K 索引
} DiffusionCtx;
```

**效果**：消除扩散路径上的动态分配，减少堆碎片和 GC 压力。

### 3. 模板解析内存泄漏修复

**文件**：`src/diffusion.c`

模板解析使用 `strdup(conn)` 分配连接词字符串，但从未释放：

```c
// 修复前：每次解析都 strdup，泄漏
char* conn_word = strdup(conn);  // 从未 free

// 修复后：栈池复用，零分配
static char conn_pool[128][32];  // 固定缓冲区
static int  conn_pool_idx = 0;
char* conn_word = conn_pool[conn_pool_idx++ % 128];
strncpy(conn_word, conn, 31);
```

### 4. 突触缩放（Synaptic Scaling）

**文件**：`src/brainstem.c`

每 600 tick 扫描全图所有边，按 `权重 × 置信度` 分层衰减：

```c
// 突触缩放核心逻辑
void synaptic_scaling(TraceWisdomNetwork* net) {
    for (int i = 0; i < net->node_count; i++) {
        ReasoningNode* node = net->nodes[i];
        for (int j = 0; j < node->connection_count; j++) {
            float strength = node->connection_weights[j] *
                           node->connection_confidences[j];

            if (strength < 0.001) {
                // 极弱连接：释放内存（彻底清除）
                _remove_connection(node, j);
                j--;  // 索引回退
            } else if (strength < 0.01) {
                // 弱连接：加速衰减
                node->connection_weights[j] *= 0.5;
            } else if (strength < 0.05) {
                // 较弱连接：缓慢衰减
                node->connection_weights[j] *= 0.9;
            }
            // strength >= 0.05：保留，不衰减
        }
    }
}
```

**设计哲学**：AI 学到的东西不能直接砍掉，弱连接自然消退，强连接保留——不是删除，而是**遗忘**。

**实测效果**（运行 2 小时）：

| 指标 | 优化前 | 优化后 |
|------|--------|--------|
| 连接数 | 232 万（不修剪） | 185 万（自动修剪 47 万） |
| RSS 内存 | 持续增长至 OOM | 420MB 左右波动 |
| 崩溃频率 | ~30 分钟一次 | ~2 小时一次（堆损坏未修复） |

### 5. 堆监控上线

**文件**：`src/brainstem.c`

每 30 tick 输出节点数/连接数/RSS/VSZ：

```
[tick=8540] nodes=67100 conn=1,850,342 RSS=420MB VSZ=1.2GB
```

供观测和调试使用，不影响引擎运行。

## 效果

| 指标 | v0.2.1 | v0.2.2 |
|------|---------|---------|
| 概念查找耗时 | ~200μs | ~2μs |
| 扩散总耗时 | ~50ms | ~5ms |
| 连接数 | 232 万（不修剪） | 185 万（自动修剪） |
| RSS 内存 | 持续增长 | 420MB 波动 |
| 稳定性 | ~30 分钟必崩 | ~2 小时（堆损坏） |

## 设计决策

1. **为什么用开放定址而非链地址哈希？**  
   开放定址缓存局部性更好，对 CPU cache 友好。概念查找是热路径，cache hit 至关重要。

2. **突触缩放为什么是衰减而非删除？**  
   生物合理性：遗忘是渐进的，不是突变的。衰减给"复苏"留有机会——若某弱连接后续被频繁共现，权重可重新上升。

## 已知问题（v0.2.2）

- **堆损坏未修复**：`trace_wisdom_net_dynamic_add_node` 内部无去重，12 万次调用后堆碎片导致崩溃（v0.2.3 修复）
- **内存泄漏未完全修复**：`handle_learn` 中的线性扫描去重缺失，导致重复调用 `dynamic_add_node`

## 关联

- 上一版本：v0.2.1（数据丰富度提升）
- 下一版本：v0.2.3（堆损坏修复 + 内感受自检，彻底稳定）
