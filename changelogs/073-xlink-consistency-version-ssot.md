# v0.5.29 — 跨链一致性整批（加载期 id 映射 / 孤儿分类 / 走边判空 / 剪枝重映射 / 压缩）+ 版本号单一真值源 + 两道回归护栏

> **日期**: 2026-09-12 | **类型**: 修复 / 工程化
> **状态**: **已提交（7 笔）+ 本轮文档收尾** —— 权威树 `/home/cx/pm-fix` HEAD `6b668fd`，工作区在本轮开始前为 **clean**（`git status --porcelain` 空）。7 笔改动**均已 `commit` 并推送**（`origin/forge` 均为 `6b668fd`，见 `xlink-hygiene-raw.md` §8）。**本文件 + 根 `CHANGELOG.md` 顶部条目 + `include/pivotmind_version.h` bump 至 `0.5.29`** 是本轮**工作区改动（未提交）**，由上级统一提交。
> **来源**: ① `fix-plans/tail-edge-1-accounting.md`（25 KB）② `fix-plans/tail-edge-1-idmap.md`（14 KB）③ `fix-plans/batch-step2-consistency.md`（33 KB，§4 运行期数字已填）④ `fix-plans/batch-step3a-nullcheck.md`（5 KB）⑤ `fix-plans/batch-step3b-remap.md`（5 KB）⑥ `fix-plans/xlink-hygiene-raw.md`（32 KB，原始输出）+ `fix-plans/edge-consistency-batch-plan.md`（53 KB，规格）⑦ `fix-plans/version-ssot.md` + `evidence-version-ssot.txt` ⑧ `fix-plans/lock-guards.md`（37 KB）⑨ `fix-plans/tail-prune-1.md` / `tail-prune-1-baseline.md` ⑩ `fix-plans/step2-consistency.md`、`step3a-raw.md`、`step3b-raw.md`。
> **说明**: 本文件只写文档。**每个结论都注出处**（报告文件名或 `提交:行`）；**数字一律从报告抄，不新造**。凡报告标注「待复核 / 未做 / 边界」的，本文照实转载，不放宽。

## 概述

v0.5.28（`changelogs/072-*.md`）记的是「**状态闸门 + v9 + 降维 256 + 自死锁**」三条线，但那批改动**并带的五条尾巴与之后落的 6 笔**当时均未入账。本版把这 6 笔（加 `f280cfa` 并带的尾巴）一次补齐，主线仍是**把「静默」变成「会喊」**——只是这一批的对象换成了**跨链引用**：从「越界就悄悄丢、错位就静默指错、踩到空槽就崩、剪枝后旧 id 就失配」变成「**能映射、能分类计数、能判空跳过、能重映射、能压缩**」。

| # | 改动 | 位置 | 一句话 | 提交 |
|---|------|------|--------|------|
| ① | **跨链一致性整批（5 笔）** | `src/multi_topology.c` / `include/multi_topology.h` / `src/associative_reasoning.c` | 加载期把**文件 id 换算成内存 id**（清掉 1470 条界内静默指错）、把越界引用**分类记账**（A/B/C/D/E）、走边**判空跳过** NULL 槽、剪枝重编号后**重映射** `cross_links`、并把 `cross_links` **压缩**到无洞且 `link_index` 一致 | `d31a212` / `a16b74a` / `12271b9` / `d9a871f` / `6b668fd` |
| ② | **版本号收为单一真值源 + 秒级门禁** | `tools/version_common.py` / `tools/sync_version_docs.py` / `tools/check_version_consistency.py` / `Makefile` / 三份活文档 | `README.md` / `README.zh-CN.md` / `ARCHITECTURE.md` **不再手写版本串**，由生成器从 `include/pivotmind_version.h` 幂等改写，秒级 gate 把守并接进 `make test` | `d8b9828` |
| ③ | **两道回归护栏（v0.5.28 教训制度化）** | `tests/tools/check_lock_discipline.py` / `tests/longrun/run_longrun_guard.sh` / `Makefile` | 锁纪律**静态检查**（秒级，治「读锁内取写锁」）+ **opt-in 长跑监护**（≥900 tick，治「分钟级才现形的死」） | `25b2bdc` |
| ④ | **`f280cfa` 并带的五条尾巴（072 未记）** | `src/multi_topology.c` / `src/feature_io.c` / `src/hippocampus.c` / `tools/merge_states.py` | 剪枝额度改「每拓扑 2%」+ 跨链越界**记账** + 防呆基线「只升不降 + 报警」+ `feature_io.c` 丢弃记账 + `hippocampus.c` 锁配对 + `merge_states.py` 以文件头 `feat_dim` 为准 | `f280cfa` |

