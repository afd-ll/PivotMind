# Changelog

## v0.5.30 — 2026-09-13

> 来源：**路径可移植化三段式全部落地（6 笔）+ `digital_life` 复活（1 笔）+ v0.5.29 之后落的 7 笔尾巴**，共 **14 笔**（`6dacf75` / `84176c0` / `1128813` / `d3b30dd` / `7995028` / `0c7a960` / `0c172a2` / `faf7655` / `1a2e873` / `0b6a3fd` / `9fd28f7` / `f500ef2` / `ab1853e` / `2e0bb19`），均已 `commit`；权威工作区 `/home/cx/pm-fix`（Pi 3B）HEAD `2e0bb19`，分支 `feat/paths-callsite-migration`，`main` 未动。完整发布说明（含逐条证据、诚实边界、红线声明、已知未做）见 [changelogs/074-paths-ssot-three-steps.md](changelogs/074-paths-ssot-three-steps.md)。主线是**把「数据文件落在哪儿」从散落各处的字符串字面量与 `getenv` 收成一个唯一真值源，并让目录「默认自建」**。

### Added

**① 路径 SSOT 模块（第 1 步，`faf7655` + `1a2e873`，4 文件 / +975 −5）**
- 新增 `include/pivotmind_paths.h`（86 行）+ `src/pivotmind_paths.c`（323 行）：`pm_home()` / `pm_dir()` / `pm_file()` / `pm_log_path()` / `pm_path()` / `pm_data_path()` / `pm_ensure_dirs()`。
- 三条定稿契约：**纯查询**（不碰文件系统、永不失败；`pm_dir(非法 which)` ⇒ `NULL`，绝不「悄悄返回 home」）；**无状态 ensure**（`pm_ensure_dirs()` 是唯一写文件系统的入口，不设缓存、不加锁、`EEXIST` 幂等）；**零分配**（`pm_path()` 缓冲区由调用方提供，模块不 `malloc`）。
- 目录族 6 位 `PM_DIR_{HOME,DATA,LOG,SESSION,RUN,CORPUS}`；数据文件登记表 12 位 `PM_FILE_*`（11 个数据文件 + `gw_token`）；`PM_FILE_ALL = 0xFFF`。
- `pm_home()` 解析顺序（`pthread_once` 解析一次并缓存）：`$PIVOTMIND_HOME` → `$HOME/pivotmind` → 编译期 `PM_HOME_DEFAULT`；任一非法值 ⇒ 视为未设置 + **一条 WARN（不许静默）**。
- `1a2e873`：Makefile 改为传**裸 token** `-DPM_HOME_DEFAULT_PATH=/var/lib/pivotmind`，引号在头文件里由 C 补齐 ⇒ 无论经过几层 shell 都是字符串字面量；并加硬门（必须是绝对路径，否则 `$(error …)`）。
- 同笔新增 `tests/unit/test_paths_unit.c`（526 行）。

### Changed

**② 调用点全量替换（第 2 步，`0b6a3fd` + `9fd28f7` + `f500ef2`，累计 32 文件 / +285 −80）**
- `0b6a3fd`（10 文件 / +129 −18）：`src/` 层 8 文件 13 处改走 `pm_file()` / `pm_log_path()`（brainstem、autonomic_learner、health_monitor、train_mode、emergent_pos、cognitive_controller、prefrontal_executive、json_config）。**门卫逻辑原样保留**。
- `9fd28f7`（22 文件 / +102 −57）：`demos/` 25 处 + `tools/` 30 处；`pivotmind_gateway.c` 接 `pm_ensure_dirs(PM_DIR_ALL)`；加 `__OPTIMIZE__` 守卫。
- `f500ef2`（2 文件 / +54 −5）：`tests/test_pfe_unit.c` 收编到私有 `PIVOTMIND_HOME`（`mkdtemp` + `setenv` + `pm_ensure_dirs`，**必须早于任何 `pm_*` 调用**，因为 `pm_home()` 只解析一次并缓存）；`demos/digital_life.c` 的 `main` 补 `pm_ensure_dirs(PM_DIR_ALL)`。
- **架构决策**：数据文件落点从「相对 CWD」改为 `<home>/data/`；`chdir` 保留但只管语料相对路径 —— **CWD 决定读什么，SSOT 决定写哪儿**。

**③ `tools/` 层「默认自建」（第 3 步，`2e0bb19`，7 文件 / +70）**
- 7 个「会写 SSOT 路径」的工具在 `main` 开头补 `pm_ensure_dirs(PM_DIR_ALL)`，与 gateway / digital_life 同款：未就绪只逐条 WARN、**不拒绝运行**：`batch_learn` / `build_cross_links` / `compound_promote` / `corpus_train` / `hebbian_pretrain` / `qa_crawler` / `seed_builder`。
- **按设计不动**：纯只读工具（`batch_test` / `compare_templates` / `debug_load` / `debug_seed` / `eval_templates` / `path_analyze` / `probe_batch_contract` / `quick_chat` / `state_dump` / `seed_teacher`）—— 读不到就是没数据，不该产生 fs 副作用；路径全走 `argv` 的工具（`feed_cli` / `edge_builder` / `merge_state` / `reader` / `template_build`）—— 目录由调用方负责，不属「默认自建」范围。

### Fixed

**④ `digital_life` 复活（`ab1853e`，3 文件 / +260 −28）**
- `demos/digital_life.c` 仍在引用 `BackgroundClock`，而该类型早在 **`caa6e36`（V0.3.0，2026-06-11）** 就被 `Brainstem` 取代 ⇒ **对 v0.3.0 之后的树根本编译不过，坏了约 3 个月**。`Makefile` 里它有独立规则（`:117` / `:207` / `:224`）却**没进 `all`** ⇒ CI 与本地 `make all` 全都覆盖不到，**这是漏检的机制性原因**。
- 1:1 证据（非猜测）：`brainstem_create(MasterTopology*, MemorySystem*, CognitiveState*)` 与 `background_clock_create(topology, memory, cognitive_state)` **参数类型/顺序完全一致** ⇒ 纯符号替换、零适配；旧 `background_clock.h/.c` 里**完全没有 thalamus**，而 `brainstem.h` 的 `thalamus` 字段由 `calloc` 置 NULL、且所有 `thalamus_*` 公开函数都判空返回安全默认值 ⇒ **未绑丘脑是安全降级，不崩**。⇒ 映射到 brainstem 是**根因修复**（它是超集），摘除反而会丢掉昼夜节律。
- 顺带把 `digital-life` 加进 `all:`；`tests/unit/test_paths_unit.c` 由 **10 条补到 13 条**（+250 行）：`t11 pm_file`（12 项登记全量断言 + 与 `pm_path(DATA,name)` 交叉验证 + 指针稳定 + 两两互异 + `PM_FILE_ALL==0xFFF` + 非法位 ⇒ NULL）、`t12 pm_log_path` 默认分支、`t13 $PIVOTMIND_LOG_FILE` 覆盖分支（绝对路径采用 / 尾斜杠规范 / 空串静默回退 / 相对路径 WARN 回退 / 未设置回退）。
- ⚠ **诚实记录的行为差异**：旧 `BackgroundClock` 内部自建 `self_learner`；新版把自主学习收归丘脑 utility slot（`THAL_UTIL_SELF_LEARNER`），**未绑丘脑时该路径被跳过**。`digital_life` 未装配丘脑 ⇒ 其余节律（tick / 衰减 / 自发激活 / 认知状态漂移 / 周期存盘）与旧一致，自主学习仍由 `ActiveLearner` 承担。

**⑤ v0.5.29 之后落的 7 笔尾巴**
- `6dacf75` / `1128813`：删两个死函数 —— `remove_cross_topology_link`（破坏新契约、零调用者）与 `insert_cross_topology_link`（自首个提交起从未被调用）。
- `84176c0`：`emergent_pos.bin` 加**维度头**，并**拒绝**无法推断维度的文件（治 v0.5.28 降维 512→256 之后旧文件被静默错读的风险）。
- `d3b30dd`：`cross_hit_hash` 改用**无符号**算术（治溢出）。
- `7995028`：加载期把**被当作重复吸收**的跨链引用**记账**（续 v0.5.29「把静默变成会喊」的主线）。
- `0c7a960`：RNN 层的权重槽初始化 + 释放自身持有的张量。
- `0c172a2`：两份 README 挂 CI 状态，并写明 CI **覆盖什么、不覆盖什么**。

### Quality

- **双架构三机**（均为 `make debug`，即 `-O0`、无 LTO）：

| 机器 | 架构 / 编译器 | `make debug` | 7 工具逐个编译 | `make test` | 空 home 冒烟 |
|---|---|---|---|---|---|
| WSL | x86_64 / **gcc 15.2.0**（20 核） | rc=0，0 error，11 warning（全为既有），1s | 7/7 rc=0，0 error，**0 新增警告** | **27 通过 / 0 失败** | 6 目录自动建出（0700） |
| armbian-1 | aarch64 / **gcc 13.3.0**（6 核） | rc=0，0 error，13 warning（全为既有），3s | 7/7 rc=0，0 error，**0 新增警告** | **27 通过 / 0 失败** | 6 目录自动建出（0700） |
| astar728-1 (Pi 3B) | aarch64 / **gcc 14.2.0** | 不跑（905 Mi，`-O2`/LTO 会挂机） | `gcc -fsyntax-only -Wall -Wextra` 7/7 rc=0 | — | — |

