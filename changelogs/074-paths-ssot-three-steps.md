# v0.5.30 — 路径可移植化三段式（SSOT 真值源 / 调用点替换 / 默认自建）+ `digital_life` 复活 + 七笔尾巴归账

> **日期**: 2026-09-13 | **类型**: 重构 / 工程化 / 修复
> **状态**: **已提交（14 笔）** —— 权威工作区 `/home/cx/pm-fix`（astar728-1，Raspberry Pi 3B Rev 1.2），分支 **`feat/paths-callsite-migration`**，HEAD **`2e0bb19`**；**`main` 未动**（仍为 `1a2e873`）。本文件 + 根 `CHANGELOG.md` 顶部条目 + `include/pivotmind_version.h` 的 bump 是同一轮**文档收尾**。
> **来源**: ① `include/pivotmind_paths.h` 头文件自述契约（三步规划的**原文出处**）② `include/brainstem.h` / `git show caa6e36`（`digital_life` 修复依据）③ 工作日志 `D:\111\.workbuddy\memory\2026-09-12.md` / `2026-09-13.md` ④ 三机实测输出：WSL `~/pm-s3`、armbian-1 `~/pm-step3`、Pi `~/pm-fix`。
> **说明**: 本文只记**已落地**的改动，每条给提交号与验证口径；未做的单列「已知未做」。**数字一律从实测输出抄，不新造**；凡属推断而非实测的，文中显式标注。

## 概述

「路径可移植化」是 v0.5.29 之后最重的一条线。它的靶心不是某个 bug，而是一类**结构性隐患**：数据文件落在哪儿，此前由**几十处散落的字符串字面量**（`"pivotmind_state.dat"`）和**各自 `getenv`**（`PIVOTMIND_LOG_FILE`）决定 —— 既无法一眼看出全仓的落点集合，也没法保证「目录不存在时自动建好」。

`include/pivotmind_paths.h` 在**第 1 步就写死了这条线的分段**（原文）：

> 这是「路径可移植化」的**第一步**：只立真值源，**不动任何现有调用点**（替换是第 2 步）。

> `/* ---- 数据文件登记表查询（第 2 步：调用点替换）---- */`

本版把三段全部落地：

| # | 步 | 一句话 | 提交 | 规模 |
|---|----|--------|------|------|
| ① | **立真值源** | 新建 `pivotmind_paths` 模块，只立契约、不动调用点 | `faf7655` + `1a2e873` | 4 文件 / **+975 −5** |
| ② | **调用点替换** | `src/` + `demos/` + `tools/` + `tests/` 全量改走 `pm_file()` / `pm_log_path()` | `0b6a3fd` + `9fd28f7` + `f500ef2` | **32 文件 / +285 −80** |
| ③ | **默认自建** | `tools/` 层 7 个「会写 SSOT 路径」的入口补 `pm_ensure_dirs(PM_DIR_ALL)` | `2e0bb19` | 7 文件 / **+70** |

另有 `ab1853e`（`digital_life` 复活）与 **7 笔 v0.5.29 之后的尾巴**。

## 一、第 1 步：立真值源（`faf7655` + `1a2e873`）

### 契约（头文件原文，标注「已逐条定稿，勿改」）

三条，一条不能少：

1. **纯查询**：`pm_home()` / `pm_dir()` **不碰文件系统、永不失败、总返回非空字符串**；`pm_dir(非法 which)` ⇒ 返回 `NULL`（调用方必须判），**绝不「悄悄返回 home」**。
2. **无状态 ensure**：`pm_ensure_dirs()` 是**唯一写文件系统**的入口；**不设缓存、不加锁、可重复调用**；共享可变状态 = 0（并发安全，`EEXIST` 幂等）。
3. **零分配**：`pm_path()` 的缓冲区由调用方提供，本模块**不 `malloc`**。

### 解析顺序与「不许静默」

`pm_home()` 命中即止，内部 `pthread_once` 解析一次并缓存：