四者独立成条：① 治跨链引用的**正确性**（丢 / 指错 / 崩 / 失配 / 虚报），② 治**版本号漂移**（活文档与真值源不一致，且无门禁），③ 把 v0.5.28 的**两条教训**变成可回归护栏，④ 补记 072 **漏记但已随 `f280cfa` 落地**的五条尾巴。

## 核心变更

### ① 跨链一致性整批（5 笔，按加载→运行→剪枝的时序）

#### ①-a 加载期：把「文件 id」换算成「内存 id」（`d31a212`，报告 `tail-edge-1-idmap.md`）

- **病根**：加载期种子副本路径会把 `huarong_net_add_node` 只写 `concept_hash`、**不写 `node_hash`** 的重复概念**再建一份副本** ⇒ 内存 id 相对文件 id **整体后移 +7**（报告 §⑤：`{7: 1123}`）。于是 `cross_links[]` 里的**文件 id 被当成内存 id 直接用**，每条 `to` 引用一律指向内存里「下一个」概念 ⇒ **界内但静默指错**。
- **改法**：加载时维护局部映射表 `xlink_f2m[32]`（键 = 文件 `node_id`，值 = 内存 `node_id`），Pass-1 落地节点时**写映射**、跨链段校验**前先查映射换算**再走**原有**范围判据；查不到仍 `continue` 丢弃。`xlink_f2m` 为**局部变量，不进结构体 / 头文件 / ABI**（报告 §①）。as-found 有一处 `xlink_idmap_free` 定义未用（`-Wunused-function` 1 条告警 + 真泄漏），已补两个出口各一行 `free`，重编后 **0 告警**（报告 §②，**由上级抽验发现**，纪律：改动不许引入新告警）。
- **效果（最硬的一条，报告 §⑤）**：**界内静默指错 1470 条（占界内 99.4%）→ 0**；`live_A` 另**救回 10 条**边（45.0% → 44.7%）。
- ⚠ **诚实边界**：**头条越界比例 48.8% 纹丝不动**——因为 48.8% 里绝大部分是**两端 id 在文件里根本不存在**的引用（§①-c 的 A/B/C/D 残差），映射表**原理上无从换算**。故「48.8%→≈0%」的验收口径**被证伪**，报告 §⑧ 明写：**这是上级的假设错了，不是方案 A 失败**；方案 A 的价值在 1470 条静默指错清零，而非头条数字。

#### ①-b 加载期：跨链孤儿**分类记账**（`a16b74a`，报告 `batch-step2-consistency.md`）

- **前身**：`f280cfa` 先把越界引用从「静默丢弃」改为「**记账**」（`xlink_seen` / `xlink_oob_total` + 前 10 条明细 + 收尾汇总；报告 `tail-edge-1-accounting.md` §①：`state_after` = **1411/2890 = 48.8%**、`live_A` = **1333/2959 = 45.0%**；配平 `2890−1411=1479=TOTAL_LINKS`）。
- **本步**把 `xlink_oob_unmapped` 拆成 **A/B/C** 并新增 **D/E**（判据 R1–R4 见 `edge-consistency-batch-plan.md` §2.2）：
  - **A**（两端都不存在）/ **B**（仅 to 不存在）/ **C**（仅 from 不存在）/ **D**（两端都存在但换算后越界 = 不变量违例）/ **E**（段提前终止）。
  - 汇总行形态（§2.4）：`[状态加载] 警告：跨链引用孤儿丢弃 1411/2890 (48.8%)：A(两端不存在)=332 B(仅to不存在)=1079 C(仅from不存在)=0 D(不变量违例)=0 E(段提前终止)=0，前 10 条已打印`——**零孤儿时完全安静**。
- **实测（armbian，报告 §4.1/4.2）**：
  - `state_after.dat`：`A=332 B=1079 C=0 D=0 E=0`，`(from,to)` 分布 `(0,1)=955 (8,1)=435 (8,0)=21`（**三项相加 = 1411**）——与规格预期**逐位一致**（§4.4 V6 ✅）。
  - `live_A`：`A=254 B=1069 C=0 D=0 E=0`，孤儿合计 **1323/2959 = 44.7%**（比记账半的 45.0% 低，正是 `d31a212` 救回 10 条的缘故）。
  - 健康文件 `pivotmind_state.dat.copy`（PRE 3860）：**stderr 零新增行**（§4.3）。
  - 验收 12 项（V2–V9）**全 ✅**，三支单测 **12/12**、`check_lock_discipline.py` **PASS**（§4.4）。