- **`make test` 内含两道门禁且均 PASS**：`check-locks`（锁纪律静态检查）与 `check-version`（版本号一致性）。
- **「默认自建」的运行时铁证**（WSL + armbian 双份）：以空 `PIVOTMIND_HOME` 跑 `build_cross_links`，输出 `[paths] 数据根: <H>` 之后报 `× 找不到状态文件: <H>/data/pivotmind_state.dat` —— **路径正确落在 SSOT 的 `<home>/data/` 下，且目录先于它被建好**；`find -maxdepth 1` 显示 `corpus/ data/ log/ run/ session/` 五子目录 + home 全部 `drwx------`（0700）。
- **既有警告清单**（非本版引入，两架构一致）：`tools/build_cross_links.c:91` `unused variable 'CROSS_WEIGHT'`；`tools/corpus_train.c:185` `-Wformat-truncation`。

### Known Issues

- **🔴 新发现：6/7 工具不在构建系统里。** `Makefile` 的 `all: $(LIB_NAME) seed-builder debug-seed gateway digital-life` 只含 `seed-builder`；`corpus_train` / `qa_crawler` / `batch_learn` 有规则但**不在 `all`**；`build_cross_links` / `compound_promote` / `hebbian_pretrain` **连编译规则都没有**。⇒ 本版第 3 步的 7 个改动里，**只有 `seed_builder` 会被 CI / `make all` 编译**，其余 6 个靠本轮手工逐个编译覆盖。机制上与 ④ 同类（`digital_life` 坏了 3 个月没人发现）。**未做处理，留给作者拍板**（补规则 + 进 CI，还是明确标废弃）。
- **`tools/batch_learn.c:576`** 的 `char path[512]` + `snprintf(…, 511, …)` 装完整 SSOT 路径，理论可截断（低风险，宜放宽到 `PM_PATH_MAX`）。**未改。**
- **`include/json_config.h:83`** 注释「NULL 则尝试 pivotmind_config.json」已不准确。**未改。**
- **`tools/template_build.c`** 的输出文件默认仍是相对 CWD 的字面量 `pivotmind_state_with_templates.dat` —— **有意保留**（一次性产物，非跨进程共享文件），故第 3 步未给它插 `pm_ensure_dirs`。
- **部署前置（承接，仍有效）**：真实 `fmt_ver=9` 载荷会被状态闸门**有意拒绝** ⇒ 直接部署 = 「启动即拒绝、空壳运行」，**不可用开关绕过**；且数据文件落点已从 `<home>/` 改到 `<home>/data/`，部署时需 `mv <home>/*.dat <home>/*.bin <home>/data/`，否则空壳启动（旧文件不丢，只是被忽略）。
- **未做**：本版**未跑 ASan/UBSan/TSan**、**未做端到端真实数据演练**、**未部署**，也**未开 PR 到 `main`**（唯一能触发 CI 的途径 —— `ci.yml` 的 `push` 只监听 `main`/`master`/`develop`，推 feat 分支实测 `total_count: 0`）。

### Notes

- **版本说明**：本版真值源 = **`0.5.30`**（`include/pivotmind_version.h`）；`README.md` / `README.zh-CN.md` / `ARCHITECTURE.md` 三份活文档 / 7 处锚点由 `make sync-version` 生成器改写，`make check-version` **PASS**。历史节与 `changelogs/**` 不参与一致性判定。
- **红线声明**：本版只动「路径落点」+ 两个死函数 + 一处维度头 + 一处无符号算术 + 一处加载期记账；**限边 / 截断 / 周期性稀疏化 / 跨拓扑上限 / 队列满丢任务**逻辑一律未触碰；`pm_ensure_dirs()` 是**幂等无状态**的 `mkdir`（`0700`），**已存在的目录绝不 chmod**，只读环境下只报未就绪位、不拒绝启动。
- **新增文件**：`changelogs/074-paths-ssot-three-steps.md` + 根 `CHANGELOG.md` 顶部本条 + `include/pivotmind_version.h` bump 至 `0.5.30`。

---

## v0.5.29 — 2026-09-12

> 来源：**跨链一致性整批（5 笔）+ 版本号单一真值源 + 两道回归护栏 + `f280cfa` 并带的五条尾巴补记**，共 **7 笔提交**（`d31a212` / `a16b74a` / `12271b9` / `d9a871f` / `6b668fd` / `d8b9828` / `25b2bdc`）均已 `commit` 并推送，权威树 HEAD `6b668fd`。主线仍是**把「静默」变成「会喊」**：这一批的对象是**跨链引用**——从「越界就悄悄丢、错位就静默指错、踩到空槽就崩、剪枝后旧 id 就失配」变成「**能映射、能分类计数、能判空跳过、能重映射、能压缩**」。完整发布说明（含逐条证据、诚实边界、红线声明、已知未修问题）见 [changelogs/073-xlink-consistency-version-ssot.md](changelogs/073-xlink-consistency-version-ssot.md)。

### Fixed

**① 跨链一致性整批（`src/multi_topology.c` / `include/multi_topology.h` / `src/associative_reasoning.c`，5 笔）**
- **加载期 id 映射（`d31a212`）**：种子副本路径使内存 id 相对文件 id **整体后移 +7**，`cross_links[]` 拿文件 id 当内存 id 用 ⇒ **界内静默指错**。改为加载时维护局部映射表 `xlink_f2m`（键 = 文件 id，值 = 内存 id），校验前先换算；**界内静默指错 1470 条（99.4%）→ 0**，`live_A` 救回 **10 条**（45.0%→44.7%）。⚠ **头条越界比例 48.8% 纹丝不动**——残差两端 id 在文件里根本不存在，映射表原理上无从换算，「48.8%→≈0%」验收口径**被证伪**（上级假设错了，非方案失败）。
- **孤儿分类记账（`a16b74a`）**：把越界引用从「静默丢」改为分类 **A/B/C/D/E** + `(from,to)` 分布 + 前 10 条明细 + 收尾汇总（**零孤儿完全安静**）。实测 `state_after`：**A=332 B=1079 C=0 D=0 E=0**，**1411/2890 = 48.8%**；`live_A` **1323/2959 = 44.7%**；配平 `2890−1411=1479=TOTAL_LINKS`。
- **走边判空跳过 NULL 槽（`12271b9`）**：`master_prune_cross_links` 只 `free`+置 NULL、不压缩 ⇒ 「界内但指向空槽」的 adj 条目合法存在，`topology_walk_greedy_impl:2082` 直接解引用 ⇒ **真崩**（ctl `CTL_EXIT=139`）；改为**整链短路**跳过（fix `FIX_EXIT=0` + `跳过 NULL 跨链槽位 2 次`）。
- **剪枝重映射（`d9a871f`）**：真 bug「**判越界排在了查表前面**」——用**压缩后**的 `nc` 判**重编号前**的旧 id ⇒ 合法链被提前判死。改为**有 remap 时先查表、后判越界**；探针悬垂端点 ctl **6/14 = 42.9%** → fix **0**。
- **压缩 `cross_links`（`6b668fd`）**：新增 `master_compact_cross_links_nolock()`（非 NULL 前移 + `link_id=新下标` + 重建 `cross_adj[].link_index` + `cross_link_count=存活数`），在两个产洞口调用；探针 pre `洞=3`/`total_links` 虚报 +3 → fix `洞=0`/**读错链 0**/`cross_link_exists` **违反 0/12**。

**② 版本号收为单一真值源 + 秒级门禁（`d8b9828`）**
- 新增 `tools/version_common.py`（真值源解析 + 4 种版本串锚点）、`tools/sync_version_docs.py`（幂等生成器）、`tools/check_version_consistency.py`（秒级门禁）；`Makefile` 加 `sync-version` / `check-version`，并把 **`check-version` 接进 `make test`**。
- `README.md` / `README.zh-CN.md` / `ARCHITECTURE.md` 里「声明当前版本」的锚点**一律由生成器改写，不得手写**（历史节天然豁免）。背景：`ARCHITECTURE.md` 抬头曾**落后 28 个小版本**（`v0.5.0` vs `v0.5.28`）无人察觉（`tests/README.md:302`）。
- 本版真值源 `include/pivotmind_version.h` 由 `0.5.28` **bump 至 `0.5.29`**，活文档经 `make sync-version` 同步，`make check-version` **PASS**。

**③ 两道回归护栏（`25b2bdc`）**
- `tests/tools/check_lock_discipline.py`（15,560 B）：**锁纪律静态检查**（秒级，治「持 master 读锁期间取 master 写锁」），`make check-locks`，**已接进 `make test`**。
- `tests/longrun/run_longrun_guard.sh`（9,874 B）：**opt-in 长跑监护**（默认 17 分钟 / 断言 tick 越过 600 持续增长到 ≥900），`make longrun GATEWAY=…`，**刻意不进 `test`/`test-fast`**（会真跑网关）。

**④ `f280cfa` 并带的五条尾巴（072 未记，本版补记——已修）**
- **剪枝额度半**：`max_remove` 由全局 `_total_nodes/50+1` 改为**本拓扑 `nc/50+1`**；3/9/11 拓扑单轮删除量由 **6.10% / 18.10% / 22.10%** → **恒 2.10%**。
- **越界记账半**：跨链越界丢弃由静默改为记账（`1411/2890 = 48.8%`，配平见 ①）。
- **防呆基线半**：`g_last_saved_nodes` 改「**只升不降**」；慢速流失 **47.5%** 改前 **0 告警** → 改后 **7 告警**（首报 40.17%），正常剪枝 **0 误报**。
- **`TAIL-ACCT-1`**：`src/feature_io.c` 维度不匹配由静默 `return -1` 改为 `LOG_ERROR`。
- **`TAIL-LOCK-1`**：`src/hippocampus.c` 的 `unlock` 移进 `if (vocab && vocab->net)` 内，只解锁真正加过的读锁。
- **`TAIL-TOOL-1`**：`tools/merge_states.py` 改为以文件头 `feat_dim` 为准，兜底 `256` 并显式记账。

