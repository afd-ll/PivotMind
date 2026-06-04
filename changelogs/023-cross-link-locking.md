# 023 - 跨拓扑连接并发锁修复

## 日期

2026-05-31

## 问题

batch_learn 多线程 (OMP) 并发调用 `autonomic_learn_from_dialog()` → `auto_link_activated_nodes()` → `master_add_cross_link()` 时，多个线程同时对 `cross_links[]` 和 `cross_adj[]` 做 realloc/malloc/赋值，**无任何锁保护**，导致：

- 堆损坏 (Invalid address specified to RtlReAllocateHeap)
- SIGTRAP / 随机 crash
- Thread 27、37、40、43、29 全部卡在 `master_add_cross_link()` → `realloc()` 内部

这是和 `connections[]` 并发 realloc 完全相同的模式：多写入者 + 无锁 = 堆损坏。

## 根因

`MasterTopology` 结构体已有 `pthread_rwlock_t rwlock` 字段且已调用 `pthread_rwlock_init`，但以下跨拓扑操作函数完全没有使用它：

| 函数 | 操作 | 并发调用路径 |
|------|------|------------|
| `master_add_cross_link()` | realloc cross_links[] / cross_adj[], malloc 新条目 | ✅ OMP 并行 |
| `cross_link_exists()` | 读 cross_adj[] 链表 | ✅ OMP 并行 (同一路径) |
| `master_clear_cross_links()` | free + 置 NULL | rebuild_cross_connections (串行但防御) |
| `master_prune_cross_links()` | free 个别条目 + 置 NULL | 串行但防御 |
| `remove_cross_topology_link()` | free + 元素搬移 | 串行但防御 |

## 修复

对全部 5 个函数添加 `pthread_rwlock` 保护：

### `master_add_cross_link()` — 写锁
- 参数检查后 `pthread_rwlock_wrlock`
- 所有 return 改为 `goto unlock` + `pthread_rwlock_unlock`
- 统一单出口：`result = link->link_id; unlock: return result;`

### `cross_link_exists()` — 读锁
- 参数检查后 `pthread_rwlock_rdlock`
- 查找到则 `exists = 1; goto unlock;`
- 统一出口返回 exists

### `master_clear_cross_links()` — 写锁（防御）
### `master_prune_cross_links()` — 写锁（防御）
### `remove_cross_topology_link()` — 写锁（防御）

## 修改文件

- `src/multi_topology.c` — 4 个函数加锁
- `src/topology_growth.c` — 1 个函数加锁

## 影响范围

- 跨拓扑连接数组的所有读写操作现在受 `MasterTopology.rwlock` 保护
- 对单线程调用路径无行为变化（锁无竞争时开销极小）
- 对 batch_learn 并行训练路径：消除堆损坏 crash