1. `$PIVOTMIND_HOME` —— 必须**非空、非全空白、绝对路径、不含 `~`、不是 `/`**
2. `$HOME/pivotmind` —— 运行期展开
3. `PM_HOME_DEFAULT` —— 编译期固定绝对路径

任一非法值 ⇒ **视为未设置 + 一条 WARN（说明原因，不许静默）**。`$PIVOTMIND_HOME` / `$HOME` 的**尾部斜杠**会被规范掉（避免出现 `//`），除此之外逐字使用。

> ⚠ 这条「解析一次并缓存」的特性有一个**下游后果**：进程内改环境变量**不再生效**。所以任何「env 覆盖」型契约测试**必须跑在子进程里**（fork 之后重新解析）。`tests/unit/test_paths_unit.c` 的 `t1/t2/t3/t10/t13` 因此统一归「子进程族」。

### 位掩码

- **目录族 6 位**：`PM_DIR_HOME` / `PM_DIR_DATA` / `PM_DIR_LOG` / `PM_DIR_SESSION` / `PM_DIR_RUN` / `PM_DIR_CORPUS`，`PM_DIR_ALL` 为并集。其中 `PM_DIR_CORPUS`（书库）标注「**默认自动建 —— 作者红线**」。
- **数据文件登记表 12 位**：`PM_FILE_STATE` / `BRAIN_CACHE` / `FEATURES` / `CROSS_EDGES` / `MEMORY_SEED` / `EMERGENT_POS` / `CONFIG` / `INTENT_BASE` / `PFE_STRATEGY` / `PFE_WORKSPACE` / `PRETRAIN_EMB` / `TOKEN`（`gw_token`，0600），`PM_FILE_ALL = 0x00000FFFu`。
- 位号 = 数组下标；与 `PM_DIR_*` **同构**：单 bit 合法，`0` / 多 bit / 越界 ⇒ `NULL`。

### `1a2e873`：把「壳层引号」这个坑焊死

Makefile 原本传的是**带引号的字符串**，经过 `make → shell → gcc` 多层之后引号归属不可控。改为传**裸 token**：

```
-DPM_HOME_DEFAULT_PATH=/var/lib/pivotmind
```

引号在**头文件里由 C 的字符串化宏**补齐（`PM_PATHS_STR`）：

```c
#define PM_HOME_DEFAULT PM_PATHS_STR(PM_HOME_DEFAULT_PATH)
```

⇒ 无论经过几层 shell，`PM_HOME_DEFAULT` 都是字符串字面量。定义放在**头文件**而非 `.c`，是为了让测试 TU 等所有消费者看见同一个值。同笔加**硬门**：`PM_HOME_DEFAULT` 必须是绝对路径，否则 `$(error …)` 直接拒绝构建。

> 平台化缺省值现状（`Makefile:19-58`）：Linux ⇒ `/var/lib/pivotmind`；Termux ⇒ `/data/data/com.termux/files/usr/var/pivotmind`。注释里留了一句在册坑：**任何一套构建漏掉 `PM_HOME_DEFAULT`，那一套产出的 `pivotmind_paths` 就少了第 3 级回退**。

### 「就绪」的判定标准

`pm_ensure_dirs()` 对每个位：`lstat` 是**目录**（非文件 / 非坏符号链接）+ `faccessat(W_OK, AT_EACCESS)` 通过，才算就绪。新建目录一律 `mkdir(path, 0700)`；**已存在的目录绝不 chmod**；只读父目录 ⇒ 该位标记为未就绪。返回值为**未能就绪的位掩码**（`0` = 全部就绪），明细逐条 WARN。

## 二、第 2 步：调用点全量替换（`0b6a3fd` + `9fd28f7` + `f500ef2`）

### 分层落地

