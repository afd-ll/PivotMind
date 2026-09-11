# v0.5.27 — round 3 并发修复：批次完成语义（R1）与激活账本锁域（R2）

> **日期**: 2026-09-11 | **类型**: 修复
> **来源**: 由 x86_64/TSan 复验驱动的 round 3 并发修复（承接 v0.5.26「已知未修问题」A 的批次契约、B 的批内竞争）
> **提交**: `49d8a69` → `4a77564` → `2f47dbc` → `9a06bbd` → `1e84755`（`49d8a69` = v0.5.26 发布点；20 个文件 `+944 / −126`）
> **说明**: 分支 `fix/round3-concurrency` 另含一个**上一发布点补记**提交 `d75eec1 docs(changelog): record x86_64/TSan evidence ... for v0.5.26`——它只改 `CHANGELOG.md` 与 `changelogs/070`，属 **v0.5.26 的文档补记、非本版代码变更**，故不在上述提交链内，也不计入「20 个文件」。

## 概述

v0.5.26 在 `changelogs/070` 的「已知未修问题」中如实登记了两类并发债，并点名 round 3 方案在此分支上修复：

- **A**：`thread_pool_batch` 的**完成契约不成立**——`workers_done` 记的是 worker 跑完的「趟数」却被当「人头数」用作完成屏障，导致 `batch()` 提前返回，调用方 `free` 任务数组时 worker 仍在读 → `dialog_topo_worker` heap-use-after-free。
- **B**：同一批**内部**的激活账本竞争——`master_activate_node` / `master_propagate_activation` 与 `dialog_topo_worker` 在「worker 线程 + 主线程偷到的任务」之间并发写同一批账；另有 `feature_learn` 刷盘读路径缺锁、若干 `node->activation` 裸写。070 明确写过：**「修好批次屏障也不会自动消失，须独立加锁」**。

本版即把 A 与 B **一并**落地，按四条主线推进，最后按 TSan 复验结果做三处收口：

| 主线 | 主题 | 要点 |
|------|------|------|
| **R1** | **批次完成语义** | `workers_done`（趟数）→ **`tasks_left` 任务账** + **每批 `batch_epoch` 代次握手**；索引分配 / 数组快照 / 代次校验收进同一把 `pool->mutex` |
| **R1'** | **对话 worker 生命周期** | `dialog_system.c` 6 处 `node->activation` 裸写改持 `node_locks[node_id & 255]`；`hop_propagated` 改原子 |
| **R2a** | **新锁域** | `MasterTopology.activation_locks[16]` 激活账本分片锁（追加末尾，不动字段偏移）+ `master_activate_node` 两不嵌套临界区 + `master_propagate_activation` 先放源分片锁再调 |
| **R2b** | **读侧补锁** | `feature_learn_graph_smooth` 单线程快路径 + OpenMP 路径按 `node_locks` 加锁 |
| **收口** | **TSan 复验后** | `thread_pool.c` 屏障读改 `__atomic_load_n(ACQUIRE)`；`multi_topology.c` `active_topo_id` 改原子；`dialog_system.c:177` 补节点锁 |

一批共同的目标是：**让「`batch()` 返回」这句话重新说得出真值**，并让「同一批内的激活账」不再被两个线程同时改写——**两者都是加同步与生命周期，而非降并发**。

## 核心变更

### ① R1 — 批次完成语义（`src/thread_pool.c`、`include/thread_pool.h`、`src/dialog_system.c`）

#### 病灶：`workers_done` 记「趟数」当「人头数」

- 旧实现的完成屏障用 `workers_done` 累加：每个 worker 完成一次窃取就 `++`，主线程看到它凑满 `num_threads` 即认为本批结束。但**一个 worker 可以在本批内多跑几趟**（多窃取几次），于是「趟数」先于「真跑完 count 件任务」凑满 → `thread_pool_batch` **在任务尚未全部执行完时返回**。
- 后果（070 已登记）：调用方 `dialog_reasoning_create` 返回后立即 `free(tasks/th_tasks)`，而 worker 仍在 `dialog_topo_worker` 里解引用该数组 → **heap-use-after-free**。这是**基线自带的老债**，v0.5.26 只加了「批次闸门」（堵并发第二批次），堵不住「单批次内部提前返回」。

#### 修复：任务账 + 代次握手 + 锁内分配