### Quality
- **跨链整批**：armbian 探针 **pre / half（故意跳过重建）/ fix** 三变体对照 + A/B 反证；压缩 patch `184 行 / 7 hunk`、重映射 `154 行 / 3 hunk`，**`dry-run exit=0`**；各条三支单测 **12/12**、`check-locks` **PASS**、编译自比 **0 新增告警**。
- **版本 SSOT**：`check-version` 秒级门禁接进 `make test`，活文档 3 份 / 锚点 7 处与真值源一致。
- **护栏**：`check_lock_discipline.py` 15,560 B、`run_longrun_guard.sh` 9,874 B，用法见 `tests/README.md`。

### Known Issues
- **已修（072 划掉）**：`TAIL-LOCK-1` / `TAIL-ACCT-1` / `TAIL-TOOL-1` → 均已随 **`f280cfa`** 修复；「版本号未 bump」→ 已由 `f280cfa` bump + `d8b9828` 门禁取代，本版 bump **`0.5.29`**。
- **仍未修（保留）**：**`emergent_pos.bin` 静默错读风险**——文件头只校验 magic+version、**无维度字段**，降维不改 version ⇒ 旧（512）文件会被新（256）二进制**静默错读**；**当前只是部署时把旧文件移开了，代码未加防线**。
- **新登记（未定案）**：加载结果 **±1 / +9 去重口径差**（`state_after` `1478` vs 配平 `1479`；`live_A 1635` vs 基线 1626）——隔离对照已证**非本步引入**，原因未定案。
- **新登记（未做）**：`remove_cross_topology_link` 是第三种破契约路径（零调用者 = 死代码）；剪枝重映射 **5 条语义点待作者拍板**（含是否需要 **id 世代号 epoch** 校验）；`master_prune_cross_links` INFO 未提示「留下 N 个 NULL 洞」。
- **验证覆盖面**：本版 7 笔**未跑 x86_64 + ASan/UBSan/TSan**、**未做端到端真实数据演练**、**未做整仓 `make`**、**未部署**。技能明载「armbian 绿了不能单独作交付依据」。
- **部署前置（承接，仍有效）**：真实 `fmt_ver=9` 载荷会被状态闸门**有意拒绝** ⇒ 直接部署 = 「启动即拒绝、空壳运行」；须先同步写入端到 v9 或等批 1 的 v10 读端，**不可用开关绕过**。

### Notes
- **更正（2026-09-12）**：`include/pivotmind_version.h` 的真值源现为 **`0.5.29`**；活文档三份 / 锚点 7 处由生成器同步一致，`make check-version` PASS。此前 `0.5.28` 为 dev 期中间值。
- **红线声明**：所有**限边 / 截断 / 周期性稀疏化 / 跨拓扑上限 / 队列满丢任务**逻辑，按作者架构红线本版**一律未触碰**；跨链整批只改「映射 / 计数 / 判空 / 重映射 / 压缩」，**不引入启发式修复、不拒绝加载**；版本 SSOT 只改活文档 + 门禁、不改历史节；护栏为新增测试工具。
- **本版只新建 `changelogs/073-*.md`、在根 `CHANGELOG.md` 顶部加本条、并把 `include/pivotmind_version.h` bump 到 `0.5.29`**；⛔ 不改 `src/` / `tests/` / `Makefile`，不做 git 写操作。

---

## v0.5.28 — 2026-09-11

> 来源：**本轮三条独立改动**（① 状态加载版本闸门 ② 特征维度 512→256 收进 main ③ 每 11 分钟自死锁修复），均已在 armbian 上做过真实验证。状态：**工作区改动（未提交、未部署）**，HEAD `efcf907`，**15 个文件 `+99 / −34`**。三条共同的主线是**把「静默」变成「会喊」**——静默丢数据 → 显式拒绝 + 记账；静默停摆 → 根因 + 长跑对照。完整发布说明（含逐条证据、诚实边界、红线声明、已知未修问题）见 [changelogs/072-state-gate-dim256-deadlock.md](changelogs/072-state-gate-dim256-deadlock.md)。

### Fixed

**① 状态加载版本闸门（`src/multi_topology.c` `master_load_state()`）**
- **旧代码对不认识的 `fmt_ver` 一律 `p = buf` 回退当 v1 猜解析**：5.17 MB 的真 `fmt_ver=9` 状态文件被静默读成「**完成: 1 节点, 0 链接**」、`LOAD_RC=1`、判 **ACCEPTED**，全程**零 ERROR 零 WARN**（事故口径 **3,860 节点无声灭失**）——违反铁律「丢弃必须记账」。
- 改为**显式三分支**：`fmt_ver > STATE_FORMAT_VERSION(=8)`（未来版本）→ `LOG_ERROR` + `return -1`，该判断**在开关判断之前，故 `PIVOTMIND_ALLOW_LEGACY_STATE` 对未来版本永远无效**；`fmt_ver < 1`（0/负数）→ 默认 `LOG_ERROR` + `return -1`；`fmt_ver == 1` → **补回 `p = buf`**（v1 无版本头，首 4 字节即首节点 `node_id`）+ `LOG_WARNING` 记账。
- 新增显式开关 **`PIVOTMIND_ALLOW_LEGACY_STATE`**（仿种子侧 `PIVOTMIND_ALLOW_LEGACY_SEED`；必须**严格等于 `"1"`**，只对 `fmt_ver<1` 生效；语义是「信任来源 + 显式记账」，不是校验）。
- **反证**：未改二进制读真 v9 → 1 节点 ACCEPTED、零记账；修复版 → `[ERROR]` + `rc=-1` + `EXIT=1`（**带开关也 `rc=-1`**）；非法首字段 `0/-1/9999/INT_MAX` 均拒绝；合法旧版本 v2/v5/v8 **不误伤**（各读 3 节点 `rc=3`）。单测 `test_memory_unit` 6/6、`test_topology_unit` 3/3、`test_diffusion_unit` 3/3 全 `EXIT=0`；修复树/未改树同口径 **0 warning 0 error**。

**② 特征维度 512→256 收进 main（13 文件）**
- 真值源 `include/constants.h:37`：`PM_NODE_FEATURE_DIM 512` → **`256`**；顺带**收敛 4 处分散定义**（`constants.h` 真值源 / `common.h` 唯一别名 / `feature_learn.c` 删本地 `#ifndef` 兜底改 `#include "common.h"` / `test_semantic_growth.c:41` 字面量改宏）与 **2 个 Python 工具**（`convert_state.py` / `clean_unicode_escapes.py` 各新增 `NODE_FEATURE_DIM = 256`）。
- `include/visual_cortex.h` 的 `feature_dim` 默认值 `512` → `PM_NODE_FEATURE_DIM`；其余为注释/文档同步。
- **穷举确认**：源码**无残留「裸 512 维度字面量」参与特征向量读写**；其余 512 均为缓冲区/容量/批大小等无关常量（未动）。

**③ 每 11 分钟必死的自死锁（`src/brainstem.c`）**
- `brainstem_tick_synapse_scale()` 持 master **读锁**期间调用会取**同一把锁写锁**的 `master_reevaluate_cross_links()`（`src/multi_topology.c:576`）→ glibc 同线程「**读→写**」升级 = **永久自死锁**；该函数只在 `tick%600==0` 进门 ⇒ **启动约 11 分钟必死**（现场 58/58 次运行全部停在 tick=600、1007 次运行最大 tick=600）。
- **引入点 commit `3d2f7cba`（v0.5.24，2026-09-06）**：属**修复引入回归**，是同类机制**第 2 次复发**（首次 `src/self_learner.c:430-433`，v0.5.14 已修）。审计：全仓 31 个 rdlock 获取点**恰好 1 处不配对 = `brainstem.c:396`**，修复后重扫 **0 命中**。
- 修法：**把调用移出读锁区间**（`unlock` 提前到 `master_reevaluate_cross_links()` 之前）+ 在读锁处补**纪律注释**（引用 `src/funcword.c:479` 原文「⚠️ 持读锁期间内部不得调用抢 master 写锁的函数」）。**功能行一字未改**。

### Quality
- **闸门**：armbian harness 对照 + 反向验证共 **60 份 `.out` 全读**；核心反证、开关语义 B1–B10、D 组（v2/v5/v8 不误伤）逐条；**全仓 v1 误伤复扫**（本机 38 + armbian 30 候选 + 仓库全历史）确认**无真实 v1 状态文件会被误伤**。
- **降维**：armbian 五支测试（`test_tensor` 14/14、`test_topology_unit` 3/3、`test_memory_unit` 6/6、`test_diffusion_unit` 3/3、`test_web_fetch` 46/46）**退出码全 0**；**维度自检 + 反证**：`pm-dim256` = 256 / 特征块 1024 B vs `pivotmind-baseline` = 512 / 2048 B。
- **死锁**：A/B 二进制 md5 不同（`53f4a0de…` vs `ca46d176…`，唯一变量 = `src/brainstem.c`），两组全量重编译 **0 warning / 0 error**。