- ⚠ **边界**：`state_after` 实测 `TOTAL_LINKS=1478`（配平值 1479，差 **−1**）、`live_A` 实测 `1635`（对比第 1 步前基线 1626，**+9**）——**隔离对照证明「本步对加载结果零影响」，两处差异均非本步引入**，指向已知的 `loaded_links`/`cross_link_count` **去重口径差**，**原因未定案**（报告 §4.5，登记为待办）。**C 类恒为 0 属预期，不要去「修」控制流**（§4.5）。

#### ①-c 运行时：走边**判空跳过** NULL 跨链槽（`12271b9`，报告 `batch-step3a-nullcheck.md`）

- **病灶**：`master_prune_cross_links`（`:548-571`）丢链时只 `free(link); master->cross_links[i] = NULL;`——**不压缩 `cross_link_count`、不重建 `cross_adj[].link_index`** ⇒ 「界内但指向空槽」的 adj 条目**合法且长期存在**。而 `topology_walk_greedy_impl`（`multi_topology.c:2082`）在 `entry->link_index < master->cross_link_count` **界内判**之后**直接解引用 `link`、且同一 else-if 链后面还会再解引用**（§0）。
- **改法（`patches/step3a-nullcheck.patch`，44 行 / 3 hunk）**：`if (!link) { xlink_null_skipped++; entry = entry->next; continue; }` —— **整链短路**（注释写明为何不能只写 `link &&`）；唯一返回路径前**仅当有跳过才** `LOG_DEBUG`。另三处同类消费点（`:489` / `:1662` / `:3125`）**本就判空**，只需补这一处。
- **正反证（armbian 探针）**：
  - **反证**：未改版**真崩** —— `CTL_EXIT=139`（`Segmentation fault`），`<<< topology_walk_greedy 返回` **从未打印**（`batch-step3a-nullcheck.md` §3）。
  - **正证**：改后 `FIX_EXIT=0`、`len=3`、`path = 0 1 2`，并打出 `[DEBUG] multi_topology.c:2379: [跨拓扑走边] 跳过 NULL 跨链槽位 2 次`（§4）。
- 回归：`make libpivotmind.a` exit 0 / 0 告警；三支单测 **12/12**；`check-locks` **PASS**（§5）。
- **政策点（留给作者，一行改动）**：`master_prune_cross_links:568` 的 INFO 只报「剪掉 N 条」，**未提示「留下 N 个 NULL 洞」**——3a 兜的正是这个后果，要不要补一句（§6）。

#### ①-d 剪枝后：重编号时**重映射** `cross_links`（`d9a871f`，报告 `batch-step3b-remap.md`）

- **真 bug：「判越界」排在了「查表」前面**（§1）。改前 `xlink_remap_topo()`：
  ```c
  3642:  if (nid >= nc || !sub->net->nodes[nid]) drop = 1;   /* ← 先用【压缩后】的 nc 判死 */
  3643:  else if (remap) { ... ni = remap[nid]; ... }
  ```
  `nc` 是**压缩后**的节点数，`cross_links[]` 存的是**重编号前**的旧 id ⇒ **合法链被提前判死**（`remap[199]=196` 是活节点，却被 `199 >= 197` 判死）。
- **探针实测（§0/§3）**：
  - ctl（未改）：悬垂端点 `PRE 2/14 → POST#1 6/14 = 42.9%`（剪枝把 4 个端点改坏）；逐条 = **真丢 2 + 误丢 2**。
  - fix（本步）：`PRE 2/14 → POST#1 0 → POST#2 0`（0.0%）；守恒配平 `5+2=7` ✔、幂等第 2 轮 `removed=0` 保住 5 / 丢 0 / 改写 0 ✔。
  - 日志：`[跨链重映射] topo=0: 节点 200→197, 重映射端点 10 个, 丢弃跨链 2 条`（10 = 5 条存活链 × 2 端）。
- **改法**：新增 helper `xlink_map_endpoint()`——**有 remap 时先查表、后判越界**；`remap[old] == -1` 才判真悬垂；`remap == NULL`（本轮无删除）⇒ 恒等，只丢「**可证**悬垂」。
- ⚠ **必须纠正的上一轮结论（§4）**：上一轮报的「第 2 轮幂等 FAIL、跨链 7→1 被静默吃掉」**不成立**——是**旧探针两处缺陷**（`snap_compare` 用「第 k 条非 NULL」对齐有洞数组 ⇒ 整体错位；汇总行在 `destroy()` 之后读垃圾）造成的**测量伪影**。用新探针回跑改前代码，第 2 轮同样 0 丢弃 / 0 改写 ⇒ 幂等本身没问题；真实情况是跨链 7 → **5**（真丢 2 + 误丢 2）。
- **待拍板语义点 5 条（§5）**：是否压缩（作者已拍「压缩」）、`remap` 按槽位分配的内存开销、是否需要 **id 世代号（epoch）** 校验、丢弃是否落 metrics、早退分支语义是否统一。