| 提交 | 层 | 规模 | 内容 |
|---|---|---|---|
| `0b6a3fd` | `src/` | 10 文件 / +129 −18 | 8 个文件 13 处：brainstem、autonomic_learner、health_monitor、train_mode、emergent_pos、cognitive_controller、prefrontal_executive、json_config |
| `9fd28f7` | `demos/` + `tools/` | 22 文件 / +102 −57 | demos/ 25 处 + tools/ 30 处；gateway 接 `pm_ensure_dirs(PM_DIR_ALL)`；加 `__OPTIMIZE__` 守卫 |
| `f500ef2` | `tests/` + `demos/` | 2 文件 / +54 −5 | `test_pfe_unit` 收编到私有 home；`digital_life` 的 `main` 补 `pm_ensure_dirs` |

累计 `1a2e873..f500ef2` = **32 files changed, 285 insertions(+), 80 deletions(-)**（`git diff --shortstat` 实测）。

### 架构决策（作者已确认）

> **数据文件落点从「相对 CWD」改为 `<home>/data/`；`chdir` 保留但只管语料相对路径 —— CWD 决定读什么，SSOT 决定写哪儿。**

这条决策的边界很清楚：`tools/` 里仍能看到 `"data/knowledge_base.json"`、`"data/jieba_dict.txt"`、`"data/hermes_knowledge_base.json"` 这类**相对 CWD 的输入路径** —— 那是**读**输入，**故意保留**（谁读什么由调用者的 CWD 决定）；被替换的是**写**路径。

### 关键实现细节

- **门卫全保留**：`digital_life` 的 D1 防覆盖门卫、`memory_seed` 防呆等逻辑**原样未动**，只把路径来源从字面量换成 `pm_file()`。
- **`__OPTIMIZE__` 守卫**（`9fd28f7`）：某些断言只在优化构建下成立，`-O0` 下跳过 —— 保证 debug/asan 构建不因断言炸掉。
- **`test_pfe_unit` 的隔离顺序是硬约束**：`setup_test_home()` 必须先 `mkdtemp` + `setenv("PIVOTMIND_HOME", …)` + `pm_ensure_dirs(PM_DIR_ALL)`，**且必须早于任何 `pm_*` 调用** —— 因为 `pm_home()` 只解析一次并缓存（见第 1 步）。该文件是 **CRLF**，改动靠 eol 探测保住行尾（diff 49 行，非整文件重写）。
- **有意保留的字面量**：`tools/template_build.c` 的 `pivotmind_state_with_templates.dat` **故意不替换** —— 它是一次性产物，不是跨进程共享文件，不属于登记表。

## 三、第 3 步：`tools/` 层「默认自建」（`2e0bb19`）

### 为什么是这 7 个

第 2 步把 `tools/` 的写路径改成了 `pm_file()`，但**按设计没给工具插 `pm_ensure_dirs`**（当时记为「归第 3 步」）。后果：`tools/` 是**独立进程**，不经过 gateway 的 `pm_ensure_dirs(PM_DIR_ALL)`，一旦 `<home>/data/` 不存在，工具就会「路径是对的、目录是没有的」。

判据是**「该工具是否会写 SSOT 路径」**，逐个查写入点（`master_save_state` / `save_features` / `save_cross_edges` / `fopen(...,"w")`）后确定：

**覆盖（7 个，各 +10 行）**

| 工具 | 写入点 | 行尾 |
|---|---|---|
| `batch_learn.c` | `save_cross_edges` :350、`master_save_state` :591、`save_features` :599、`save_cross_edges` :609 | LF |
| `build_cross_links.c` | `master_save_state(state_path)` :145 | LF |
| `compound_promote.c` | `master_save_state(state_path)` :194 | LF |
| `corpus_train.c` | `save_cross_edges` :475、`save_features` :478、`master_save_state` :479 | LF |
| `hebbian_pretrain.c` | `master_save_state(state_path)` :345 | LF |
| `qa_crawler.c` | `master_save_state(pm_file(PM_FILE_STATE))` :464 / :473（**恒定走 SSOT**） | LF |
| `seed_builder.c` | `master_save_state(state_file)` :420（`state_file` = `pm_file(PM_FILE_STATE)`） | LF |

