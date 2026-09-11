# v0.5.26 — 数据保命、并发与生命周期、门禁与诚实、N17 扩散上限（全量基线审查修复轮 2）

> **日期**: 2026-09-10 | **类型**: 修复 / 优化
> **来源**: 全量基线审查（19 P0 / 26 P1）+ 修复轮 2
> **提交**: `5d6c9b1`（相对基线 `3eb2e6e`，35 个文件 `+1442 / −437`）

## 概述

本版是把一份**全量基线审查报告（19 个 P0 / 26 个 P1）**落地的修复轮 2。改动横跨持久化、并发/生命周期、测试门禁、扩散算法与验证期暴露的既有缺陷五个方向，按五批推进：

| 批次 | 主题 | 要点 |
|------|------|------|
| ① | **数据保命** | 种子存盘/加载门卫、footer 完整性、死索引停用、原子写 + fsync、四处"0 节点存盘"门卫 |
| ② | **并发与生命周期** | 关闭期 UAF（网关连接线程槽表 + join）、init 线程 join、learn 队列守卫、共享池批次闸门、线程池 shutdown 守卫、共享状态加锁、`thread_started` 旗标 |
| ③ | **门禁与诚实** | CI 真正执行测试、ASan 单一来源 + `make asan-test`、`detect_leaks=1`、数值断言、桩测试清出、文档措辞如实化 |
| ④ | **N17 扩散上限** | 删除 `SPREAD_MAX_EXTRA=256` 与栈数组上限，改按强度闸门裁决 |
| ⑤ | **验证期发现的既有缺陷** | `pthread_join(0)`（基线自带）、`node_cache` 脏指针、`calloc` 泄漏、测试自身 `strdup` 泄漏 |

一批共同的目标是：**让"失败不再被静默吞掉"**——加载失败不再被空状态覆盖、线程句柄无效不再被当成有效去 join、CI 不再"只编译不跑测试"、验证结论不再含糊其辞。

## 核心变更

### ① 数据保命（第一批）

#### G2 — 种子加载失败不再被空状态覆盖（`src/memory_system.c`）

- 旧行为：种子加载失败后流程继续走空状态初始化，并在后续保存时把空状态回写覆盖原文件——**"读不出就抹掉"**，一次加载失败即导致长期记忆整体丢失。
- 现行为：加载失败即**拒绝回写**，保留原种子文件，损坏可见、可人工抢救。

#### D2 — 种子 footer 完整性校验，缺 footer 默认拒绝（`src/memory_system.c`）

- 0 字节文件无法证明任何完整性 → 明确返回 -1，拒绝加载（合法空种子应是 16 字节仅 footer）。
- 尾部残字节 1~15：损坏，拒绝。
- 尾部 16 字节非 `PMSEED2` footer：损坏，拒绝。
- **无 footer 的旧格式（v0.5.24 及更早）默认拒绝**——无哈希可证其完整性；确需迁移的，须显式设置 `PIVOTMIND_ALLOW_LEGACY_SEED=1` 才按旧格式一次性接受，日志明确标注"按旧格式加载"。
- 判定改为对 `tail` 的**完备**分支（`tail==0` / `1..15` / footer 校验），不留下"两分支都不命中 ⇒ 隐性接受"的空档。

#### D3 — 死索引 `index_map` 停用（`src/memory_system.c`）

- 全仓核查：`index_map` 在本仓**只写不读**——`create`/`store`/`destroy` 之外零引用。
- 风险：驱逐路径存在 `index_map[index_size]` 的堆越界写。
- 修复：停止维护该冗余索引（恒空、`index_size` 恒 0，与实际占用一致）；`index_map` 字段保留以兼容结构布局，注释固化"本仓无读方"的前提。
- 明确未采用的方案：驱逐时对 `index_map` 全量重建——仅在确有读方时才值得做。

#### A-P1-2 — 种子原子写：`fsync(file) + fsync(dir)` + 唯一 tmp 名（`src/memory_system.c`）

- 临时名加入 pid + 单调序号，避免并发/重入保存互踩同一 `.tmp`。
- 顺序修正为 **`fflush` → `fsync(file)` → `fclose` → `rename`**：旧顺序下 rename 可能在内容尚未落盘时已生效，断电后留下半写文件。
- 补**目录 fd 的 `fsync`**（`open(dir, O_RDONLY|O_DIRECTORY)`），确保 rename 后的目录项本身落盘。
- 同时补 `getpid`/`access` 等所需头文件。

#### A-P1-5 — `emergent_pos` 原子写（`src/emergent_pos.c`）

- 同样 tmp + rename + `fsync(file)/fsync(dir)`，临时名带 pid + 序号。
- **逐写检查返回值**（`EP_WRITE` 宏包裹每次 `fwrite`）：磁盘满或写失败时清理 tmp、返回 -1，不再在写失败的情况下谎报"持久化完成"。

