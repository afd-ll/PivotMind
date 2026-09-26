# v0.6.10 — 内存安全缺陷批（ASan 实锤 10 项）

**日期**：2026-09-26
**来源**：工作台 **BG-29 / BG-30-A2~A8 / BG-35 / BG-37 / BG-38**（同一轮内存审计）
**分支**：`bg29-fixes`
**状态格式**：**不变（仍 v11）** ⇒ 回退 = 换回旧二进制，数据文件无需迁移

---

## 一、这一批是什么

同一轮内存审计（ASan + 三路子代理读码复核）排出的缺陷：**7 类 10 处**，
性质全是「读-释放次序错误」「扩容不变量被破坏」「取自文件而未校验的长度」。
**没有一处改变行为语义，也没有一处改变盘上格式**——所以这一版可以在线换二进制，
不需要迁移数据。

## 二、逐项

### BG-35 `src/nn/memory_arena.c:389` — 池扩容重复发放（double-free 的根因）

`object_pool_acquire` 扩容分支把新对象写进**尾部** `[old_total, new_capacity)`，
却仍按**头部**语义 `free_list[--free_count]` 发放 ⇒ 拿到 `free_list[old_total-1]`，
那是第 1 次 acquire 就已发出的对象 ⇒ **同一指针发放两次** ⇒
`causal_graph_destroy` 对同一指针双重 release ⇒ `object_pool_destroy` 双重 free
（ASan: `attempting double-free, 64B`）。

修法：新对象写头部 `[0, grown)`（此时 `free_count == 0`，旧副本覆盖无副作用），
并令 `free_count = grown`，恢复不变量「**空闲区 = free_list[0, free_count)**」。
回归：`test_object_pool_grow_no_dup`（容量 4、连取 10 次跨两次扩容，两两指针互异）。

### BG-38 `src/multi_topology.c:3418` — 截断多字节序列越界读

按 UTF-8 前导字节取 `clen`（2/3/4）后无条件读满 `clen` 字节：末字符若是被截断的
多字节序列（3 字节前导却只剩 1 字节），越过 `strdup` 分配末尾读到 NUL 之后 1 字节
（ASan: `heap-buffer-overflow READ of size 1 @ :3416`）。
条件里加 `*cp`（**当前字节**）——⛔ 不是 `cp[b]`：`cp` 在循环体里自增，`cp[b]` 会读成
`orig[2b]`（越读越远，反而制造新越界）；`*cp` 恒读当前位，最多读到那个 NUL（仍在分配内）。

### BG-37 `src/prefrontal_executive.c:680` — free 后读

`causal_search_results_free(results, …)` 之后才 `LOG_INFO(… results[0].total_strength)`
⇒ `heap-use-after-free`。改为**先 LOG 后 free**。

### BG-30-A2 `src/catastrophic_forgetting.c:1055` — weights 双重 free

`task_snapshot_destroy` 里 `free(np->weights)` 之后**没有再判指针**，后面又
`if (np->weights) free(np->weights)` ⇒ double free。改为 free 后置 `NULL`。

### BG-30-A3 `src/template_builder.c:269` — 三次 realloc 统一判空

`nc` / `ir` / … 各自独立 realloc，统一在末尾判空：若**前一个成功、后一个失败**，
`realloc` 已释放旧块，但字段仍指该已释放块，随后 `free(nc)` 丢掉新块，
`template_free_groups()` 又释放旧字段（悬挂指针）⇒ double free。
改为**逐个处理：成功立即赋回字段；失败不释放原块**（字段恒为 NULL 或有效）。

### BG-30-A4 `src/causal_reasoning.c:1522` — realloc 未赋回

`realloc(pattern->instance_cause, …)` 成功后未立刻赋回字段，失败分支
`free(new_cause)` —— 那个 `new_cause` **正是已生效的新块** ⇒ double free。
改为 realloc 成功即赋回；失败分支不再 free。

### BG-30-A5 `src/node_cache.c:323` — 读端零校验

`node_cache_thaw` 的 `concept_len` / `feat_dim` / `conn_count` **全部取自文件、零校验**：
截断/损坏文件会导致 `p += concept_len+1` 越界、`memcpy(…, feat_dim*4)` 越界读、
`calloc(feat_dim, …)` 巨量分配、`conn_count` 循环越界读。
补三处「**上界 + 剩余字节**」校验，且**必须在 calloc 之前**：

- `concept_len ∈ [0, 4096]` 且 `nc_end - p >= concept_len + 1`
  （口径注：本文件盘上 `concept_len` 是 **strlen 不含 NUL**，故 0 合法——
  与 `multi_topology.c:5442` 的「含 NUL、拒绝 0」口径**不同**，两处各自沿用既有写法）
- `feat_dim ∈ [0, PM_NODE_FEATURE_DIM]` 且剩余 ≥ `feat_dim * sizeof(float)`
  （上界取全仓 SSOT `constants.h:37`，不自行编造数字）
- `conn_count ∈ [0, 65536]` 且剩余 ≥ `conn_count * 16`（每条边盘上恰 16B）

⚠️ 遗留（如实记账，未修）：本文件兄弟函数 `node_cache_export_frozen_edges(:456)` 与
`…_snap(:529)` 仍**未校验 `feat_dim`** —— 视为既有遗漏。

### BG-30-A8 `src/vocab.c:325` — 栈溢出（且是死写）

`char qbuf[4096], abuf[4096]` 配合无界 `strcpy(qbuf + qpos, token)`：单行分词累计
超 4096 字节即**栈溢出**。而这两个缓冲在本函数内**没有任何读取点**（纯死写）⇒
连同写语句一并删除，分词/词频统计逻辑与执行顺序**完全不变**。

## 三、验证与部署

| 项 | 结果 |
|---|---|
| 构建（armbian，`make -j4 all`） | **0 error / 0 warning** |
| 预飞行（新二进制 + 8099 端口 + 隔离数据） | 状态加载正常 · `clock_ticks` 68→81 在涨 · `[ERROR]` = **0** |
| 线上切换 | 见 `pivotmind-verify` 技能「切换单」流程 |
| 回退 | 数据格式未变 ⇒ 直接换回 `pivotmind_gateway.prev-<date>` 即可 |

签名：昭