**按设计不动**

- **纯只读工具（10 个）**：`batch_test` / `compare_templates` / `debug_load` / `debug_seed` / `eval_templates` / `path_analyze` / `probe_batch_contract` / `quick_chat` / `state_dump` / `seed_teacher`。读不到就是「没数据」，**不该产生 fs 副作用**（若无条件 ensure，一个只读诊断工具会在用户主目录里凭空造出 6 个目录）。
- **路径全走 `argv` 的工具（5 个）**：`feed_cli`（`<state.dat> <file...>`）/ `edge_builder`（同）/ `merge_state`（3 个 argv）/ `reader`（输出路径来自 argv）/ `template_build`（输出是 CWD 字面量）。目录由调用方负责，**不属「默认自建」范围**。

### 插入形态（与既有两个入口逐字同款）

插在 `main()` 内、**所有路径变量声明之后**（避免 `-Wdeclaration-after-statement`），块作用域内声明 `bad`（变量名冲突从作用域上不可能）：

```c
    /* 路径 SSOT：数据目录默认自建（未就绪则加载/存盘会失败）。
     * 与 gateway / digital_life main 同款处理；不拒绝运行（只读环境下仍可做只读查询）。 */
    {
        unsigned bad = pm_ensure_dirs(PM_DIR_ALL);
        if (bad != 0u)
            fprintf(stderr, "[paths] ⚠ 部分数据目录未就绪 (mask=0x%x)：加载/存盘会失败\n", bad);
        else
            printf("[paths] 数据根: %s\n", pm_home());
    }
```

**为什么用 `PM_DIR_ALL` 而不是只建 `PM_DIR_DATA`**：与既有 `gateway` / `digital_life` **保持同一约定**；`pm_ensure_dirs` 本就设计为「无状态幂等、可重复调用」的唯一写入口；建全树也避免将来工具加了日志（`pm_log_path`）再回头改。副作用上限 = 在**程序自己的家目录**下多几个空目录，与 `gateway` 行为一致。

**改法**：用带断言的 Python 脚本（每处锚点 `count(old) == 1` 才替换，否则整体 abort）逐文件改，`newline=''` 逐字读写保住行尾 —— 实测 7 个文件**全 LF**，`CRLF=0`。

## 四、`digital_life` 复活（`ab1853e`，3 文件 / +260 −28）

### 病灶

`demos/digital_life.c` 仍引用 `BackgroundClock`，而该类型在 **`caa6e36`（V0.3.0，2026-06-11）** 就被 `Brainstem` 取代 ⇒ **对 v0.3.0 之后的树根本编译不过，坏了约 3 个月**。

**为什么 3 个月没人发现**：`Makefile` 里 `digital_life` 有独立规则（`:117` 编译规则 / `:207` `digital-life` 目标 / `:224` `run`），但**没进 `all:`** ⇒ CI 跑 `make linux`（= `clean + all`）、本地跑 `make all`，**全都覆盖不到它**。这是漏检的**机制性原因**。

### 修复依据（1:1 证据，不是猜）

- `git show caa6e36 --stat`：`brainstem.h +74` / `brainstem.c +386`，`background_clock.h −110` / `.c −350` ⇒ **brainstem 是 background_clock 的演化继承者**。
- 签名比对：`Brainstem* brainstem_create(MasterTopology*, MemorySystem*, CognitiveState*)` 与旧 `background_clock_create(topology, memory, cognitive_state)` **参数类型与顺序完全一致** ⇒ **纯符号替换，零适配**。
- **旧 `background_clock.h/.c` 里完全没有 thalamus**（grep 为空）；`brainstem.h` 结构体虽有 `Thalamus* thalamus`，但 `brainstem_create` **不绑**（`calloc` 后为 NULL）。
- 所有 `thalamus_*` 公开函数都判空返回安全默认（`is_region_enabled`→0、`get_region/get_utility`→NULL、`get_throttle`→1.0f、`tick/set_circadian`→return）⇒ **未绑丘脑是安全降级，不崩**。
- `PM_CLOCK_TICK_INTERVAL_MS` / `PM_CLOCK_DECAY_PER_TICK` 在 `include/constants.h`，新旧共用 ⇒ `digital_life` 行 244 无需改。