#### ①-e 压缩 `cross_links` 并保持 `link_index` 一致（`6b668fd`，报告 `xlink-hygiene-raw.md`）

- **问题**：`master_prune_cross_links` **只 free + 置 NULL、不压缩** ⇒ 数组**有洞**，`cross_link_count` 含洞数、`cross_adj[].link_index` 存的是**原始下标**。后果：消费点读到「已删除链」的位置上是**别的活链**（**静默指错**），`master_get_system_status:3424` 的 `total_links` **虚报**。
- **改法**：新增 `master_compact_cross_links_nolock()`（非 NULL 前移 + `link_id=新下标` + 扫 `cross_adj[]` 重建 `entry->link_index` + `cross_link_count=存活数`），在**两个产洞口**调用：① `master_prune_dead_nodes_nolock`（3b remap 扫描之后，`:4031`）；② `master_prune_cross_links`（`:659`）。另给 `associative_reasoning.c:189` 与 `multi_topology.c:965` 两处**无界内判定**的直接索引**补上判界**（§1.2）。
- **三变体探针（pre / half=故意跳过重建 / fix，armbian 实跑）**：
  - **pre**：`洞=3`、`total_links=12` vs 真实活跃 9 ⇒ **虚报 +3**；`PHASE4` 幂等 FAIL。
  - **half**（只前移、不重建 `link_index`）：**读错链 12 条**、`cross_link_exists` **违反 9/12**、`PHASE7` 污染 12 处 ⇒ **反证「不重建 link_index = 静默指错」**。
  - **fix**：`洞=0`、`total_links=9` 变真、**读错链 0**、`cross_link_exists` **违反 0/12**、幂等 OK。
- 体量：patch `184 行 / 7 hunk`（3 文件），`dry-run exit=0`，`apply exit=0`；严格编译自比 **0 新增告警**；三支单测 **12/12**；`check-locks` **PASS**（§3–§7）。
- ⚠ **登记遗留**：`remove_cross_topology_link`（`topology_growth.c:1000-1029`）`free` 后**原地左移**数组却**不重编 `link_id`、不动 `cross_adj`**（第三种破契约路径），但**零调用者 = 死代码**，运行期不产洞，**不在本任务两个产洞口之内**（§1.2，登记为遗留项）。

### ② 版本号收为单一真值源 + 秒级门禁（`d8b9828`，报告 `version-ssot.md`）

- **病根**：活文档（`README.md` / `README.zh-CN.md` / `ARCHITECTURE.md`）**手写**版本串，与真值源 `include/pivotmind_version.h` 各改各的。`tests/README.md:302` 自记：「`ARCHITECTURE.md` 抬头一度落后 **28 个小版本**（`v0.5.0` vs `v0.5.28`）而无人察觉」。
- **改法（三件套 + 接线）**：
  - `tools/version_common.py`（218 行）：真值源解析 + **4 种版本串锚点**（shields.io badge URL / 正文当前版本句 / 指标表版本行 / 架构抬头）+ CHANGELOG「陈旧断言」规则，`sync` 与 `check` 共用。
  - `tools/sync_version_docs.py`（115 行）：**幂等生成器**，把活文档里的锚点改写为真值源版本；⛔ 历史（`changelogs/**` / `CHANGELOG.md` 历史节 / `docs/**`）不改。
  - `tools/check_version_consistency.py`（72 行）：**秒级门禁**，不一致即逐条打印 `文件:行 现值=… 期望=…` 并 `exit 1`。
  - `Makefile` 加 `sync-version` / `check-version`，并把 **`check-version` 接进 `make test`**（`Makefile:333-337`，紧随 `check-locks` 之后）。
- **规则要点（`check_version_consistency.py` 文档串）**：门禁只认「**声明当前版本**」的锚点；**历史节天然豁免**；另有一条「**陈旧断言必须带更正注记**」规则只管 `CHANGELOG.md` 的**最新一个发布节**——若该节提到 `pivotmind_version.h` 却写着过期版本串，则须在该节内有 `更正（YYYY-MM-DD）` 且写明当前真值版本的注记。
- **本版动作**：真值源 `include/pivotmind_version.h` 由 `0.5.28` **bump 至 `0.5.29`**（`PIVOTMIND_MAJOR/MINOR/PATCH` = `0/5/29`）；活文档由 `make sync-version` 从真值源**幂等改写**；`make check-version` 复核 **PASS**（原始输出见本文件「验证」节 ②）。
  > **更正（2026-09-12）**：`include/pivotmind_version.h` 的真值源现已为 **`0.5.29`**，活文档三份 / 锚点 7 处由生成器同步一致；本版此前的 `0.5.28` 为 dev 期中间值，非当前断言。