- **`tasks_left` 任务账**取代 `workers_done`：每完成一件任务就原子减一，**归零才是本批结束**——账目记的是「还剩几件活」而非「跑了几趟」，与并发度/窃取次数解耦。
- **每批 `batch_epoch` 代次握手**：每批分配一个单调递增的代次；**每个 worker 每批只「进入」一次**（进入时核验代次，防止把上一批的残留计数算进本批），并按代次做完成广播。
- **同一把 `pool->mutex` 收口**：`next_index` 的索引分配（任务窃取）、任务数组快照、代次校验**全部在锁内**完成——worker 只能在锁内认领「本批 + 本代」的任务，**跨代串批**被结构性排除。
- **契约写入头文件**（`include/thread_pool.h`）：`thread_pool_batch` 返回 ⟺ (i) 本批 count 个任务**全部执行完**（函数指针已返回） **且** (ii) 本池无 worker 仍站在「本批之前的任务数组」上。因此**调用方在返回后释放 `tasks`/`th_tasks` 安全；返回前不得释放**。头文件同时保留 `THREAD_POOL_BUSY(-2)`（池忙时本次不执行任何任务、由调用方串行降级）语义。
- **`hop_propagated` 原子化**（`src/dialog_system.c`）：070「问题 B」登记的 `(*task->hop_propagated)++` 非原子共享自增改为 `__sync_fetch_and_add(..., 1)`。它参与「本跳零传播即 `break`」（`dialog_system.c` 本轮结束判定）的判据，非原子自增会让判据漏判/误判。

### ② R1' — 对话路径 worker 生命周期（`src/dialog_system.c`，共 6 处，含 `:692`）

- 对 `node->activation` 的**裸写**（无任何同步）改为持 `node_locks[node_id & (PM_NODE_LOCK_COUNT - 1)]`（`PM_NODE_LOCK_COUNT = 256` 分片）的**单锁临界区**。
- 用的是仓库既有节点级锁池（与 `boost_connection_weighted` 同一分片键），**不引入新锁域、不参与锁序**——单锁临界区天然不可能 ABBA。
- 覆盖 070 登记的 `:112`（读 `node->activation`）/`:124`（写 `node->is_visited`）/:826（复位）等站点所在的批内竞争。

### ③ R2a — 新锁域：master 激活账本分片锁（`include/multi_topology.h`、`src/multi_topology.c`、`src/cognitive_controller.c`）

- **新增 `activation_locks[PM_TOPO_LOCK_COUNT=16]`**，分片键 `PM_TOPO_LOCK_IDX(topo_id) = topo_id & 15`。保护范围：
  - master 的 `active_topo_id` / `active_node_ids[]` / `activation_levels[]`；
  - `SubTopology` 的 `total_activations` / `recent_activation` / `avg_activation_value` / `last_used`（均按 `topo_id` 归属）。
- **二进制兼容**：锁数组**追加在 `MasterTopology` 结构体末尾**，已有字段偏移**一律不动**（`pthread_mutex_t` 数组不进中间）；创建/销毁路径逐片 `pthread_mutex_init` / `pthread_mutex_destroy`。
- **锁纪律（写入头文件锁序注释，新增「Level 1.5」）**：
  - **只允许单锁临界区**——与 `node_locks` 之间、与另一分片之间**永不同时持有**；故它不参与锁序、结构上不可能 ABBA。
- **`master_activate_node` 拆两个不嵌套临界区**：取锁 → 改账 → 放锁，再取下一把——过程中不跨锁持有。
- **`master_propagate_activation` 先放源分片锁再调 `activate_node`**：避免「持源分片锁去请求目标分片锁」的 ABBA（源与目标可能是不同分片）。
- 需求来源是 TSan 一手证据（同一批内多个 `dialog_topo_worker` 任务 + 主线程在 `thread_pool_batch` 里偷到的任务，并发写同一批账）——**这不是「批次互踩」，批次屏障修好也不会消失**，故必须独立加锁。

### ④ R2b — 读侧补锁（`src/nn/feature_learn.c`）