### Known Issues
- **`TAIL-LOCK-1`**：`src/hippocampus.c:63/:94` **反向不配对（多解锁）**——`vocab==NULL` 早退时会 `unlock` 一把**从未加过**的读锁，可能污染 `__readers` 计数。方向与本轮自死锁相反，**非停摆根因**，属真缺陷，待单独立项。
- **`TAIL-ACCT-1`**：`src/feature_io.c:95` 是特征加载路径上**唯一的静默丢弃点**（无 `LOG_ERROR`/记账）→ 违反铁律「丢弃必须记账」。
- **`TAIL-TOOL-1`**：`tools/merge_states.py:25` 的 `NODE_FEATURE_DIM = 24`（**既非 512 也非 256**，注释还谎称一致），且 `:88` **忽略**从文件读到的 `feat_dim` → **拿真状态文件跑会静默错读**（`:88/:91` 读、`:205/:206/:209` 写全按 24）。与本次降维**正交**（远端起即存在）。
- **`emergent_pos.bin` 静默错读风险（本轮新发现）**：文件头只校验 magic+version、**无维度字段**，降维不改 version → 旧（512）文件会被新（256）二进制**静默错读**（与 `pivotmind_state.dat` 的显式拒绝形成不一致防御）。静态分析结论，未端到端复现。
- **长跑 A/B 对照结论已核实**（`fix-plans/deadlock-branchstem-fix.md` 已补完 · **§5.3/§5.4/§6**）——对照组 `final_tick=600`（自 20:09:20 起**冻结约 11 分钟**、4 线程成排 `futex_wait_queue`）；修复组 `final_tick=1110`（**越过 600 后又前进 510 tick**、`wchan` 无死锁形态）；**唯一变量 = `src/brainstem.c`**、同机同时段、同一份起始数据（`c7a21e48…`）。⚠ **边界（报告 §6.3）**：修复组**未做小时级长稳观察**，**不能声称「永不崩溃」**——结论严格限于「越过 600 这个已知必死点、无死锁形态」。
- **验证覆盖面**：闸门线**未跑 `make test` 全量**、**x86_64 + ASan/UBSan/TSan 那条腿完全没跑**；降维线仅 5 支、**无运行期语义质量证据**。两条线**均未取得跨架构/跨 libc 交付依据**。
- **未部署、未提交**；`include/pivotmind_version.h` 仍为 **`"0.5.27"`（版本号未 bump）**。
- **→ 更正（2026-09-12）**：上面这条**已过期**（历史原文保留、不涂改）。`include/pivotmind_version.h` 已在提交 **`f280cfa`（2026-09-12）** 中 bump 至 **`"0.5.28"`（`PIVOTMIND_MAJOR/MINOR/PATCH` = `0/5/28`）**，上述三条改动亦已提交（`f280cfa` = 状态版本闸门 + 降维 512→256 + 死锁修复；`25b2bdc` = 长跑监护 + 锁纪律门禁那一轮）。⚠ **部署前置仍未解除**：真实 `fmt_ver=9` 载荷会被新闸门有意拒绝，须先同步写入端到 v9（或等批 1 的 v10 读端），不可用开关绕过。**版本号自此收敛为单一真值源**：活文档（`README.md` / `README.zh-CN.md` / `ARCHITECTURE.md`）不再手写版本串，改由 `tools/sync_version_docs.py` 从真值源幂等生成、由 `tools/check_version_consistency.py` 秒级门禁把守（已接进 `make test`，用法见 `tests/README.md`；施工与逐条证据见 `fix-plans/version-ssot.md`）。
- **部署前置**：真实 `fmt_ver=9` 载荷共 4 副本（含 armbian `~/pivotmind/pivotmind_state.dat`），新闸门会**有意拒绝** → 直接部署 = 「启动即拒绝、空壳运行」；须先同步写入端到 v9 或等批 1 的 v10 读端，**不可用开关绕过**。

### Notes
- **主线：把「静默」变成「会喊」。** 三种静默各对应一条修法：静默丢数据（旧解析）→ 显式拒绝 + 记账；静默停摆（自死锁）→ 根因 + 长跑对照；静默错读（维度/工具）→ 单一真值源 + 如实登记。
- **教训：「秒级的测试抓不住分钟级的死」。** 该自死锁引入于 **v0.5.24**，**v0.5.26 / v0.5.27 两轮**（含 x86_64/TSan 复验、armbian 全量）都没抓住——因为它要**跑满约 11 分钟（tick=600）才第一次执行到**，秒级单测/TSan/冒烟套件都在此之前结束；且它是**纯自死锁**，TSan/Helgrind **不报 report、只跟着挂住**。建议后续加一条**长跑监护用例**（建议 ≥900 tick，断言 tick 越过 600 且持续增长）＋ 锁纪律探针，把这类低频分支上的死锁压到可回归。
- **红线声明**：所有**限边 / 截断 / 周期性稀疏化 / 跨拓扑上限 / 队列满丢任务**逻辑，按作者架构红线本版**一律未触碰**；版本闸门只改「拒绝/记账」不改合法文件解析结果，降维只收敛真值源不改算法，死锁修复功能行一字未改。

---

## v0.5.27 — 2026-09-11

> 来源：**round 3 并发修复**（由 x86_64/TSan 复验驱动，承接 v0.5.26 登记的既有并发债：已知未修问题 A 的批次契约、B 的批内竞争）。commit `1e84755`，相对 v0.5.26 发布点 `49d8a69`，**20 个文件 `+944 / −126`**（分支另含一处 v0.5.26 文档补记提交 `d75eec1`，不计入本数）。三条主线：① R1 批次完成语义（`tasks_left` 任务账 + 每批 `batch_epoch` 代次握手）② 对话路径 worker 生命周期 + R2a/R2b 激活账本锁域 ③ TSan 复验后的三处收口。完整发布说明（含双平台验证数字、跨树对照与诚实边界、红线声明）见 [changelogs/071-round3-concurrency-activation-locks.md](changelogs/071-round3-concurrency-activation-locks.md)。

### Fixed

**① R1 批次完成语义（核心，`src/thread_pool.c`、`include/thread_pool.h`、`src/dialog_system.c`）**
- **`thread_pool_batch` 完成屏障不成立**：旧实现用 `workers_done` 记“worker 跑完的趟数”却当作“人头数”用作完成判据——计数在上一批残留、或某 worker 多跑一趟即可提前凑满，使 `batch()` **在任务未全部执行完时就返回**；调用方（`dialog_reasoning_create`）据此 `free` 任务数组，而 worker 仍在解引用 → v0.5.26 记录的 `dialog_topo_worker` heap-use-after-free（老债）。
- 现改为**任务账 `tasks_left`**（每完成一件原子减一，归零才是本批结束）+ **每批 `batch_epoch` 代次握手**（每个 worker 每批只“进入”一次，进入时校验代次），并按代次收尾完成广播；`workers_done` 的“趟数当人头”错误不再存在。
- 索引分配（`next_index` 窃取）、任务数组快照、代次校验**全部收进同一把 `pool->mutex`**：worker 只在锁内认领“本批 + 本代”的任务，杜绝跨代串批。
- 契约写入头文件（`include/thread_pool.h`）：`thread_pool_batch` 返回 ⟺ 本批 count 个任务全部执行完 **且** 无 worker 仍站在本批之前的任务数组上——调用方返回后释放 `tasks`/`th_tasks` 安全；反之返回前不得释放。
- `dialog_system.c`：`hop_propagated` 由**非原子共享自增**（v0.5.26 已知问题 B 登记）改为 `__sync_fetch_and_add` 原子累加——它参与“本跳零传播即 break”的判据，非原子自增会让判据出错。

**② 对话路径 worker 生命周期（`src/dialog_system.c`，共 6 处，含 `:692`）**
- 对 `node->activation` 的**裸写**改为持 `node_locks[node_id & 255]`（`PM_NODE_LOCK_COUNT=256` 分片）的单锁临界区；与既有节点锁同域，不参与锁序、不可能 ABBA。

**③ R2a 新锁域：master 激活账本分片锁（`include/multi_topology.h`、`src/multi_topology.c`、`src/cognitive_controller.c`）**
- 新增 **`activation_locks[16]`**（`PM_TOPO_LOCK_COUNT=16`，`PM_TOPO_LOCK_IDX(topo_id)` 分片键），保护 master 的 `active_topo_id` / `active_node_ids[]` / `activation_levels[]` 及 `SubTopology` 的 `total_activations` / `recent_activation` / `avg_activation_value` / `last_used`。
- 锁数组**追加在结构体末尾**，`MasterTopology` 已有字段偏移不变（二进制兼容）。
- `master_activate_node` 拆成**两个不嵌套**的临界区（取锁—改账—放锁，再取下一把），满足“只允许单锁临界区”纪律：与 `node_locks`、与另一分片**永不同时持有** → 不参与锁序、不可能 ABBA。
- `master_propagate_activation` **先释放源分片锁再调用** `activate_node`，避免“持源分片锁去取目标分片锁”的 ABBA 死锁。

**④ R2b 读侧补锁（`src/nn/feature_learn.c`）**
- `feature_learn_graph_smooth` 的**两条读路径**（单线程快路径 + OpenMP 并行路径）对 `node->edges[].weight/.confidence` 的读**按 `node_locks` 加锁**；其写侧（`boost_connection_weighted`）此前**已持** `net->node_locks[]`（`src/autonomic_learner.c` 仅补注释固化“写侧合规、缺的是读侧”），补齐后该站点两侧同步。

**⑤ TSan 复验后的三处收口（`src/thread_pool.c`、`src/multi_topology.c`、`src/dialog_system.c`）**
- `thread_pool.c`：屏障读改为 `__atomic_load_n(..., __ATOMIC_ACQUIRE)`。
- `multi_topology.c`：`master->active_topo_id` 为全局单字段、被分片锁保护属**设计踩空**（单字段无法分片），改为原子访问。
- `dialog_system.c:177`：补节点锁。

