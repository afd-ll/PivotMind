# 0.5.6 — 惰性内存分配优化

> **日期**: 2026-07-24 | **类型**: 优化

## 概述

本轮核心目标：降低单节点内存开销，缓解自学增长导致的 OOM。不改特征格式、不动修剪策略、不动架构——仅改变分配时机。

## 背景

v0.5.1 取消硬天花板后，节点数随自学持续增长，每节点固定消耗 ~2.3KB（struct 256B + features 2048B + edges 预分配 240B + concept 字符串），3.5 天自学可导致单进程膨胀至 2.47GB 触发 OOM。

当前架构下 freeze 仅释放边数据（~200B/节点），节点本体和特征向量一直留在内存中。

## 核心变更

### 惰性特征分配 (`huarong_topology.c`)

- `create_reasoning_node()` 不再预分配 512 维特征向量，改为 `features = NULL`
- 新增 `lazy_alloc_node_features()`：首次需要特征时按需分配，用概念文本 FNV-1a 哈希填充确定性种子
- 若调用方传入了特征数组则立即复制（保持兼容）
- `autonomic_learner.c` Hebbian 更新时自动触发惰性分配

### 冻结时特征释放 (`node_cache.c`)

- `node_cache_freeze()` 在释放边数据后，同步释放特征向量并置 `features = NULL`
- `node_cache_thaw()` 反序列化时自动重建特征（已有逻辑，无需改动）
- 冷节点解冻后从磁盘按偏移量读回 2048 字节，一次 fread 几乎无感

### 惰性边分配 (`huarong_topology.c`)

- `create_reasoning_node()` 不再 `calloc(10, Edge)`，改为 `edges = NULL, edge_capacity = 0`
- `huarong_net_add_connection()` 处理零容量首条边：使用默认容量起步，跳过 NULL 旧数组的 memcpy

## 内存效果

| 节点状态 | 改动前 | 改动后 | 节约 |
|---------|--------|--------|------|
| 刚创建（未激活） | ~2.3KB | ~300B | **87%** |
| 首次激活后 | ~2.3KB | ~2.3KB | 0（按需分配） |
| 冻结后 | ~2.1KB（features 保留） | ~300B（全部释放） | **86%** |
| 解冻后 | ~2.3KB | ~2.3KB | 0（从磁盘读回） |

核心收益在冷节点——大量被 `/learn` 创建但从未参与扩散的单字/低频组合词节点，创建时不再付 2KB+ 的内存账单。

## 附带变更

本轮同时合入此前未提交的本地改动：

### 死节点清理 (`multi_topology.c`)

- 新增 `master_prune_dead_nodes()`：移除零边零激活的孤立节点
- 脑干每 60 tick 存盘前自动调用

### 语言感知扩散 (`diffusion.c`)

- 输入主导语言检测（CJK vs ASCII），扩散时同语言邻居激活 ×1.3，跨语言 ×0.4
- 解决中英混合输入时走边偏向错误语言的问题

### 搜索增强 (`perception.c`)

- 搜索结果文本同步喂入 `auto_learn_concepts`，利用搜索数据建复合词节点和 PMI 边

### 存盘间隔调整 (`brainstem.c`)

- 存盘间隔从 300 tick 缩短为 60 tick（~5min → ~17min），减少重启丢数据

## 改动文件

| 文件 | 变更 |
|------|------|
| `src/huarong_topology.c` | 惰性特征分配 + 惰性边分配 + `lazy_alloc_node_features()` |
| `include/huarong_topology.h` | 声明 `lazy_alloc_node_features()` |
| `src/node_cache.c` | freeze 时连特征一起释放 |
| `src/autonomic_learner.c` | Hebbian 更新时触发惰性特征分配 |
| `src/multi_topology.c` | `master_prune_dead_nodes()` 实现 |
| `include/multi_topology.h` | `master_prune_dead_nodes()` 声明 |
| `src/brainstem.c` | 存盘间隔 300→60 tick，存前调用 prune |
| `src/diffusion.c` | 语言感知扩散 |
| `src/perception.c` | 搜索结果走 `auto_learn_concepts` |

## 编译验证

```bash
$ make gateway
ccache gcc ... -o build/bin/pivotmind_gateway ...

$ nm build/bin/pivotmind_gateway | grep lazy_alloc
00000000000431a4 t lazy_alloc_node_features
```

编译通过无报错，二进制 412KB。