- `feature_learn_graph_smooth` 的**两条读路径**——**单线程快路径**与 **OpenMP 并行路径**——对 `node->edges[].weight` / `.confidence` 的读**按 `node_locks` 加锁**。
- 其**写侧**（`boost_connection_weighted`，`src/autonomic_learner.c`）此前**已持** `net->node_locks[]`（写侧本就合规）；本批在该文件**只补注释**固化「写侧合规、缺的是读侧，读侧在 `feature_learn.c`」，**无代码改动**。
- 读侧只持 1 把锁 → 不参与锁序、不可能 ABBA。至此该站点**两侧同步**。

### ⑤ 收口 — TSan 复验后的三处（`src/thread_pool.c`、`src/multi_topology.c`、`src/dialog_system.c`）

- **`src/thread_pool.c`**：屏障相关的读改为 `__atomic_load_n(ptr, __ATOMIC_ACQUIRE)`（配套原子写用 release 语义），保证「任务账归零」对其他线程可见。
- **`src/multi_topology.c` 的 `master->active_topo_id`**：它是**全局单字段**，无法被「按 topo_id 分片」的锁真正保护——用分片锁护单字段属**设计踩空**（同一字段落在某一分片、却代表全局语义）；改为**原子访问**，与分片锁并存。
- **`src/dialog_system.c:177`**：补一处节点锁（此前遗漏的裸访问点）。

## 验证

### x86_64 / G15-WSL（gcc 15.2、glibc 2.43）

- **TSan：0 条报告**（两份日志**均 0**，去重后剩余清单为**空**）。
- **跨树对照（同一探针/同一测试）**：未修复树 **12 条**（OMP=20）/ **11 条**（OMP=1）、v0.5.26 树 **117 条**、基线 **41 条**。
- **覆盖度正控（gcov 逐行）**：证明**单线程**与 **OpenMP** 两条路径**分别**被跑到——「0 报告」不是「没跑到」造成的假阴性。
- **ASan**：与 v0.5.26 **逐项一致、无新增帧**；旧的 `dialog_topo_worker` **heap-use-after-free 消失**。

### aarch64 / armbian

- 终态干净全量构建 `all`：**0 warning / 0 error**。
- 测试构建：**14 warning（全在 `tests/integration/test_integration.c`）无新增**、0 error。
- 全套测试：**PASS = 23 / FAIL = 0 / TOTAL = 23**。

### N17 行为不变（回归正控）

- 修复树编译的二进制与**round 3 之前**编译的旧二进制，扩散**账行**输出**逐字节相同**（**sha256 一致**）——证明本批只加同步、不改扩散语义（含 v0.5.26 的强度闸门行为）。
- 不变量脚本 `tests/round3/n17_ledger_invariants.py`：**`bad = 0`**。

### 验证边界的诚实声明（须连同结论一并采信）

1. **TSan 日志末尾的 `nested bug in the same thread, aborting.` 是退出期伪影**——**新旧日志都有**、返回码**恒为 66**，**不是本版消除的东西**，不应当作「本版修复」的证据。
2. **高熵 ASLR 下的 TSan 采信边界**：TSan 运行中出现过 `WARNING: ThreadSanitizer: memory layout is incompatible, possibly due to high-entropy ASLR`，此类警告下 TSan 报告**需谨慎采信**；据此，本版「0」的结论以 **ASan + 跨树对照 + 覆盖度正控** **三方交叉**为准，TSan 原文仅作佐证。
3. **`test_cognitive_controller` 几乎没有断言**——全文件 `assert` 数为 **0**（332 行），它「通过」的**说服力很弱**（它恰是 v0.5.26 在 x86_64 下崩的那一支）；aarch64 的 23/23 **不能**作为该路径「没问题」的证据。
4. **锁开销未实测**——读侧每节点多一次加/解锁，量级 `O(node_count) × 3`，**本次未拿到耗时数据**，故**不宣称「无性能影响」**。

### 门禁与可复现性

- 新增**批次契约探针** `tools/probe_batch_contract.c` + `make probe-batch-contract`：**只编 `src/thread_pool.c`**，**不进 `libpivotmind.a`**、**不参与 `all` / `test` / `asan-test`**（Makefile 注释已固化该隔离，并记录了本仓 `-MD -MP` 的构建坑规避方式）。
- 探针实测（**契约正控**）：修复后 **58/58 迭代全 HOLDS、0 违约**；**同一探针在旧树上 40 违约**（v0.5.26）／**38 违约**（基线）——这是 `batch()` 完成契约「旧不成立、新成立」的直接证据。
- 复验脚本集中在 `tests/round3/`（G-T1 批次契约 / G-T2 TSan / G-T3 ASan / G-T4 armbian 全量 / G-T5 N17 账行不变量），产物目录 `tests/round3/artifacts/`。