### Changed
- **新增门禁工具与探针（`tools/probe_batch_contract.c`、`tests/round3/`、`Makefile`）**：批次契约探针 `tools/probe_batch_contract.c` + `make probe-batch-contract`（**只编 `src/thread_pool.c`，不进 `libpivotmind.a`，不参与 `all`/`test`/`asan-test`**）；`tests/round3/` 存放 G-T1..G-T5 复验脚本（批次契约 / TSan / ASan / armbian 全量 / N17 账行不变量）与产物目录。探针的价值在于**独立于业务代码**地实测 `batch()` 完成契约：修复前在旧树上给出违约，修复后给出 HOLDS。

### Quality
- **批次契约探针（正控）**：修复后 **58/58 迭代全 HOLDS、0 违约**；**同一探针在旧树上 40 违约**（v0.5.26）／**38 违约**（基线）。
- **N17 行为不变**：修复树编译的二进制与 **round 3 之前**编译的旧二进制，扩散账行输出**逐字节相同**（sha256 一致）；不变量脚本 `tests/round3/n17_ledger_invariants.py` 结果 `bad=0`。
- **x86_64 / G15-WSL**（gcc 15.2、glibc 2.43）TSan **0 条报告**（两份日志均 0，去重后剩余清单为空）；**对照**：未修复树 **12 条**（OMP=20）/ **11 条**（OMP=1）、v0.5.26 树 **117 条**、基线 **41 条**。**覆盖度正控**（gcov 逐行）证明单线程与 OpenMP 两条路径**分别**被跑到；ASan 与 v0.5.26 **逐项一致、无新增帧**；旧的 `dialog_topo_worker` heap-use-after-free **消失**。
- **aarch64 / armbian**：终态干净全量构建 `all` **0 warning / 0 error**；测试构建 **14 warning**（**全在 `tests/integration/test_integration.c`**，无新增）**0 error**；全套 **PASS=23 FAIL=0 TOTAL=23**。

### Known Issues
- **锁开销未实测**：读侧每节点多一次加/解锁，量级 `O(node_count)×3`，本次**未拿到耗时数据**（不宣称“无性能影响”）。
- **TSan 退出期伪影**：日志末尾的 `nested bug in the same thread, aborting.` 是**退出期伪影**，新旧日志都有、rc 恒 66，**不是本版消除的东西**。
- **`test_cognitive_controller` 几乎没有断言**（全文件 `assert` 数为 0，“通过”的说服力弱）——aarch64 的 23/23 不能作为该路径“没问题”的证据。
- **TSan 采信边界**：运行中出现过 `WARNING: ThreadSanitizer: memory layout is incompatible, possibly due to high-entropy ASLR`，故“0”以 **ASan + 跨树对照 + 覆盖度正控** 三方交叉为准。

### Notes
- **红线声明（本版一律未动）**：所有限边 / 截断 / 周期性稀疏化 / 跨拓扑上限 / 队列满丢任务逻辑，按作者架构红线本版均未触碰。
- 本批只做**同步与生命周期**修复，**未使用**降低并发度 / 缩小批量 / 休眠退避等掩盖竞争的手段。

---

## v0.5.26 — 2026-09-10

> 来源：**全量基线审查（19 P0 / 26 P1）+ 修复轮 2**。commit `5d6c9b1`，相对基线 `3eb2e6e`，35 个文件 `+1442 / −437`，按五批落地：① 数据保命 ② 并发与生命周期 ③ 门禁与诚实 ④ N17 扩散上限 ⑤ 验证期发现的既有缺陷。完整发布说明（含验证证据、红线声明、已知未修问题）见 [changelogs/070-review-round2-persistence-concurrency-gates.md](changelogs/070-review-round2-persistence-concurrency-gates.md)。

### Fixed

**数值与解析**
- **小矩阵乘法读未初始化内存（E-P0-1，数值正确性）**：`matrix_multiply_naive` 对 `tensor_create` 分配的未置零缓冲做 `+=` 累加；现于入口 `memset`，并补逐元素数值断言（`tests/unit/test_tensor.c`）。此前 `test_tensor` 只断言 shape/size，且历史上用"调小期望值"的方式让红灯变绿（见 v0.5.22 更正）。
- **`_sample_negative` 均匀采样分支无循环上限（P2-4 复核）**：`vocab->size <= 5` 时 `sampled < 5` 恒真 → 死循环；另补 `vocab` 空指针守卫。报告中"`pretrain.c:240` 除零"经核对**不成立**（该函数入口已有 `vocab->size <= 5` 守卫，该行不可达）。
- **`model_io.c` 加载畸形文件可致堆溢出（P2-1，32 位目标）**：`ndim` 补上界、`input*output*sizeof(float)` 补回绕守卫（权重与偏置两路）。
- **`lr_reduce_on_plateau` 未过滤 NaN（P2-3）**：非有限 `val_loss` 现直接跳过平台期更新。

**① 数据保命（第一批）**
- **种子加载失败不再被空状态覆盖（G2）**：加载失败原会继续走空状态初始化并回写覆盖原文件，等于"读不出就抹掉"；现加载失败即拒绝回写，保留原种子文件。
- **种子 footer 完整性校验——缺 footer 默认拒绝（D2）**：0 字节文件、尾部 1~15 残字节、16 字节非 `PMSEED2` footer 一律拒绝加载；无 footer 的旧格式**默认拒绝**，须显式 `PIVOTMIND_ALLOW_LEGACY_SEED=1` 才按旧格式一次性迁移接受。
- **死索引 `index_map` 停用（D3）**：全仓"只写不读"（create/store/destroy 之外零引用），且驱逐路径存在 `index_map[index_size]` 堆越界写；现停止维护该冗余索引（恒空、`index_size` 恒 0，与实际占用一致）。
- **`fsync(file) + fsync(dir)` + 唯一 tmp 名（A-P1-2）**：临时名加 pid + 单调序号防并发/重入互踩；先 `fflush + fsync(file)` 再 `fclose` 再 `rename`，并对目录 fd 补 `fsync`，确保目录项本身落盘（`memory_system.c`）。
- **`emergent_pos` 原子写（A-P1-5）**：同样 tmp+rename + `fsync(file)/fsync(dir)`；逐写检查返回值，磁盘满/写失败不再谎报"持久化完成"（`src/emergent_pos.c`）。
- **四处"0 节点存盘"门卫（D1）**：与既有"拓扑 total>=20"门卫对等，种子内容按 LTM 条目数判定（下限 `MEMORY_SEED_MIN_ENTRIES=1`），空状态不回写覆盖。

**② 并发与生命周期（第二批）**
- **关闭期 UAF：网关连接线程槽表 + join（C1）**：旧版无法 join 连接线程，只能"轮询计数 15s 后强拆 main 栈上的 `gw`"，慢连接线程随后访问已释放内存；新增连接线程登记槽表（`g_conn_mutex` 保护，`used`/`ever` 双标记）+ 主循环机会式回收，关闭时对 `ever==1` 的槽逐个 join。
- **init 线程 join（H2）**：初始化线程此前无人 join，关闭时可能仍在加载并重建 worker；现关闭路径显式 join init 线程，join 返回即代表加载流程已停。
- **learn 队列生命周期守卫（C3）**：关闭已开始（`stop` 置位）后绝不再入队；`learn_queue_shutdown` 只 join "创建成功"的 worker 槽（原实现无条件 join 全部槽，`pthread_create` 失败时 join 的是零值 `pthread_t`，属未定义行为）。
- **共享线程池批次闸门（C4）**：单例池同一时刻只允许一个批次；池忙时 `batch()` 立即返回 `THREAD_POOL_BUSY(-2)` 且不执行任何任务，调用方（`dialog_system.c` / `multi_topology.c`）据此串行降级，消除两处并发提交互串批次。
- **线程池 shutdown 守卫 + 共享状态加锁**：批次进行中不响应 `shutdown`，worker 完成本批后由 `batch()` 正常收尾；批次状态重置移入锁内。
- **`learning_scheduler` 引入 `thread_started` 旗标（B-P1-2）**：以旗标而非对象地址判定句柄是否有效，决定 `stop` 能否 join；`stop` 幂等（含 destroy 内部那次重复 stop 直接返回），避免 `join(0)`。

**⑤ 验证期发现的既有缺陷（第五批，多为基线自带）**
- **`autonomic_stop_async_flush` 用 `state->initialized` 当存活判据 → `pthread_join(0)`**：`initialized` 并不代表 flush 线程句柄有效，导致 `join(0)` ——glibc 2.43 下 SIGSEGV、glibc 2.39 下静默返回 `ESRCH`。该缺陷**基线自带**（`ab1f79e`，2026-06-04 引入）。改用 create 成功之后才置位的 `flush_started` 旗标判定。
- **`master_topology_create` 从不初始化 `master->node_cache`**：`malloc` 不置零，字段是脏指针而全仓使用点都写成 `if (master->node_cache)` 守卫形式 → 非 NULL 垃圾值绕过守卫解引用野指针（ASan 下 `diffusion.c` 解引用 `node_cache->auto_thaw_ok` 已实测 SEGV）。现显式初始化 `node_cache`/`ext_dict`/`cognitive_state_ptr`/`_pad_parallel_mode`。
- **`multi_topology.c` 每步 `calloc` 被剪枝 `break` 跳过 `free`**：`path_target_weights` 在循环体内每步 `calloc`、循环体末尾 `free`，但循环内多个 `break` 退出点（剪枝 / 语义场休止 / 候选耗尽）会跳过释放 → 库侧泄漏。现上提到循环外单次分配，每步 `memset` 复位（语义等价），在函数唯一返回路径释放。
- **`tests/unit/test_memory.c` 自身 `strdup` 未释放**：测试内的泄漏，已补 `free`。