### ③ 两道回归护栏（`25b2bdc`，报告 `lock-guards.md`）

> 直接回应 v0.5.28 的两条教训：**「秒级的测试抓不住分钟级的死」** + **「纯自死锁 TSan 不报、只跟着挂住」**。

- **护栏 ①：锁纪律静态检查器（秒级）** —— `tests/tools/check_lock_discipline.py`（**15,560 B**，纯标准库，不编译 / 不运行，可进 CI）；`make check-locks` 等价直跑。输出每个命中的 `文件:行` + 所在函数 + 读锁区间 + 被调函数 + 写者的写锁取锁点，末尾 `LOCK-DISCIPLINE: PASS` / `FAIL (N 处)`，**退出码 0 = PASS、非 0 = FAIL**。**已接进 `make test`**（`Makefile:327-332`），每次全量测试顺手跑一遍（报告 §1.1）。
- **护栏 ②：长跑监护（opt-in，约 15 分钟）** —— `tests/longrun/run_longrun_guard.sh`（**9,874 B**，`chmod +x`，`bash -n` 通过）；`make longrun GATEWAY=<pivotmind_gateway>`。默认 `--minutes 17 --tick-target 900 --stall-tol 90`，**断言 tick 越过 600 后持续增长到 ≥900**；**刻意不进 `test` / `test-fast`**（它**会把网关真跑起来**，须在允许运行产物的机器上跑；脚本自带内存守卫，受限验证机上直接拒跑）。停止引擎**一律 `kill -9`**（不触发退出存盘、不污染沙箱）；沙箱数据目录默认 `$HOME/pm-lockguard/<tag>`，**绝不指向线上数据目录**；令牌**永不打印**（一律 `[REDACTED]`）（报告 §1.2）。
- **边界**：护栏 ② 提供的是**可回归的监测能力**；报告 §3 给出该轮实测，但**「修复后永不崩溃」这一断言仍不成立**（v0.5.28 报告 §6.3 边界未放宽：**未做小时级长稳观察**）。

### ④ `f280cfa` 并带的五条尾巴（072 未记，本轮补记）

> 072 的「已知未修问题」把 `TAIL-LOCK-1` / `TAIL-ACCT-1` / `TAIL-TOOL-1` 登记为**未修**，但这三条（连同另两条）**实际上已随 `f280cfa` 落地**（`git show --stat f280cfa` 含 `src/feature_io.c` / `src/hippocampus.c` / `tools/merge_states.py`）。此处按「已修」补记。

| 尾巴 | 072 状态 | 实际 | 落地证据 |
|------|----------|------|----------|
| **`TAIL-PRUNE-1` 额度半** | 072 未记 | **已修 `f280cfa`** | `max_remove` 由全局 `_total_nodes/50+1` 改为**本拓扑 `nc/50+1`**（`multi_topology.c:3882`）；3/9/11 拓扑单轮删除量由 **6.10% / 18.10% / 22.10%** → **恒 2.10%**（`tail-prune-1.md` §3） |
| **`TAIL-EDGE-1` 记账半** | 072 未记 | **已修 `f280cfa`**（后被 `a16b74a` 细分） | 越界引用记账 `1411/2890 = 48.8%`、`live_A 1333/2959 = 45.0%`，配平 `2890−1411=1479=TOTAL_LINKS`（`tail-edge-1-accounting.md` §④） |
| **`TAIL-PRUNE-1` 基线半** | 072 未记 | **已修 `f280cfa`** | 防呆基线 `g_last_saved_nodes` 改「**只升不降**」（`master_baseline_raise`，`multi_topology.c`），显式加载仍可升可降；慢速流失 **47.5%**：改前 **0 告警**（防线失效）→ 改后 **7 告警**，首报第 24 轮 **40.17%**（恰在既有 60% 线上）；正常剪枝 B1/B2 **均 0 误报**（`tail-prune-1-baseline.md` §4/§5） |
| **`TAIL-ACCT-1`** | 072 记「未修」 | **已修 `f280cfa`** | `src/feature_io.c` 维度不匹配由**静默 `return -1`** 改为 `LOG_ERROR("[特征持久化] 拒绝加载 %s: 特征维度不匹配 (期望=%d, 文件=%d)")`（`git show f280cfa -- src/feature_io.c`） |
| **`TAIL-LOCK-1`** | 072 记「未修」 | **已修 `f280cfa`** | `src/hippocampus.c` 把 `pthread_rwlock_unlock(&hc->topology->rwlock)` **移进 `if (vocab && vocab->net)` 作用域内**，只解锁本线程真正加过的读锁（`git show f280cfa -- src/hippocampus.c`；附注释「移出本层或上提 rdlock 都会再次破坏配对，勿改」） |
| **`TAIL-TOOL-1`** | 072 记「未修」 | **已修 `f280cfa`** | `tools/merge_states.py` 的 `NODE_FEATURE_DIM` 由硬编码 `24` 改为 **`256`**（真值源兜底），并改为「**优先以文件头读到的 `feat_dim` 为准**」（`fmt_ver>=5`）/「节点内声明为准」（`fmt_ver<=4`），缺失/非法才退兜底并**显式记账** `_fallback_note`（`git show f280cfa -- tools/merge_states.py`） |