#### D1 — 四处"0 节点存盘"门卫（`demos/gateway_system.c`）

- 与既有"拓扑 total>=20"门卫对等；种子的真正"内容"是 LTM 条目数，故门卫看**条数**而非字节：`MEMORY_SEED_MIN_ENTRIES = 1`，条目数低于下限即视为空状态，**不回写覆盖**。

### ② 并发与生命周期（第二批）

#### C1 — 关闭期 UAF：网关连接线程槽表 + join（`demos/pivotmind_gateway.c`）

- 旧行为：每连接线程无法被 join，关闭时只能"轮询计数 15s 然后强拆 main 栈上的 `gw`"；慢连接线程随后访问已释放内存 → **关闭期 use-after-free**。
- 修复：新增连接线程**登记槽表**（`g_conn_mutex` 保护；`used` = 槽当前占用，`ever` = 该槽句柄尚未 join）；主循环内做**机会式回收**（已结束但未 join 的槽在锁外 `pthread_join`，先复制 tid、锁内清 `ever`，避免持锁 join 死锁）；关闭路径对所有 `ever==1` 的槽逐个 join，join 返回即代表线程彻底离开 `gw`。

#### H2 — init 线程 join（`demos/gateway_system.c`）

- 初始化线程此前无人 join：关闭时它可能仍在加载，并在关闭之后重建 worker（而没人再 join 它们）。
- 修复：关闭路径显式 join init 线程；join 返回后才进入后续 destroy。前置条件（此时不能有连接线程在跑）由 C1 的逐槽 join 自检。

#### C3 — learn 队列生命周期守卫（`demos/gateway_learn.c`）

- 关闭已开始（`g_learn_q.stop` 已置位）后**绝不再入队**。
- `learn_queue_shutdown` 只 join **创建成功**的 worker 槽：旧实现无条件 join 全部槽，`pthread_create` 失败时槽是零值 `pthread_t`，`join(0)` 属未定义行为（旧注释与代码相反，已更正）。

#### C4 — 共享线程池批次闸门（`src/thread_pool.c`、`include/thread_pool.h`、`src/dialog_system.c`、`src/multi_topology.c`）

- 单例池同一时刻只允许一个批次；池忙时 `batch()` **立即返回 `THREAD_POOL_BUSY(-2)`** 且不执行任何任务，不做阻塞等待。
- 调用方（`dialog_system.c`、`multi_topology.c`）收到 `-2` 后**串行降级**（自行串行执行），消除两处并发提交互串批次。
- `THREAD_POOL_BUSY` 的正式定义收口到头文件，`src/thread_pool.c` 不再另写一份。

#### 线程池 shutdown 守卫 + 共享状态加锁

- 批次进行中不响应 `shutdown`：worker 完成本批并上报 done，由 `batch()` 正常收尾。
- 批次状态（tasks/task_count/next_index/workers_done）重置移入锁内。

#### B-P1-2 — `learning_scheduler` 引入 `thread_started` 旗标（`include/learning_scheduler.h`、`src/learning_scheduler.c`）

- 以 `thread_started` 旗标（而非对象地址）判定线程句柄是否有效，决定 `stop` 能否 join。
- `stop` 幂等：重复 stop（含 destroy 内部那次）直接返回，避免 `join(0)`。
- `autonomic_learner.c` 同构处理：`flush_started` 只在 create 成功之后置位。

### ③ 门禁与诚实（第三批）

- **CI 真的跑测试了（E-P1-A）**：原 "Run tests" step 调用的 `make test-*`（Makefile:248-271）是**纯构建别名、从不执行**——15 个测试即使必挂也让 CI 变绿。现改为 `make test`（构建 + 执行 + 汇总 + 退出码门禁）。
- **ASan 门禁收敛（E-P1-12）**：ASan 旗标收敛为**单一来源**（`Makefile` 的 `ASAN_CFLAGS`/`ASAN_LDFLAGS`），新增 `make asan-test`，CI 只调它；覆盖 `-DHAS_OPENSSL` 出货代码路径，消除"CI 绿 ≠ 本地 asan 绿"。
- **`detect_leaks=1`**：`ASAN_OPTIONS` 原含 `detect_leaks=0`、**关闭了 LeakSanitizer**，与 069 宣称不符；现改为 `1`。
- **数值断言**：`matrix_multiply_naive` 入口补 `memset`（此前对 `tensor_create` 的未置零缓冲做 `+=`）；`tests/unit/test_tensor.c` 补逐元素数值断言（`{9,12,9,12}`，注释给心算过程）。
- **桩测试清出**：删除 8 行 Hello World `test_io.c`；`test_chinese.c` 改写为 UTF-8/CJK 真断言（不再需要 windows.h，CI 不再跳过）；`tests/scratch/test_tensor_broadcast.c` 移入 `tests/unit/` 并接线到 `make test`——即此前**闲置的广播测试接进 CI**。
- **测试目标集统一**：`test:` 前置与 `TEST_BINS` 逐项一致；`test-integration`/`test-semantic-growth`/`test-tensor-broadcast` 全部纳入。
- **文档措辞如实化**：把验证结论中"验证过"的表述标注为"**仓外临时程序、不可复现、不受 CI 保护**"，同步 `changelogs/069` 的验证章节。
- 附带：`_sample_negative` 补 `vocab->size <= 5` 循环上限守卫（原会死循环）与空指针守卫；`lr_reduce_on_plateau` 过滤非有限 `val_loss`；`model_io.c` 加载畸形文件补 `ndim` 上界与尺寸回绕守卫。