⇒ **映射到 brainstem 是根因修复**（它是功能超集，额外有 `get_real_time/get_uptime/get_circadian/get_circadian_phase` + `health_monitor`）；**摘除反而会丢掉昼夜节律**。

全量映射共 **1 include + 1 字段 + 6 调用点**：`BackgroundClock*`→`Brainstem*`；`background_clock_create/destroy/start/stop`→`brainstem_*`；`background_clock_set_verbose`→`brainstem_set_verbose`；`bg_clock->tick_count`→`brainstem_tick_count(bs)`；`#include "background_clock.h"`→`"brainstem.h"`。修完后 `background_clock` 在**整个仓库的 `.c/.h` 里彻底消失**（只剩 `include/dream_engine.h:11` 一句注释文字），与 `caa6e36` 的重构意图完全对齐。

### ⚠ 诚实记录的行为差异

旧 `BackgroundClock` 内部**自建 `self_learner`**（`self_learner_create(master, NULL)`）；新版把自主学习收归丘脑 utility slot **`THAL_UTIL_SELF_LEARNER`** ⇒ **未绑丘脑时该路径被跳过**。`digital_life` 未装配丘脑，因此：

- **与旧一致**：tick / 衰减 / 自发激活 / 认知状态漂移 / 周期存盘。
- **仍由 `ActiveLearner`（`sys->learner`）承担**：自主学习。
- **要接全**：需对齐 gateway 的完整装配（创建 thalamus + 注册各脑区），属独立话题。

### 同笔补的契约单测（`tests/unit/test_paths_unit.c` 10 → 13 条，+250 行）

第 2 步补测前，`test_paths_unit.c`（526 行）里 `pm_file(` / `pm_log_path(` 的调用次数是 **0** —— 也就是说 **「非法 which 返回 NULL」这条核心契约完全没有单测覆盖**（只间接经 `pm_path` 覆盖了底层）。本笔补齐：

- **`t11 pm_file`**：12 项登记名全部硬编码断言 + 与 `pm_path(PM_DIR_DATA, name)` **交叉验证** + 指针稳定（多次调用同址）+ 12 项**两两互异** + `PM_FILE_ALL == 0xFFF` + 非法位（`0` / `1<<12` / `1<<31` / 多 bit / 全 1）⇒ `NULL`。
- **`t12 pm_log_path`（默认分支）**：`<home>/log/pivotmind.log`，且**不与 12 个数据文件撞车**。
- **`t13 $PIVOTMIND_LOG_FILE`（覆盖分支）**：绝对路径采用 / 尾斜杠规范 / **空串静默回退（不 WARN）** / 相对路径 WARN 回退 / 未设置回退。**必须归「子进程族」** —— 见第 1 步的 `pthread_once` 缓存说明。`main` 里把 `t13` 与 `t1/t2/t3/t10` 放同一批（共 5 条），且**父进程沙箱补 `unsetenv("PIVOTMIND_LOG_FILE")`**，保证 `t12` 断言的是默认分支。