> **072 与本版的口径差**：072 的 `TAIL-LOCK-1` / `TAIL-ACCT-1` / `TAIL-TOOL-1` 条目**内容属实、结论已过期**（历史原文保留、不涂改）；本版按「已修 + 提交号 `f280cfa`」更新，并在「已知未修问题」节逐条标注划掉 / 保留。

## 改动文件

`git show --stat`（7 笔提交）逐笔如下：

| 提交 | 文件 | 变更体量 |
|------|------|----------|
| `d31a212` | `src/multi_topology.c` | `+122 / −3` |
| `a16b74a` | `src/multi_topology.c` | `+138 / −4` |
| `12271b9` | `src/multi_topology.c` | `+21` |
| `d9a871f` | `src/multi_topology.c` | `+125 / −1` |
| `6b668fd` | `include/multi_topology.h` / `src/associative_reasoning.c` / `src/multi_topology.c` | `+127 / −2`（3 文件） |
| `d8b9828` | `tools/version_common.py`（新）/ `tools/sync_version_docs.py`（新）/ `tools/check_version_consistency.py`（新）/ `Makefile` / `README.md` / `README.zh-CN.md` / `ARCHITECTURE.md` / `CHANGELOG.md` / `tests/README.md` | 9 文件 |
| `25b2bdc` | `tests/tools/check_lock_discipline.py`（新，379 行）/ `tests/longrun/run_longrun_guard.sh`（新，228 行）/ `Makefile` / `tests/README.md` | `+696 / −4`（4 文件） |
| `f280cfa` | 19 文件（含 `changelogs/072-*.md` 与 `CHANGELOG.md`；功能面见本节 ④ 表） | `+638 / −62` |

**本轮（写 changelog 这一手）**：只新建 / 修改下面 3 个文件，⛔ 未碰 `src/` / `tests/` / `Makefile`，⛔ 无 git 写操作：

| 文件 | 动作 |
|------|------|
| `changelogs/073-xlink-consistency-version-ssot.md` | **新建**（本文件） |
| `CHANGELOG.md` | 顶部**新增** v0.5.29 条目（历史节不动） |
| `include/pivotmind_version.h` | `PIVOTMIND_VERSION "0.5.28"→"0.5.29"`、`PIVOTMIND_PATCH 28→29` |

## 验证

> 口径：以下均为**报告 / 原始输出里能读到的数字**，逐条注明来源；**读不到的一律标「未做 / 待复核」**。

### ① 跨链一致性整批

- **加载期 id 映射（`d31a212`，`tail-edge-1-idmap.md`）**：界内静默指错 **1470（99.4%）→ 0**（§⑤）；位移直方图 **`{7: 1123}`**（§⑤）；`live_A` 救回 **10 条**（45.0%→44.7%，§③④）；构建对照 ctl 0 告警 / cand 修复后 0 告警（§②）。
  ⚠ 报告 §③④/§⑤ 数值为「**离线数据 + 已校准模型**」口径（校准见其 §③④ 前提），armbian 实跑那一手记在其 §⏳；本 changelog 照实转载。
- **孤儿分类（`a16b74a`，`batch-step2-consistency.md`）**：`state_after` **A=332 B=1079 C=0 D=0 E=0**、`xlink_seen=2890`、孤儿合计 **1411**、比例 **48.8%**、`(from,to)` 分布 **955 / 435 / 21**；`live_A` **A=254 B=1069 C=0 D=0 E=0**、合计 **1323/2959 = 44.7%**；健康文件 **零新增行**；三支单测 **12/12**、`check-locks` **PASS**（§4.1–4.4）。**配平** `2890−1411=1479`。
  ⚠ `state_after` 实测 `TOTAL_LINKS=1478`（−1）、`live_A 1635`（+9）**非本步引入**，隔离对照已证（§4.5）。