## 红线声明（本版一律未动）

按作者架构红线，本版**未触碰**以下任何逻辑：

- 限边（每节点出边上限）；
- 截断（候选/结果截断）；
- 周期性稀疏化；
- 跨拓扑上限；
- 队列满时丢弃任务的策略。

并且：**本批只加同步与生命周期，未使用**降低并发度 / 缩小批量 / 休眠退避等**掩盖竞争**的手段。即本版不改变作者设定的架构约束，也不以牺牲并发为代价换「干净」。

## 已知未修问题

- **锁开销未实测**（见「验证边界的诚实声明」第 4 条）：读侧每节点多一次加/解锁（量级 `O(node_count)×3`），本次未取耗时数据；如需性能结论，须另跑基准。
- **TSan 退出期伪影 `nested bug in the same thread, aborting.`**（见第 1 条）：非本版可消除项，新旧日志一致。
- **`test_cognitive_controller` 无断言**（见第 3 条）：仍应补断言，使该用例的「通过」具备证据力；这是**测试债**，本版未动。
- **`hops_propagated` 之外的其余「批内共享自增/裸访问」**：本批按 TSan 复验清单逐点收口，但收口依据是「复验报告为空 + 三方交叉」，**不宣称全仓已无任何并发缺陷**——后续若 TSan/ASan 换配置再出新报告，须另立批次。

## 修改文件

| 文件 | 类型 |
|------|------|
| include/pivotmind_version.h | 版本 bump 0.5.27 |
| include/thread_pool.h | R1：`thread_pool_batch` 完成契约收紧 + `THREAD_POOL_BUSY` 语义 |
| include/multi_topology.h | R2a：`PM_TOPO_LOCK_COUNT`/`PM_TOPO_LOCK_IDX` + `activation_locks[16]`（追加末尾）+ 锁序注释 Level 1.5 |
| src/thread_pool.c | R1：`tasks_left` 任务账 + `batch_epoch` 代次握手 + 锁内索引分配/快照/代次校验；收口：屏障读 `__atomic_load_n(ACQUIRE)` |
| src/dialog_system.c | R1'/R2：6 处 `node->activation` 裸写改持节点锁、`:177` 补锁、`hop_propagated` 原子化 |
| src/multi_topology.c | R2a：分片锁创建/销毁、`master_activate_node` 两不嵌套临界区、`master_propagate_activation` 先放源分片锁；收口：`active_topo_id` 原子访问 |
| src/cognitive_controller.c | R2a：共享激活状态加锁 |
| src/nn/feature_learn.c | R2b：`feature_learn_graph_smooth` 单线程 + OpenMP 两读路径按 `node_locks` 加锁 |
| src/autonomic_learner.c | R2b：仅注释（写侧已合规，缺的是读侧） |
| tools/probe_batch_contract.c | 新增：批次契约探针（只编 `thread_pool.c`，不进库） |
| tests/round3/README.md | 新增：round 3 复验门禁说明 |
| tests/round3/artifacts/.gitkeep | 新增：产物目录占位 |
| tests/round3/artifacts/README.md | 新增：产物目录说明 |
| tests/round3/n17_ledger_invariants.py | 新增：N17 账行不变量脚本 |
| tests/round3/negative-controls/README.md | 新增：负控说明 |
| tests/round3/run_g_t1_probe.sh | 新增：G-T1 批次契约探针执行 |
| tests/round3/run_g_t2_tsan.sh | 新增：G-T2 TSan 复验 |
| tests/round3/run_g_t3_asan.sh | 新增：G-T3 ASan 复验 |
| tests/round3/run_g_t4_armbian.sh | 新增：G-T4 armbian 全量构建/测试 |
| tests/round3/run_g_t5_n17_ledger.sh | 新增：G-T5 N17 账行不变量 |
| Makefile | 新增 `probe-batch-contract` 目标（隔离于 all/test/asan-test） |
| CHANGELOG.md | 本版本条目 |
| changelogs/071-round3-concurrency-activation-locks.md | 本发布说明 |