### Changed
- **N17 扩散上限（第四批）**：删除 `src/diffusion.c` 的 `SPREAD_MAX_EXTRA=256` 与栈数组上限；改为**每跳累加全部入边贡献后按强度闸门裁决**（θ = max(0.001, 0.15×本跳峰值)），消除"首边独占、顺序即命运"。实测：候选 2000 = 点亮 1000 + 落选 1000，点亮数不再被 256 卡住，且把边顺序整体对调后账行**逐字节相同**。

### Quality
- **CI 真的跑测试了（E-P1-A）**：原 "Run tests" step 调的 `make test-*`（Makefile:248-271）是**纯构建别名、从不执行**，15 个测试必挂也让 CI 变绿；现改为 `make test`（构建 + 执行 + 汇总 + 退出码门禁）。
- **桩测试清出**：删除 8 行 Hello World `test_io.c`；`test_chinese.c` 改写为 UTF-8/CJK 真断言（不再需要 windows.h，CI 不再跳过）；`tests/scratch/test_tensor_broadcast.c` 移入 `tests/unit/` 并接线到 `make test`（即此前闲置的广播测试接进 CI）。
- **测试目标集统一**：`test:` 前置与 `TEST_BINS` 逐项一致；`test-integration`/`test-semantic-growth`/`test-tensor-broadcast` 全部纳入，消除"定义了却没人跑"。
- **ASan 门禁收敛（E-P1-12）**：旗标单一来源（Makefile），本地 `make asan-test` 与 CI 一致且覆盖 `-DHAS_OPENSSL` 出货代码路径；`detect_leaks=1`（此前为 0），与 changelogs/069 宣称一致。
- **随机种子可复现（P2-2）**：新增 `init_random_seed()` / `init_random_from_env()`（`PIVOTMIND_SEED` 环境变量），既有调用点零改动。
- ARM 交叉构建删除纯 C 项目无意义的 `-static-libstdc++`。
- **文档措辞如实化（第三批）**：把"验证过"的表述标注为"**仓外临时程序、不可复现、不受 CI 保护**"（同步 changelogs/069 的验证章节）。
- **验证状态（如实）**：**aarch64 / armbian**（glibc 2.39）`make -j2 all` exit 0、全套 **23/23 通过**、ASan（`detect_leaks=1`）**5/5 干净、零泄漏**；**x86_64 / G15-WSL**（gcc 15.2、glibc 2.43）ASan **5/5 干净**、全量 **22/23**——`test_cognitive_controller` 崩溃，已定位为上述 `autonomic_stop_async_flush` 的 `pthread_join(0)`，**本版已修**。
- **x86_64/TSan（首次在 x86_64 上跑并发检测）**：抓到 `dialog_topo_worker` 的 heap-use-after-free（与基线 `3eb2e6e` 同型，见 Known Issues）及若干独立并发站点；**两条须连同结论一并采信的边界**：(1) TSan 运行中出现过 `WARNING: ThreadSanitizer: memory layout is incompatible, possibly due to high-entropy ASLR`，此类警告下报告需谨慎采信；(2) `test_cognitive_controller` **几乎没有任何断言**，其在 aarch64 上的"通过"说服力很弱（它恰是 x86_64 下崩的那一支）。另：修复树 TSan 因 OpenMP 报告洪水未跑完（工具限制），UAF 报告在首分钟内取得。

### Known Issues
- **未修（基线自带 `heap-use-after-free`）**：`src/dialog_system.c:149` `dialog_topo_worker`——主线程 `dialog_reasoning_create` 用完批次任务即 `free`（本版 `:814`／基线 `:810`）而 worker 仍在读，基线 `3eb2e6e` 报出**同型** UAF（`:149`/`:162`）故属**既有债**；本版新增的 `in_batch` 批量闸门（C4）只堵"并发第二批次"、**不覆盖该路径**（根因是 `thread_pool_batch` 的完成屏障本身不成立——`workers_done` 记的是"趟数"却被当"人头数"用），**本版未修**，round 3 方案在修（分支 `fix/round3-concurrency`）。此处如实记录，不作"已修"或含糊表述。

### Notes
- **红线声明（本版一律未动）**：所有限边 / 截断 / 周期性稀疏化 / 跨拓扑上限 / 队列满丢任务逻辑，按作者架构红线本版均未触碰。

---

## v0.5.25 — 2026-09-09

### Fixed
- **线程池 destroy/超时路径 UAF 与串批隐患（P1-1）**：批次状态重置移入锁内（旧版无锁重置 + volatile 属 C11 数据竞争 UB）；`shutdown` 仅在「非批次进行中」才响应，destroy 恰逢批次进行时 worker 完成本批并上报 done，batch() 正常收尾；worker 用锁内快照 `batch_count` 窃取任务，防本批超时强退后慢 worker 串批执行下一批任务；等待完成不再"10s 超时强制结束返回成功"（会 UAF 栈上 tasks），超时改持续告警等待；batch 收尾广播 `cv_done`，destroy 不再干等 5s 超时。
- **记忆种子截断即全损且无校验（P0-2）**：旧版 `fopen(path,"wb")` 直接覆盖，崩溃/断电写中途截断原文件无备份可回退；加载边读边 store，损坏文件被静默"部分加载"。改为 tmp+rename 原子写 + FNV-1a 64 哈希 footer（MAGIC `PMSEED2`），加载两遍解析先校验后提交，损坏拒绝加载，旧格式（无 footer）保持兼容。
- **记忆种子哈希校验两端不对称（P0-3）**：保存端把 footer 魔数 `PMSEED2` 也喂进哈希后才落盘 hash，加载端只对记录区算哈希 → 保存端 `FNV(记录区‖MAGIC)` vs 加载端 `FNV(记录区)`，所有新格式种子文件校验恒失败、保存后即不可读。修复：魔数改裸 `fwrite` 不参与哈希，与加载端第一遍解析范围严格对齐。
- **种子加载循环边界把 footer 当记录解析（P0-5）**：第一遍解析循环用 `while (pos + 16 <= sz)`，最后一条记录读完后剩余恰好 16 字节仍进循环，把 footer 魔数前 4 字节 `"PMSE"` 当 `key_len`（1163010384 > 4096）→ 恒判"记录截断"→ 新格式种子即使哈希正确也 100% 加载失败，空种子（16B 纯 footer）同样被拒。修复：条件改 `while (pos + 16 < sz)`。实测 3 条记录文件往返一致、空种子返回 0、篡改/截断文件仍被拒。
- **学习 worker 无退出机制（P0-4）**：`g_learn_q.stop` 无任何置位点、worker 从未 join，`gw_system_shutdown` 直接销毁 brainstem/topology/perception 等，而 worker 仍在消费任务访问 `gw->topology`/`gw->perception` → 关闭时 use-after-free。新增 `learn_queue_shutdown()`（stop + broadcast + join 全部 worker + 防御性排空残留），并在 `gw_system_shutdown` 最前调用，早于任何 destroy。
- **网关 token 比较时序侧信道（C1）**：strcmp 逐字节提前返回可被响应时间逐位爆破 → 全程遍历 + XOR 累积的常量时间比较（长度不等直接拒绝）。
- **Content-Length 解析歧义**：只扫描 header 区间 `[0, header_end)`，防 body 内同名文本被误当头部；`atoi` → `strtol` 严格校验非法数字。

### Changed
- **网关每连接独立线程（P0-1）**：旧版单线程串行 accept→处理，慢上传/同步推理阻塞后续所有连接、/health 被拖死；改独立线程 + `g_conn_count` 原子计数限并发（`GW_MAX_CONN=64`），超限直接 503；退出有界等待在途连接线程（15s），避免 detached 线程访问已释放的 gw。
- **token 打印脱敏**：启动日志不再整段明文打 token，仅显前 4 后 4，完整值存 GW_TOKEN_FILE（0600）。
- **端口占用探测去重（P2-2）**：删 3 处重复 connect 预探测与重复 `/tmp/pivotmind.port` 写入；预探测只连 127.0.0.1 与实际 bind 地址不一致且存在 TOCTOU，改直接 bind 按 errno==EADDRINUSE 判定。
- **日志线程安全 + 可落盘（P2-4）**：error.c 新增 `log_set_output(FILE*)`（NULL 恢复 stderr），日志整行加 pthread 互斥防并发写交错；`level >= LOG_WARNING` 即 fflush；gateway 支持 `PIVOTMIND_LOG_FILE=路径`，dup2 重定向 stdout/stderr（crash handler 直接 write(2) 同样落盘，fd 带 O_CLOEXEC）。
- **错误码扩展（P2-3）**：`ErrorCode` 末尾追加 ERR_IO_ERROR/ERR_TIMEOUT/ERR_BUSY/ERR_UNAUTHORIZED/ERR_BAD_REQUEST/ERR_PARSE_FAILED/ERR_INVALID_STATE/ERR_CHECKSUM_MISMATCH，头文件注明"新增必须追加末尾禁中间插入"（错误码按数值持久化/对外引用）；error_string() 补齐。