> **本笔过程中我自己引入过并当场被编译器抓住的一个 bug**：`t13` 里把 `log_child_result*` 传给了形参为 `const child_result*` 的 `child_exited_clean()`。两个结构体的 `status` 字段**偏移不同**（`child_result` 前面多了 `ensure_rc` / `ensure_mask`）⇒ 读到**错误内存**，`t13` 的 5 个退出码断言**全是假判定**。GCC 13 只给 `-Wincompatible-pointer-types` **warning**（**GCC 14 起是 error**）——若只看「27/27 通过」就会漏掉。**根治**：把 `child_exited_clean()` 改成收 `int status`（10 处调用点传 `r.status`）⇒ 「传错结构体指针」这类错误**从类型上不可能再发生**。教训已记：**`make test` 全绿 ≠ 干净，必须看 warning 计数。**

## 五、七笔尾巴（v0.5.29 之后落的）

| 提交 | 一句话 |
|---|---|
| `6dacf75` | 删死函数 `remove_cross_topology_link`（**第三种破契约路径**，零调用者） |
| `1128813` | 删死函数 `insert_cross_topology_link`（**自首个提交起从未被调用**） |
| `84176c0` | `emergent_pos.bin` 加**维度头**，并**拒绝无法推断维度的文件** —— 治 v0.5.28 降维 512→256 后「旧文件被新二进制静默错读」的风险（v0.5.29 Known Issues 里明确登记为「仅靠部署时移开旧文件，代码未加防线」，本笔补上了防线） |
| `d3b30dd` | `cross_hit_hash` 改用**无符号**算术（治溢出） |
| `7995028` | 加载期把**被当作重复吸收**的跨链引用**记账** —— 续 v0.5.29「把静默变成会喊」的主线 |
| `0c7a960` | RNN 层的**权重槽初始化** + 释放自身持有的张量 |
| `0c172a2` | 两份 README 挂 CI 状态，并写明 CI **覆盖什么、不覆盖什么** |

## 改动文件（全量）

```
 Makefile                          |  50 +++-        （PM_HOME_DEFAULT 平台化 + 硬门 + sync/check-version + digital-life 进 all）
 include/pivotmind_paths.h         | 100 ++++        （新增）
 src/pivotmind_paths.c             | 323 ++++++++    （新增）
 include/brainstem.h               |  (无本版改动；digital_life 修复引用它)
 demos/digital_life.c              |  46 +++---      （background_clock→brainstem + pm_ensure_dirs）
 demos/pivotmind_gateway.c         |   9 +++-
 src/{brainstem,autonomic_learner,health_monitor,train_mode,
      emergent_pos,cognitive_controller,prefrontal_executive,
      json_config}.c               |  13 处
 tools/{batch_learn,build_cross_links,compound_promote,corpus_train,
        hebbian_pretrain,qa_crawler,seed_builder,...}.c           | 30 处 + 第3步 7 处
 tests/unit/test_paths_unit.c      | 776 ++++------   （526 新建 + 250 扩到 13 条）
 tests/test_pfe_unit.c             |  49 ++++--
 README.md / README.zh-CN.md / ARCHITECTURE.md |  版本锚点 7 处由生成器同步
 include/pivotmind_version.h       |  bump 0.5.29 → 0.5.30
 CHANGELOG.md                      |  顶部新增 v0.5.30 节
 changelogs/074-paths-ssot-three-steps.md | （本文件）
```

## 编译验证

### 双架构三机（均为 `make debug`，即 `-O0`、**无 LTO**）

| 机器 | 架构 / 编译器 | `make debug` | 7 工具逐个编译 | `make test` | 空 home 冒烟 |
|---|---|---|---|---|---|
| **WSL** | x86_64 / **gcc 15.2.0**（20 核，7.0 GB avail） | rc=0，**0 error**，11 warning（全为既有），1s | **7/7 rc=0**，0 error，**0 新增警告** | **27 通过 / 0 失败** | ✅ 6 目录自动建出 |
| **armbian-1** | aarch64 / **gcc 13.3.0**（6 核，2.9 GB avail） | rc=0，**0 error**，13 warning（全为既有），3s | **7/7 rc=0**，0 error，**0 新增警告** | **27 通过 / 0 失败** | ✅ 6 目录自动建出 |
| **astar728-1 (Pi 3B)** | aarch64 / **gcc 14.2.0**（905 Mi） | **不跑**（`-O2`/LTO 会挂机，见 v0.5.29 事故记录） | `gcc -fsyntax-only -Wall -Wextra` **7/7 rc=0** | — | — |

