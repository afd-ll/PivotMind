# 040 — 修复延迟释放 retired 指针的悬空竞态

## 版本
`0.4.1`

## 问题
`double free or corruption (!prev)` 崩溃。RSS=81.9MB，连接=10,257，非 OOM。

根因：`add_connection` 扩容 edge 数组时，旧数组**先入退役链表、后 swap 活指针**。退役链表中的旧指针被 `cleanup_retired` free 后，`from_node->edges` 仍短暂指向已释放内存，训练线程无锁读 `node_conn_find` 踩到悬空指针，破坏 malloc 元数据。

同类问题：多处 `free(conn_hash)` 后 `conn_hash = NULL`，free→NULL 之间存在悬空窗口。

## 修复
**核心原则：先切断活引用，再释放/退役旧内存。**

### 修改文件

| 文件 | 修改点 |
|------|--------|
| `src/huarong_topology.c` | 4 处 |
| `src/node_cache.c` | 1 处 |
| `src/health_monitor.c` | 1 处 |

### `huarong_topology.c`

1. **`add_connection` edge 扩容**：先 `from_node->edges = new_edges`、初始化新槽位，再用保存的 `old_edges_save` 入退役链表
2. **`cleanup_retired`**：去掉死 guard `active_readers`（从未递增），增加 `retired_pending` 快速返回和 double-check
3. **3 处边压缩 `conn_hash` 重建**：`void* old = conn_hash; conn_hash = NULL; free(old)` 替代 `free(conn_hash); conn_hash = NULL`

### `node_cache.c`
- `freeze` 路径：先 `node->edges = NULL` / `node->conn_hash = NULL`，再 `huarong_net_retire_blob()`

### `health_monitor.c`
- prune 路径：先 NULL 再 free，与 huarong_topology.c 一致

## 安全保证
- swap-before-retire：活指针在入退役链表前已指向新内存
- NULL-before-free：无锁读 `node_conn_hash_lookup` 碰到 NULL 直接返回 -1，不会解引用已释放指针
- `cleanup_retired` 仅在 epoch 间隙/销毁时调用，不与 `add_connection` 并发