### Refactor
- **gateway 单文件按模块拆分（P2-6）**：`demos/pivotmind_gateway.c`（~2200 行）拆为 6 文件——`gateway_internal.h`（共享类型/宏/跨模块原型 185 行）、`gateway_http.c`（JSON/HTTP 工具 + parse_request）、`gateway_system.c`（初始化/保存/关闭，gw_system_init 保持 static）、`gateway_learn.c`（LearnTask 队列与 worker，队列状态模块内 static）、`gateway_handlers.c`（11 个 REST handler）、主文件瘦身 ~590 行（token 生命周期/路由/P0-1 连接线程/crash/main）。handle_connection 只做路由分发，handler 逻辑零改动搬迁；导出面收敛为 23 个跨模块符号，模块内部辅助函数保持 static。Makefile 链接规则纳入 GATEWAY_OBJ。

### Quality
- CI 新增 `asan-ubsan` job（P2-7）：ASan/UBSan 全量编译 + 7 个核心单测（model/memory/topology/dialog/learner/causal/forgetting）跑 sanitizer，`halt_on_error=1`。
- WSL gcc `-Wall -Wextra` **语法校验**零告警（gateway 6 文件 + memory_system/thread_pool 等改动文件）——注：语法校验 ≠ 构建验证 ≠ 运行验证。另有**仓内可复现**的构建验证：`make -j2 gateway` 在 aarch64 上 EXIT=0、0 warning。
- 记忆种子保存/加载验证 20 项断言全通过（往返一致 / 哈希对称 / 篡改与截断拒绝 / 空种子）——**验证方式：WSL gcc 15 上的仓外临时测试程序（不入库、不可复现、不受 CI 保护）**，详见 changelogs/069「验证」章节的可复现性声明。
- 符号完整性审计与 HEAD 38 个顶层函数逐一比对无缺失；YAML 合法；git diff 共 16 文件（含 4 新增拆分 .c + internal.h + Makefile）。

详见 [changelogs/069-code-review-optimization-round.md](changelogs/069-code-review-optimization-round.md)

---

## v0.5.24 — 2026-09-06

### Fixed
- **存盘空批误判 OOM（根治）**：候选A 流式批化存盘主循环用 `if (!batch)` 一刀切判 OOM，把「空拓扑/空批」（情绪拓扑惰性初始化 node_count==0）误判成 OOM → break 放弃整轮，主状态文件自 9-03 起长期无法更新。改为「先以 cnt==0 判空批推进，再以 batch==NULL 判真 OOM」。
- **锁内写路径饿死**：删除旧内存门卫（est+512MB 回退锁内写），主流程改流式批循环（每批 ≤100 节点短持锁深拷贝 + 锁外序列化追加写），磁盘 I/O 100% 在锁外。
- **崩溃 handler 纯 async-signal-safe**：彻底删 backtrace 系列（内部走 dlopen/dladdr 会再入动态连接器与 malloc，堆损坏时自锁死锁），仅 write(2) 输出信号行后 _exit。

### Changed
- **激活记录 pthread_key 化**：__thread 线程局部存储改堆分配 + pthread_key，支持可重入。
- **常量扩容**：PM_EDGE_TRACK 128→256、PM_ACTIVATED_PAIRS 4096→8192。
- **concept 解析直查**：快照序列化目标 concept 从 concept_by_id 查表改为 net->nodes[id]->concept 直接查。

### Quality
- 编译 `-Wall -Wextra` 零警告；单测 topology 3/3、memory 4/4。
- 实测：喂料 50 条节点 30167→30367、跨重启加载零丢失，OOM 报错归零。

详见 [changelogs/068-streaming-batch-save-oom-fix.md](changelogs/068-streaming-batch-save-oom-fix.md)

---

## v0.5.23 — 2026-08-19

### Added
- **top-K 缝合（绑定三件套①）**：`topology_walk_greedy_topk` 每步候选 top-3 + 综合打分（激活+跨拓扑预加热+模板），单链贪心升级；K=1 逐位等价，其余调用方零影响。
- **cross_hit 持久化（绑定三件套②）**：跨拓扑联合激活计数落盘（STATE_FORMAT_VERSION 7→8），跨重启不归零，≥5 次自动建边闭环可达成。
- **种子词表（绑定三件套③）**：16 个实体种子词预注册整词节点，强制 TOPO_VOCABULARY，带开关；实体词不再被拆成字符碎片。

### Fixed
- **R6 锁模型**：net->nodes 双路径 realloc 竞态（add_node net->mutex / auto_extend master->rwlock 互不排斥）→ net->mutex 唯一权威锁，写侧补 4 缺锁点 + 读侧补 3 缺锁点，锁序恒 master→net。修复凌晨全量重喂下 SIGABRT（堆损坏）+ SIGSEGV（auto_learn_concepts 读野指针）。
- **种子词落错拓扑**：预注册强制 TOPO_VOCABULARY（原随喂料领域拓扑）。
- **网关 token 持久化**：随机生成 + gw_token 文件（0600）记住有效 token，跨重启不变。
- **watchdog 双实例并发**：gateway_watchdog.sh 加 flock（08-18 21:00 状态缩水事故元凶）。

### Changed
- **树莓派黑匣子探测**：适配网关绑 127.0.0.1（SSH 板上本地 curl + 日志动态取 token）。

### Quality
- 单测全绿：topology 3/3、tensor 13/13、learner 3/3、dialog 4/4；全仓编译零警告。

详见 [changelogs/067-binding-trilogy-lock-model.md](changelogs/067-binding-trilogy-lock-model.md)

---

## v0.5.22 — 2026-08-18

### Security
- **网关全端点鉴权**：`X-Pivot-Token` 覆盖除 /health、/healthz 外所有端点（/chat /learn /feedback /qa /debug /force_templates /train/* 等）；默认绑定 127.0.0.1（`PIVOTMIND_BIND_ADDR=0.0.0.0` 可覆盖），局域网直接访问面关闭。
- **qa_crawler 注入面清除**：`system()` 全清零 → `fork+execvp`（argv 直传杜绝 shell 注入）+ 父进程超时 SIGKILL 保护；URL 白名单默认拒绝 + 协议/域名/路径三级校验（封堵子串伪造）。

### Fixed
- **SIGSEGV 真根因修复**：`object_pool_acquire` 扩容分支 `free_count` 恒 0 → `free_list[-1]` 越界读返回垃圾指针——infer 建图边数超过池容量时崩溃（"薛定谔的猫"类因果查询偶发 SIGSEGV；GPT 审查/pro 复审曾误判为缓存悬垂）。压测 6/6 因果查询零崩溃。
- **causal_reasoning 缓存悬垂**：`causal_associative_search` early-return 路径 destroy 共享缓存 `g_cg_cache` 后未置 NULL（且锁外 destroy）→ 改为不销毁，统一由指纹变化分支管理。
- **UTF-8 标点比较**：gateway CJK 多字节字符常量 vs 单字节 char 恒 false → strncmp UTF-8 序列比较，汉字标点真正计入统计。
- **corpus_train fread 缓冲未终止**：按实际读取数定 NUL 位置。

### Changed
- **Makefile 并行度**：删 `MAKEFLAGS += -j$(nproc)`（3.8GB 板全核编译 OOM）→ `JOBS ?= 2`，命令行 -j 优先。
- **_ar_find_pair 查找优化**：PairEntry 固化键哈希 + 每槽 2×strcmp → 1 次 int 短路 + rehash 免 snprintf 重哈希（哈希同值性 10 万对验证，语义零漂移）。澄清：查找本就是开放定址哈希，O(n²) 真根因（三字扩展双层迭代）已于 3878e84 修复。

### Quality
- **test_tensor 13/13**：3 处断言修复（reshape 3×5→{1,15}、matmul size 6→4、NULL 输入测试传参错误）。
  - **⚠️ 更正（第三批「门禁与诚实」）**：其中 "**matmul size 6→4**" 是**把期望值改小以让红灯变绿**——该用例（2×3 乘 3×2）的正确结果尺寸本就是 4，真正的缺陷是**测试从不校验元素数值**，而 `matrix_multiply_naive`（小矩阵路径）用 `+=` 累加到 `tensor_create` 的**未初始化缓冲**（对照 `matrix_multiply_blocked` 有 `memset`）→ 堆一复用就产生随机错误结果。本批已修正：naive 路径入口补 `memset`，并补逐元素数值断言（期望值 `{9,12,9,12}`，注释中给出心算过程）。
- **全仓编译警告清零**：20 条 -Wall -Wextra 全消（未用变量/未用参数/符号比较/多字节字符常量等）。

详见 [changelogs/066-security-hardening-crash-fix.md](changelogs/066-security-hardening-crash-fix.md)

---

## v0.5.21 — 2026-08-15

### Security
- **C1 远程 RCE 封堵**：`media_reader.c`/`visual_cortex.c` 7 个 `popen/system` 调用点全部 execvp 化（去 shell 解释层）；`/media/*` 加 `X-Pivot-Token` 鉴权（启动生成 64 位 hex token 打印日志，无 token 401）；realpath+S_ISREG+媒体目录白名单灭 SSRF；`"> NUL"` Windows-ism 修复。
- **B1 边界**：H1 状态加载 `from_node` 范围校验（防损坏文件 OOB/OOM）；M4 去重分支 `return -1` → `return result`。

### Fixed
- **B3 锁外快照写盘**（STUCK 根治）：存盘从"持 master 读锁写 330MB"改为"三锁同时拿全量深拷贝 + 锁外写盘"，字节级等价，`PIVOTMIND_SAVE=locked` 回退。
- **B4 learn 队列化**：`handle_chat` 同步学习入队（输入 flush / 回复 fire-and-forget），worker 2→1，`_learn_tokens` 零改动。
- **B2 broca 越界**：`pos_tags[64]` 栈越界读 → 动态分配；空格插入/NUL 无边界 → 扩容 helper。
- **C1 side_inhibit 字节 bug**：`strncmp(...,2)` 比 2 字节（CJK 误判"一/丁"）→ UTF-8 完整字符比较。
- **C2 词锚重叠去重**：三处词锚插入点加字符重叠检查，修"方鸿+鸿渐"裂词。

### Changed
- **C3 `MAX_REPLY_WORDS` 常量化**：主路径与降级路径共用，便于 A/B 实验。

### Experiment
- **4→6 词 A/B**：6 词只放大词层拼接垃圾、质量不升 → 回退 4。信息量瓶颈在选词/组句，不在长度限制；生成端微调收手，等 dist_sig 数据上阶段 2.5/3。

详见 [changelogs/065-security-concurrency-generation.md](changelogs/065-security-concurrency-generation.md)

---

## v0.5.9 — 2026-08-07

### Added
- **字符对/字符表记忆化**（用户拍板架构方向：一切统计=记忆，遗忘≠删除）：`PairEntry`/`CharEntry` 加巩固度/活跃度/短期长期层级；Hebbian 巩固（0.02×(1-c)），≥0.5 晋升长期永不删；表满清理只清「未巩固+长期不活跃」（重建式删除防探测链断裂）；冷启动零误杀。
- **/learn 队列化**：固定 2 worker + 256 队列（满丢最旧）——不再每请求 spawn 线程，线程 35→6（40 次采样全 6）。
- **POS 池 256→64**：聚类快 16 倍。
- **探索记录生命周期**：悬垂回收（节点冻结/删除的记录）+ 最久未探索覆盖。
- **路径频率表淘汰修正**：低频+长期不活跃才驱逐（旧 count 优先会误杀刚学路径）。