> **gcc 15.2 这一档是额外价值**：仓库此前主要跑 gcc 13/14，而「GCC 14 起 `-Wincompatible-pointer-types` 由 warning 变 error」正是上文那个自引入 bug 的教训 —— 用最新的编译器过一遍能提前撞到这类问题。

### 「默认自建」的运行时铁证（WSL + armbian 双份）

以**空** `PIVOTMIND_HOME` 跑 `build_cross_links`（该工具 `argc==1` 时路径取 `pm_file(PM_FILE_STATE)`）：

```
[paths] 数据根: /tmp/pm_s3_build_cross_links
╔═══════════════════════════════════════════╗
║    跨拓扑连接构建工具 v1.0                ║
╚═══════════════════════════════════════════╝
[1/3] 加载拓扑状态...
  × 找不到状态文件: /tmp/pm_s3_build_cross_links/data/pivotmind_state.dat
  exit=1
  --- 目录树 ---
    drwx------  /tmp/pm_s3_build_cross_links
    drwx------  /tmp/pm_s3_build_cross_links/corpus
    drwx------  /tmp/pm_s3_build_cross_links/data
    drwx------  /tmp/pm_s3_build_cross_links/log
    drwx------  /tmp/pm_s3_build_cross_links/run
    drwx------  /tmp/pm_s3_build_cross_links/session
```

三件事一次性成立：① 路径**正确落在 SSOT 的 `<home>/data/` 下**；② 目录**先于它被建好**（否则报的会是「无法打开」而不是「找不到」）；③ 新建目录 mode **`0700`**（`drwx------`）。`compound_promote` 同款复现。`qa_crawler` 则走了真网络 RSS（33 查询 × 3s delay），真写出 `<home>/data/pivotmind_state.dat`，并在退出时被**绝对下限防呆**拦下（`当前 0 可持久化节点且文件已存在，拒绝覆盖`）—— 该门卫**未被本版触碰**，行为符合预期。

### `make test` 内含的两道门禁（均 PASS）

- `check-locks`（`tests/tools/check_lock_discipline.py`，锁纪律静态检查）：`写者集合（会取 master 写锁的函数）: 19 个`，`✅ 未发现「持 master 读锁期间取 master 写锁」的站点`，`LOCK-DISCIPLINE: PASS`。
- `check-version`（`tools/check_version_consistency.py`）：`VERSION-CONSISTENCY: PASS  真值源 include/pivotmind_version.h = 0.5.30；活文档 3 份 / 锚点 7 处一致；CHANGELOG 最新节无未更正的陈旧断言`。

### 既有警告清单（**非本版引入**，两架构一致）

- `tools/build_cross_links.c:91` `unused variable 'CROSS_WEIGHT'`（`-Wunused-variable`）
- `tools/corpus_train.c:185` `-Wformat-truncation`

（WSL `make debug` 的 11 条与 armbian 的 13 条 warning 全部落在 `prefrontal_executive.c` / `visual_cortex.c` / `multi_topology.c` / `gateway_system.c` / `gateway_handlers.c` 的 `-Wformat-truncation` / `-Wrestrict`，均为既有，与本版无关。）

## 已知未做