- **走边判空（`12271b9`，`batch-step3a-nullcheck.md`）**：ctl `CTL_EXIT=139`（段错误）→ fix `FIX_EXIT=0` + `跳过 NULL 跨链槽位 2 次`（§3/§4）；`make libpivotmind.a` exit 0 / 0 告警；三支单测 **12/12**；`check-locks` **PASS**（§5）。
- **剪枝重映射（`d9a871f`，`batch-step3b-remap.md`）**：ctl 悬垂 **6/14 = 42.9%** → fix **0**（§0/§3）；守恒 `5+2=7`、幂等第 2 轮 0 丢弃 / 0 改写（§3）；`make libpivotmind.a` exit 0 / 0 告警；三支 **12/12**；`check-locks` **PASS**；patch `154 行 / 3 hunk`，`dry-run exit=0`（§3）。
- **压缩（`6b668fd`，`xlink-hygiene-raw.md`）**：pre `洞=3` / `total_links` 虚报 +3 / 幂等 FAIL；half **读错链 12**、`cross_link_exists` **违反 9/12**、污染 12 处；fix `洞=0` / `total_links=9` 变真 / **读错链 0** / **违反 0/12** / 幂等 OK（§§PHASE0–7）。patch `184 行 / 7 hunk`、`dry-run exit=0`、`apply exit=0`（§3/§6）；严格编译自比 `MT CC=0 warnings=4`（4 条为既有，无新增）、`AR 0`（§7）；三支单测 **12/12**（§5）；`check-locks` **PASS**（§4）。

### ② 版本号单一真值源（本轮实测）

- **bump 前**（`0.5.28`，`python3 tools/check_version_consistency.py`）：`VERSION-CONSISTENCY: PASS  真值源 include/pivotmind_version.h = 0.5.28；活文档 3 份 / 锚点 7 处一致；CHANGELOG 最新节无未更正的陈旧断言`（`EXIT=0`）——与 `d8b9828` 落地后状态一致。
- **bump 后**（`0.5.29`）：真值源改毕、活文档经 `make sync-version` 同步，`make check-version` **PASS**——**原始输出见 `fix-plans/changelog-v0529.md`**（本文件不复制，避免二次转写误差）。

### ③ 两道护栏（`25b2bdc`，`lock-guards.md`）

- 交付物体量：`check_lock_discipline.py` **15,560 B**、`run_longrun_guard.sh` **9,874 B**（报告 §1.1/§1.2）。
- `make check-locks` 已接进 `make test`；`make longrun` 需 `GATEWAY=<二进制路径>`；脚本选项与默认值见报告 §1.2。
- ⚠ **边界**：护栏 ② 的「永不崩溃」结论**不成立**（与 v0.5.28 报告 §6.3 同口径，未做小时级观察）。

### ④ 尾巴五条（来源见「核心变更 ④」表；数字出处逐条已在表中标注）

### ⚠ 本版未做 / 未取得

- **未部署**：7 笔改动虽已提交并推送，但**未部署**到任何线上服务；且 `fmt_ver=9` 载荷仍会被状态闸门**有意拒绝**，部署前置未解除（v0.5.28 报告「部署前置」节）。
- **未做跨架构 / 跨 libc 交付依据**：`d31a212`/`a16b74a`/`12271b9`/`d9a871f`/`6b668fd` 的验证**均在 armbian（aarch64）**；**x86_64 + ASan/UBSan/TSan 那条腿本轮未跑**；`d9a871f` 报告 §6 明写「只走探针路径，未在真实数据上端到端跑『剪枝 → 存盘 → 重载』」。
- **未做整仓 `make`**：各条只做单 TU / `libpivotmind.a` 级别构建（报告各自 §未做）。

## 红线声明

按作者架构红线，本版 7 笔改动**均未触碰**以下任何逻辑：**限边（每节点出边上限）、截断（候选/结果截断）、周期性稀疏化、跨拓扑上限、队列满时丢弃任务的策略**。

并且：

- **跨链一致性整批**只改「**映射 / 计数 / 判空 / 重映射 / 压缩**」，**不引入启发式修复、不拒绝加载**（孤儿一律 `continue` 丢该条，不 `return -1`）；`cross_link_count` / `link_id` 的既有语义在压缩后**重新成立**（`link_id == 下标`）。
- **版本号 SSOT** 只改「**活文档由生成器改写 + 门禁**」，**不改历史节**（`changelogs/**` / `CHANGELOG.md` 历史节 / `docs/**` 天然豁免）。
- **护栏**为**新增测试工具**，不改任何产品代码路径。
- 本 changelog 本身**只新建 `changelogs/073-*.md`、在根 `CHANGELOG.md` 顶部加一条摘要、并把 `include/pivotmind_version.h` bump 到 `0.5.29`**，**不改任何 `src/` / `tests/` / `Makefile`，不改 `changelogs/` 下已有文件，不做任何 git 写操作**。