### Changed
- **字符对表哲学**：记账本 → 突触（形成→巩固→降权→不删除），与 Hebbian 边权/冻结/三重记忆统一。
- **/learn 并发模型**：每请求一线程 → 队列+固定 worker（与感知 worker 同构）。

详见 [changelogs/064-charpair-memory-learning-queue.md](changelogs/064-charpair-memory-learning-queue.md)

---

## v0.5.8 — 2026-08-07

### Fixed
- **主循环网络阻塞根治（gdb 三次抓栈实锤）**：感知搜索异步化（`perception_tick` 只入队，worker 线程串行搜索——主循环永不碰网络）；自学锁外搜索（`self_learner_cycle` 锁内只收集概念名）；主循环自学改 `perception_enqueue_search` 异步入队；`g_fetch_lock` 改 timedlock（8s 超时放弃）。
- **两处漏锁（各 4 处提前 return）**：`autonomic_compound_consolidate` 漏解锁 → master 写锁永久持有；`_article_flush_locked` 漏解锁 → ar->mutex 永久持有 → 274 线程堆积雪崩。均改 goto 统一出口。
- **POS 池喂养锁外化**：tag_soft 含 O(n²) 聚类（256² 矩阵 2-5 秒），锁内执行导致学习线程排队堆积——改锁内只收集概念名，`article_flush` 解锁后喂养。

### Added
- **POS 池管道**：喂料路径（article_reader）首次喂养语法拓扑——新词查询词性进池 → 池满聚类 → **额外词类 0→1 历史性破零**（08-04 基线 0：POS 池只吃生成/对话路径，玄枢不会说话 → 死循环打破）。
- **`perception_enqueue_search`**：异步入队式搜索，调用方零阻塞。
- **聚类后清池**：`try_emerge` 检查后清空未分类池，散词不再反复触发 O(n²) 聚类。

### Changed
- **版本策略**：0.5.7 → 0.5.8（小步更新）。
- **黑匣子哨兵**：no-agent 纯告警 → agent 模式（告警自动触发调查处理）。

详见 [changelogs/063-perception-async-pos-pipeline.md](changelogs/063-perception-async-pos-pipeline.md)

---

## v0.5.7 — 2026-08-01

### Fixed
- **rwlock 死锁根治**：glibc 写锁内嵌套读锁返回 EDEADLK，忽略返回值后错误 unlock → 锁状态损坏 → 永久卡死。`cross_link_exists` 拆 `nolock` 变体。
- **知识被清光多层根因**：load_protect 未初始化（保护期从未生效）+ 时间保护期 30 分钟 + 保底激活 0.3 + 边权保底 0.2 + 修剪跳过 is_cooled + RED 阈值放宽。
- **词巩固并发 SIGSEGV**：全程持 master 写锁（防对话线程 realloc 悬垂）。
- **PFE 四大 O(N²) 卡点**：因果图去重 → djb2 哈希桶；加边查重/统计/环检查 → no_check 批量版；边查询 → 邻接表（outgoing 存边索引）；A* 扩展 5 万 target → 上限 2000 + 探索 100。
- **因果图缓存悬垂**：causal_associative_search 不再 destroy 共享缓存图。
- **上下文注入串扰**：禁用上轮回复拼接（词锚定命中旧回复词）。
- **子目标主语提取**：extract_subject 重写（"历史为什么重要"→"历史"）。

### Added
- **词层架构**：词巩固（相对/绝对强度双通道涌现 + 虚字过滤 + 高频方向定词序）→ 概念拓扑晋升 + cross-link 回字；词锚定优先输出。
- **话题性三层**：relevance 评分（候选与锚定集边权，<0.3 过滤）+ 有界联想回退（0.3-0.5 次相关）+ 话题序组装（≤4 实词短句 + 中文单字过滤）。
- **PFE 推理管线跑通**：推理词保底触发 + 子目标分解 → 因果搜索/diffusion 求解 → 推理链合成（6 子目标 + 置信度）；28s 卡死 → 2s。
- **词级语义场**：feed_cli 2 字窗口配对词候选 + 词-词 Hebbian 共现边 + diffusion 词邻居扩散。
- **词级语义拓扑**：词聚类语义生长（激活排序采样 + 特征 0 填充）+ 语义场查询（词→概念→成员词）。

### Changed
- **词级虚字表**：缩小到"绝对虚字"——"过去/将来/上面"等含字级虚字但整体实义的词保留（用户纠正）。
- **功能词兜底**：纯"很+X"功能词回复 → "好的。"
- **版本策略**：只能小步更新（0.5.7 这种），大版本（0.6）必须用户拍板。

详见 [changelogs/062-word-semantic-field-reasoning-pipeline.md](changelogs/062-word-semantic-field-reasoning-pipeline.md)

---

## v0.5.6 — 2026-07-27

### Fixed
- **node_hash 整数溢出崩溃**：`node_hash_reserve` 参数 `int` → `size_t`，节点数达 40009 后 `calloc(负数)` → crash。结构体 `bucket_count`/`node_count` 同步改为 `size_t`，去掉人工上限，靠 OOM 兜底自然增长。
- **编译警告清零**：`node_hash.c` 修复 5 处 `size_t` 格式串与符号比较警告。

### Changed
- **内感受驱动冻结**：`brainstem_tick_freeze` 接入 `health_monitor` 健康等级，不再固定 10/轮：
  - GREEN → 600 tick, 10/轮
  - YELLOW → 200 tick, 50/轮
  - RED → 1 tick, 不限量 + 删孤立冻节点
- **RED 级节点回收**：冻结后调用 `prune_isolated_nodes` 真删 0 边冻节点 + `master_prune_dead_nodes` 清理死节点，不再只砍边不删节点。
- **RED 时暂停语义生长**：`semantic_grow_from_vocab` 在 RED 时跳过，先腾内存再说话。

### Added
- **按需解冻机制**：对话管道 `dialog_process` 在语义理解后，只对当前输入的实体词和分词调用 `master_find_or_thaw` → `node_hash_find_or_thaw`，精准解冻当前话题涉及的冻节点，不盲扫无关领域（如科技对话不解冻医疗节点）。
- **内存安全阀**：`NodeCache.auto_thaw_ok` 由 `health_monitor` 每 120tick 同步更新，YELLOW/RED 时禁止全部解冻（含按需解冻），防止越救越糟。
- **`NodeHashTable` 注入管线**：新增 `cache` + `net` 引用，gateway 初始化时注入，`node_hash_find_or_thaw` 直接可用。
- **`master_find_or_thaw`** — 跨拓扑查找概念并自动解冻的便捷 API。

---

## v0.5.5 — 2026-07-18

### Added
- **网关启动保护**：启动前检测端口占用，若已被占用则拒绝启动，防止重复实例冲突
- **动态端口文件**：启动时将端口号写入 `/tmp/pivotmind.port`，脚本无需硬编码端口

### Changed
- QA 对统一走 `/learn` 通道，与语料一致走 PMI 共现拓扑，不做独立检索匹配
- 撤回 `/qa` REST 端点（含 `qa_memory_add`），简化架构

### Fixed
- 删除重名 NetworkManager 连接，BSSID 锁死 `7C:FD:FD:CA:B2:B0`，解决双频 WiFi 漫游断连问题

---

## v0.5.5 — 2026-07-13

### Added
- 语义约束管线：组合节点 `P(B|A) ≥ 0.5, N ≥ 10` + 自举分词
- 两跳激活扩散 `λ=1.0/0.4` + Jaccard → `node_act` 权重 0.35
- 语义休止 `node_act < 0.05`
- 语言感知扩散（同语言 `×1.3` / 跨语言 `×0.4`）
- 英文词间空格自动插入
- 搜索 → learn 管线打通 (`perception.c`)
- `semantic_growth.c` hook

### Changed
- 存盘周期改为 60 tick（~17 分钟）
- 跨语言边禁建，按 token 首字节分流（ASCII → 英文 PMI，CJK → 中文 PMI）