1. **🔴 6/7 工具不在构建系统里（本版新发现）**：`all: $(LIB_NAME) seed-builder debug-seed gateway digital-life` 只含 `seed-builder`；`corpus_train` / `qa_crawler` / `batch_learn` 有规则但**不在 `all`**；`build_cross_links` / `compound_promote` / `hebbian_pretrain` **连编译规则都没有**。⇒ 第 3 步的 7 个改动里**只有 `seed_builder` 会被 CI / `make all` 编译**。机制上与 `digital_life` 同类。**未处理，留给作者拍板**（补规则 + 进 CI，还是明确标废弃）。
2. **`tools/batch_learn.c:576`** 的 `char path[512]` + `snprintf(…, 511, …)` 装完整 SSOT 路径，理论可截断（低风险，宜放宽到 `PM_PATH_MAX`）。**未改。**
3. **`include/json_config.h:83`** 注释「NULL 则尝试 pivotmind_config.json」已不准确。**未改。**
4. **未跑 ASan / UBSan / TSan**（本版）。v0.5.29 也未跑。
5. **未做端到端真实数据演练、未部署。**
6. **未开 PR 到 `main`** —— `ci.yml` 的 `push` 只监听 `main`/`master`/`develop`，推 feat 分支**实测 `total_count: 0`**（不触发 CI）。开 PR 是唯一途径。
7. **部署前置（承接 v0.5.29，仍有效）**：真实 `fmt_ver=9` 载荷会被状态闸门**有意拒绝** ⇒ 直接部署 = 「启动即拒绝、空壳运行」，**不可用开关绕过**；且数据文件落点已从 `<home>/` 改到 `<home>/data/`，部署时需把旧文件移入 `data/`，否则空壳启动（**旧文件不丢，只是被忽略**）。

## 红线声明

- 本版只动「**路径落点**」+ 两个死函数 + 一处维度头 + 一处无符号算术 + 一处加载期记账。
- **限边 / 截断 / 周期性稀疏化 / 跨拓扑上限 / 队列满丢任务**逻辑**一律未触碰**。
- `pm_ensure_dirs()` 是**幂等无状态**的 `mkdir`（`0700`）：**已存在的目录绝不 chmod**，只读环境下只报未就绪位、**不拒绝启动**（gateway 与 digital_life 的注释都写明这条）。
- 第 1 步的「只立真值源、不动调用点」是**刻意的分段**：真值源与替换分离，才能在替换出问题时把范围一刀切回去。

## 环境坑（本轮新增，可复用）

1. **Pi 3B 上绝不跑 `make all`（`-O2` + `-flto`）**：LTO 链接把所有 `.o` 的 GIMPLE 一起吞进内存，峰值 1–2 GB；Pi 只有 905 Mi + zram 452 M + USB swapfile 2 G，换页打到 USB 即 **D 状态挂死**（2026-09-12 事故，1 小时无自愈，只能物理断电）。**`make debug`（`-O0`、无 LTO）是可跑的安全路径。**
2. **构建前先 `free -m`**，内存余量 < 300 M 不开并行；长任务必须 `nohup`/`setsid` 丢后台，**不要挂在 SSH 会话里等**。
3. **本机（Windows）Bash 工具的 PATH 会坏**（`dirname`/`head`/`grep`/`ls` 全 `command not found`）⇒ 临时前缀 `export PATH="/usr/bin:/bin:$PATH"` 即可恢复。
4. **给 Windows 原生 Python 传路径要用 `D:/...` 形式**（`/d/...` 只有 MSYS 工具认）。
5. **PowerShell 管道会把 UTF-8 中文显示成 GBK 乱码**：**不要凭乱码判定远端文件坏了** —— 用 `md5sum` + `od -An -tx1` 看原始字节（`修复` = `e4 bf ae e5 a4 8d`）。
6. **改大文件用带断言的 Python 替换脚本**（每处 `count(old) == 1` 才替换，任何偏差立即 `exit` 非 0），比手改可靠；写完校验 `CR/LF` 计数保住行尾。
7. **WSL 的默认用户与仓库位置会变**：本轮实测默认用户是 `cx`（非 root，无 sudo），`/root/pm-verify` 已不存在；`/home/cx/*` 里多个目录是**剥掉 `.git` 的源码副本** ⇒ 验证前先 `git rev-parse` 探明，或直接用 bundle 建干净克隆。