## 已知未修问题

> 承接 v0.5.28（`changelogs/072-*.md`）。**已修的标「已修 + 提交号」，未修的保留。**

### 072 登记条目的更新

- **`TAIL-LOCK-1`** —— ✅ **已修（`f280cfa`）**。`src/hippocampus.c` 的 `unlock` 已移进 `if (vocab && vocab->net)` 作用域内，只解锁本线程真正加过的读锁（`git show f280cfa -- src/hippocampus.c`）。
- **`TAIL-ACCT-1`** —— ✅ **已修（`f280cfa`）**。`src/feature_io.c` 维度不匹配由静默 `return -1` 改为 `LOG_ERROR`（含文件名 / 期望 / 实际维度）。
- **`TAIL-TOOL-1`** —— ✅ **已修（`f280cfa`）**。`tools/merge_states.py` 改为「**以文件头读到的 `feat_dim` 为准**」，兜底 `NODE_FEATURE_DIM = 256` 并**显式记账**（`_fallback_note`）。
- **`emergent_pos.bin` 静默错读风险** —— ⛔ **仍未修（保留）**。文件头只校验 magic + version、**无维度字段**，降维不改 version ⇒ 旧（512）文件会被新（256）二进制**静默接受并错读**。**当前只是部署时把旧文件移开了，代码未加防线**。建议（属产品改动，**未实施**）：给头加 `feat_dim` 字段并按值校验，或把 version 提到 2 使旧文件被显式拒收。
- **「版本号未 bump」** —— ✅ **已被取代**。`f280cfa` 已 bump 至 `0.5.28`；`d8b9828` 把版本号**收为单一真值源 + 秒级门禁**（`check-version` 接进 `make test`）；本版 bump 至 **`0.5.29`**。该条**不再以「未 bump」形态存在**，改为由门禁自动把守。

### 本版新登记（未修 / 未定案）

- **加载结果 ±1 / +9 去重口径差未定案**（`batch-step2-consistency.md` §4.5）：`state_after` 实测 `TOTAL_LINKS=1478`（配平值 1479，−1）、`live_A 1635`（对比基线 1626，+9）；**隔离对照已证非本步引入**，指向 `loaded_links` / `cross_link_count` 的去重口径差，**原因未定案**。
- **`remove_cross_topology_link` 是第三种破契约路径**（`xlink-hygiene-raw.md` §1.2）：`free` 后原地左移数组却**不重编 `link_id`、不动 `cross_adj`**；**零调用者 = 死代码**，运行期不产洞，**未在本批范围内**。
- **剪枝重映射的 5 条语义点待作者拍板**（`batch-step3b-remap.md` §5）：`remap` 按槽位分配的内存开销、是否需要 **id 世代号（epoch）** 校验（真实数据里 30.2% 那批可能含「指向存活但错误节点」的历史错位引用，**本轮无法识别**）、丢弃是否落 metrics / 状态持久化、早退分支语义是否统一。
- **`master_prune_cross_links` 的 INFO 未提示「留下 N 个 NULL 洞」**（`batch-step3a-nullcheck.md` §6，一行改动，待拍板）。
- **验证覆盖面**：本版 7 笔**未跑 x86_64 + ASan/UBSan/TSan**、**未做端到端真实数据演练**（`d9a871f` 仅探针路径）、**未部署**、**未做整仓 `make`**。技能明载「**armbian 绿了不能单独作交付依据**」。
- **072 承接的其他未清尾巴仍在**：`TAIL-GATE-6`~`TAIL-GATE-11`、`TAIL-RED-1`~`TAIL-RED-5`、`TAIL-PERF-1`（锁开销正式基准仍未测）、N17 账行不变式未在本轮重跑等（见 `072-state-gate-dim256-deadlock.md`「已知未修问题」节，本版未处理）。

### 部署前置（承接，仍有效）

- 真实 `fmt_ver=9` 载荷共 **4 份副本**（含 armbian `~/pivotmind/pivotmind_state.dat`，5,176,666 B），**新状态闸门会「有意拒绝」它们** ⇒ 直接部署 = 「启动即拒绝状态加载、空壳运行」。**须先同步写入端到 v9，或等批 1 的 v10 读端一起上**，**不可用开关绕过**（开关对未来版本无效）。**本版未改变该结论。**