### ④ N17 扩散上限（第四批，`src/diffusion.c`）

- **删除 `SPREAD_MAX_EXTRA=256` 与栈数组 `spread1_ids[256]` 上限**（及其"线性扫降级"分支）。
- 改为**每跳累加全部入边贡献后按强度闸门裁决**：
  - `SPREAD_COMPETE_BAND = 0.15f`（竞争带宽：相对本跳最强贡献的比例，无量纲 → 与图规模/权重绝对值无关）；
  - `SPREAD_SALIENCE_FLOOR = 0.001f`（显著性下限，仅排除数值死值，与既有 `0.01f`/`0.001f` 门槛同量级）；
  - 生效阈值 `θ = max(0.001, 0.15 × 本跳峰值)`，强度 `>= θ` 者点亮。
- 消除旧实现的"**首边独占、顺序即命运**"——旧版在 256 上限内先到先得，边的遍历顺序直接决定谁被点亮。
- **实测**：候选 2000 = 点亮 1000 + 落选 1000，点亮数不再被 256 卡住；把边顺序**整体对调**后，账行（点亮数 / 落选数 / 峰值 / θ）**逐字节相同**，顺序无关性成立。

### ⑤ 验证期发现的既有缺陷（第五批）

#### `autonomic_stop_async_flush` 用 `state->initialized` 当存活判据 → `pthread_join(0)`（`src/autonomic_learner.c`）

- `state->initialized` 只表示状态对象可用，**并不代表 flush 线程句柄有效**；据此判存活会对未创建/已回收的句柄调用 `pthread_join`，即 `join(0)`。
- 表现：**glibc 2.43 下 SIGSEGV、glibc 2.39 下静默返回 `ESRCH`**（后者正是它长期未被察觉的原因）。
- 归属：**基线自带**，`ab1f79e`（2026-06-04）引入，非本版引入。
- 修复：改用 **create 成功之后才置位的 `flush_started` 旗标**判定（与 `learning_scheduler` 的 `thread_started` 同构）。
- 与本版验证的直接关联：x86_64/G15-WSL 上 `test_cognitive_controller` 崩溃即由此缺陷造成，修复后该用例不再崩溃。

#### `master_topology_create` 从不初始化 `master->node_cache`（`src/multi_topology.c`）

- `malloc` 不置零，字段是**脏指针**；而全仓使用点都写成 `if (master->node_cache)` 的守卫形式 → 非 NULL 垃圾值让守卫失效，直接解引用野指针（ASan 下 `diffusion.c` 解引用 `node_cache->auto_thaw_ok` 已实测 SEGV）。
- 修复：显式初始化 `node_cache = NULL`、`ext_dict = NULL`、`cognitive_state_ptr = NULL`，并将保留 int `_pad_parallel_mode` 置零，使本结构创建后不再存在任何未初始化字段（三个运行时注入型指针的注入点已在注释中列明）。

#### `multi_topology.c` 每步 `calloc` 被剪枝 `break` 跳过 `free`（`src/multi_topology.c`）

- `path_target_weights` 原先在循环体内每步 `calloc`、在循环体末尾 `free`；但循环内存在多个 `break` 退出点（剪枝 / 语义场休止 / 候选耗尽），一旦从 `break` 退出就跳过 `free` → **库侧内存泄漏**。
- 修复：分配上提到循环外单次 `calloc`，每步以 `memset` 复位等价替代 `calloc` 的置零（每步语义不变），在函数**唯一返回路径**释放——任何 `break` 都只离开循环、无法绕过 `free`。

#### `tests/unit/test_memory.c` 自身 `strdup` 未释放（`tests/unit/test_memory.c`）

- 测试代码自身的泄漏，已补 `free`（否则在 `detect_leaks=1` 下会污染 LeakSanitizer 结果）。

## 验证

### aarch64 / armbian（glibc 2.39）

- `make -j2 all` **exit 0**。
- 全套测试 **23/23 通过**。
- ASan（`detect_leaks=1`）**5/5 干净、零泄漏**。

### x86_64 / G15-WSL（gcc 15.2、glibc 2.43）

- ASan **5/5 干净**。
- 全量测试 **22/23**：`test_cognitive_controller` 崩溃。
  - 已定位为 `autonomic_stop_async_flush` 的 `pthread_join(0)`（见第五批）——**本版已修**。

### 待办与可复现性（如实声明）

- 069 中那批"记忆种子保存/加载 20 项断言"仍来自**仓外临时测试程序**（不在本仓库、不可复现、不受 CI 保护）；本版已把该缺口如实标注（第三批"文档措辞如实化"），并继续登记为待办：将这些断言固化为仓内测试。
- 本版未新增任何"仅口述、不可复现"的验证结论。

## 红线声明（本版一律未动）

按作者架构红线，本版**未触碰**以下任何逻辑：

- 限边（每节点出边上限）；
- 截断（候选/结果截断）；
- 周期性稀疏化；
- 跨拓扑上限；
- 队列满时丢弃任务的策略。

即：本版只做"保命、并发正确性、门禁与诚实、扩散闸门"层面的修复，不改变作者设定的架构约束。

## 已知未修问题

- **`heap-use-after-free`（基线自带）**：`src/dialog_system.c:149` `dialog_topo_worker` —— 主线程 `dialog_reasoning_create` 释放批次任务时，worker 仍在读该批任务。
- **本版未修**。正在做下一批（**round 3**）方案。
- 本条如实记录，**不作"已修"，也不含糊带过**。

## 修改文件

| 文件 | 类型 |
|------|------|
| include/pivotmind_version.h | 版本 bump 0.5.26 |
| include/autonomic_learner.h | 第五批：`flush_started` 旗标 |
| include/common.h | 第三批：`init_random_seed` / `init_random_from_env`（P2-2） |
| include/learning_scheduler.h | 第二批：`thread_started` 旗标（B-P1-2） |
| include/thread_pool.h | 第二批：`THREAD_POOL_BUSY(-2)` 收口 |
| src/memory_system.c | 第一批：G2 / D2 / D3 / A-P1-2 |
| src/emergent_pos.c | 第一批：A-P1-5 原子写 + 逐写检查 |
| src/thread_pool.c | 第二批：C4 批次闸门 + shutdown 守卫 + 锁语义 |
| src/learning_scheduler.c | 第二批：`thread_started`（B-P1-2） |
| src/autonomic_learner.c | 第五批：`flush_started` 判据修复 |
| src/multi_topology.c | 第二批 C4 降级 + 第五批：`node_cache` 初始化、`calloc` 泄漏 |
| src/cognitive_controller.c | 第二批：共享状态加锁 |
| src/diffusion.c | 第四批：N17 扩散上限改为强度闸门 |
| src/health_monitor.c | 第二批：生命周期/状态守卫 |
| src/train_mode.c | 第二批：共享状态加锁 |
| src/nn/matrix_ops.c | 第三批：`matrix_multiply_naive` 入口 `memset` |
| src/nn/model_io.c | 第三批：`ndim` 上界 + 尺寸回绕守卫（P2-1） |
| src/nn/pretrain.c | 第三批：`_sample_negative` 循环上限 + 空指针守卫 |
| src/nn/scheduler.c | 第三批：`lr_reduce_on_plateau` NaN 过滤（P2-3） |
| src/nn/tensor.c | 第三批：数值断言相关 |
| demos/pivotmind_gateway.c | 第二批：C1 连接线程槽表 + join |
| demos/gateway_system.c | 第一批 D1 门卫 + 第二批 H2 init join |
| demos/gateway_learn.c | 第二批：C3 生命周期守卫 |
| demos/gateway_handlers.c | 第二批：并发/生命周期相关 |
| demos/gateway_http.c | 第二批：并发相关 |
| demos/digital_life.c | 随动调整 |
| Makefile | 第三批：CI/ASan 单一来源 + `make asan-test` + `TEST_BINS` |
| .github/workflows/ci.yml | 第三批：CI 真正执行测试 + ASan 门禁收敛 |
| tests/unit/test_tensor.c | 第三批：逐元素数值断言 |
| tests/unit/test_chinese.c | 第三批：UTF-8/CJK 真断言 |
| tests/unit/test_memory.c | 第五批：自身 `strdup` 泄漏 |
| tests/unit/test_io.c | 第三批：删除 8 行桩测试 |
| tests/unit/test_tensor_broadcast.c | 第三批：由 scratch 移入并接线 CI |
| changelogs/069-code-review-optimization-round.md | 第三批：验证可复现性措辞如实化 |
| CHANGELOG.md | 本版本条目 |
