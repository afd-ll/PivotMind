# 077 — 旧扁平布局 fail-loud 门 + 长跑脚本隔离洞 + `snprintf` 字面量尺寸体检

- **版本**：v0.5.33（v0.5.32 → v0.5.33）
- **日期**：2026-09-13
- **工作区**：`/home/cx/pm-fix`（Pi 3B），分支 `feat/paths-callsite-migration`；`main` 未动
- **来源**：老大「**全部修复**」—— 修 v0.5.32 收尾时挖出的两个高危问题，外加一轮全仓 `snprintf` 尺寸体检
- **前置**：[076 — 删除 batch_learn_lowmem 变体 + 清除死宏 CROSS_REBUILD_INTERVAL](076-drop-batch-learn-lowmem-variant.md)

---

## 一、问题 A：长跑脚本的「沙箱」是假的

**文件**：`tests/longrun/run_longrun_guard.sh`

**现象**：脚本有 `--data DIR` 参数，看起来能在沙箱里跑压测。但路径 SSOT 落地（v0.5.30）之后，
**这个隔离已经失效**。

**根因**：脚本只把 `$DATA` 当 **argv** 传给引擎，**从不设 `PIVOTMIND_HOME`**。而

- 数据落点由 `pm_home()` 决定（`$PIVOTMIND_HOME` → `$HOME/pivotmind` → 编译期缺省）；
- `demos/pivotmind_gateway.c:483` 已明确注释：**argv 只喂 `chdir()`**（给语料相对路径用）。

⇒ 不设 `PIVOTMIND_HOME` 时，引擎会读写 **`$HOME/pivotmind`（= 线上数据目录）**。
脚本自以为的沙箱，写的是真脑子；而且**不报错**。

**同一个脚本里的第二个洞**：`TOKEN_FILE` 还硬编码着 `/home/cx/pivotmind/gw_token`。
`GW_TOKEN_FILE` 早已收编为 `pm_file(PM_FILE_TOKEN)` = `<home>/data/gw_token`，
这行硬编码既过时、又正好指着线上目录 —— 起网关时若 token 文件不存在它会**新建**，直接污染线上数据根。

**修复**（脚本内四处）：

```sh
# 沙箱守卫之后，立刻把 SSOT 钉死在沙箱上
export PIVOTMIND_HOME="$DATA"
```

- `--data` 用法注释下补 4 行说明：为什么必须 export（argv 只喂 chdir，不设就会打到 `$HOME/pivotmind`）；
- `TOKEN_FILE="/home/cx/pivotmind/gw_token"` → `TOKEN_FILE="$DATA/data/gw_token"`；
- 启动横幅加一行 `PIVOTMIND_HOME=...`，让**跑之前眼睛就能看到**落点；
- 令牌注释标题/正文同步更正（「不再是编译期绝对路径」）。

---

## 二、问题 B：旧扁平布局 ⇒ 原地升级会「静默失忆」

**现象**：线上 `/home/cx/pivotmind/` 是 v0.5.28 的**扁平布局**（`pivotmind_state.dat` 等
文件直接躺在根目录）。而 v0.5.33 的 SSOT **只读 `<home>/data/`**。把新二进制换上去：

> 引擎在 `<home>/data/pivotmind_state.dat` 找不到主状态 → **新建空状态 → 正常启动 → 不报错**。

表现就是「**升级后玄枢失忆**」。这是本次最危险的一条：**没有崩溃、没有日志、没有任何信号**，
训练了几十万条 QA 的脑子看起来「还在」，实际从零开始。

**修复**：新增两个导出函数（不动 `pm_ensure_dirs` 的「不写文件系统」契约），加在
`include/pivotmind_paths.h` + `src/pivotmind_paths.c`：

| 函数 | 职责 |
|---|---|
| `int pm_legacy_layout_report(char *report, size_t cap)` | **纯查询**：遍历 12 个登记文件，若 SSOT 落点 `<home>/data/<name>` **不存在**、而旧落点 `<home>/<name>` **存在**，计为命中。命中清单逐行写入 `report`（`  · <name>`），容量不足则整行不写。返回命中件数。 |
| `int pm_legacy_layout_guard(const char *who, int refuse)` | **fail-loud 门**：无命中 ⇒ 静默返回 0；有命中且 SSOT 主状态已就位 ⇒ `WARN` 清单、返回 0；有命中且主状态缺失 ⇒ `ERROR` 清单，`refuse != 0` 时返回 1。 |

**接线（4 个入口）**：

| 入口 | 策略 | 理由 |
|---|---|---|
| `demos/pivotmind_gateway.c` | **硬拒**（`refuse=1`，`return 1`） | 生产网关；静默空脑是最坏结果，必须拦死 |
| `demos/digital_life.c` | 只告警（`refuse=0`） | 前台交互程序，操作者当场可见 |
| `tools/batch_learn.c` | 只告警 | 训练工具，就地提示 |
| `tools/quick_chat.c` | 只告警 | 交互式 CLI |

**逃生开关**：`PIVOTMIND_ALLOW_LEGACY_LAYOUT=1` ⇒ 显式放行（返回 0），但**仍打 WARN**。
用途：确实想以空脑启动（例如故意重训）。

**配套迁移脚本**：`deploy/migrate-home-layout.sh`（见下）。

---

## 三、问题 C：全仓 `snprintf` 字面量尺寸体检（21 处）

起因：076 收尾时在 `gateway_handlers.c:618` / `pivotmind_gateway.c:236` 见到
`snprintf(buf, 128, ...)` 这类**字面量尺寸**写法。字面量本身不一定错 —— 问题在于
**没有任何机制保证它跟缓冲区声明一致**。于是把全仓 21 处一次查清。

判定原则：

- 缓冲区是 `char buf[N]` ⇒ 写成 `N-1` 是 **off-by-one**（安全但白丢一字节）；写成 `N` 正确。
- 缓冲区是 `char *p = malloc(N)` ⇒ `snprintf(p, N, ...)` 正确；**改成 `sizeof(p)` 只会得到指针大小 8**，是引入 bug。
- `snprintf(dst + j, K, ...)` ⇒ `K` 是**剩余容量**，与「转义输出宽度」配对，正确。

### 3.1 ★ 真 bug（1 处）

**`src/template_builder.c:1016`**

```c
snprintf(tn->tpl_connectors[k], 8, "%s", conn);          // 改前
snprintf(tn->tpl_connectors[k], sizeof(tn->tpl_connectors[k]), "%s", conn);   // 改后
```

字段声明是 `char tpl_connectors[4][TPL_CONNECTOR_BUF]`，`TPL_CONNECTOR_BUF = 32`，
头注释写着「UTF-8 中文约 10 字」。**写死 8 ⇒ 连接词被静默截到 7 字节**（中文只剩 2 字）。
更糟的是：它参与**合并键归一化** ⇒ 不同连接词会**撞成同一个键**，静默串味。

### 3.2 off-by-one（5 处，安全但白丢末字节）

| 位置 | 原写法 | 缓冲区 |
|---|---|---|
| `src/dialog_system.c:1616` | `255` | `char cause_key[256]` |
| `src/dialog_system.c:1617` | `255` | `char effect_key[256]` |
| `src/dialog_system.c:1692` | `127` | `char intent_key[128]` |
| `src/dialog_system.c:1916` | `63` | `char fb[64]` |
| `src/hippocampus.c:43` | `1023` | `char dialog_log[][1024]` |

统一改 `sizeof`。长输入本会在最后一个字节被悄悄砍掉。

### 3.3 改为 `sizeof`（等价，纯归正，4 处）

| 位置 | 字面量 | 缓冲区 |
|---|---|---|
| `src/dream_engine.c:84` | `512` | `char questions[][512]` |
| `src/dream_engine.c:85` | `2048` | `char answers[][2048]` |
| `src/perception.c:1425` | `128` | `char expanded[][128]` |
| `demos/gateway_handlers.c:618` | `128` | `char rs[128]` |
| `demos/pivotmind_gateway.c:236` | `64` | `char rs[64]` |

行为逐字节不变，只是把「尺寸」与「声明」绑死。

### 3.4 ⛔ 必须保留字面量（7 处，改 `sizeof` 就是引入 bug）

全部是 `malloc(N)` 后 `snprintf(ptr, N, ...)` —— **`sizeof(ptr)` = 8**：

| 位置 | 字面量 | 现场 |
|---|---|---|
| `src/dialog_system.c:1650` | `256` | `char *response = malloc(256)`（上方有 OOM 分支） |
| `src/causal_reasoning.c:1147` | `512` | `char *explanation = malloc(512)` |
| `src/concept_processor.c:198` | `64` | `char *output = malloc(64)` |
| `src/concept_abstraction.c:321` | `32` | `(*output_patterns)[i] = malloc(32)` |
| `src/concept_abstraction.c:397` | `64` | `(*patterns)[i] = malloc(64)` |
| `src/perception.c:1319` | `256` | `char *result = malloc(256)` |
| `src/perception.c:1361` | `128` | `char *result = malloc(128)` |

**保持原样**，并在本文档留档说明 —— 免得下次「体检」又被当成漏网之鱼改错。

### 3.5 ⛔ 必须保留字面量（2 处，「剩余容量 + 转义宽度」配对）

| 位置 | 字面量 | 守卫 | 说明 |
|---|---|---|---|
| `demos/gateway_http.c:40` | `8` | `for (...; j < dst_size - 8; ...)` | `\uXXXX` = 6 + `\0` = 7 ≤ 8 字节，正确 |
| `demos/pivotmind_gateway.c:63` | `3` | `for (...; o + 2 < cap; ...)` | `%02x` = 2 + `\0` = 3 字节，正确 |

### 3.6 契约补注（1 处）

`include/topology_growth.h`：`diagnose_topology()` 的 `report` 参数**没有尺寸参数**，
调用者无从知道该给多大。补注：

```
* @param report 输出报告 (可为 NULL；**非 NULL 时缓冲区至少 256 字节**)
```

该函数**仓库内无调用方**，故**未改签名**（避免动 ABI）。

### 3.7 汇总

| 判定 | 处数 | 处置 |
|---|---|---|
| ★ 真 bug（静默截断 + 键碰撞） | 1 | 改 `sizeof` |
| off-by-one | 5 | 改 `sizeof` |
| 等价归正 | 4+1 = 5 | 改 `sizeof` |
| 保留（`malloc` + 指针陷阱） | 7 | 不动，本文档留档 |
| 保留（剩余容量） | 2 | 不动，本文档留档 |
| 契约补注 | 1 | 改注释 |
| **合计** | **21** | |

---

## 四、新增文件

### `deploy/migrate-home-layout.sh`

旧扁平布局 → SSOT 布局迁移。**只搬不删、绝不覆盖**。

- 默认**只打印计划**；`--yes` 才动手；`--dry-run` 显式演练。
- 检测到在用实例（扫 `/proc/*/environ` 的 `PIVOTMIND_HOME` 与 `/proc/*/cwd`）⇒ **拒绝运行**。
- 同盘 `mv -n`（原子改名）；两边都在 ⇒ **跳过并告警**，交人工裁决。
- 跨文件系统时告警：`mv` 会退化为「复制 + 删除」。
- 数据根合法性检查与 `pm_home()` 同款（非空、绝对、不含 `~`、不是 `/`）。
- 回滚：把 `<home>/data/<name>` 搬回 `<home>/<name>` 即可。

### `deploy/README.md`

数据根布局表、`pm_home()` 三级回退、12 个数据文件清单、**升级步骤**、
fail-loud 门行为矩阵、`PIVOTMIND_ALLOW_LEGACY_LAYOUT` 逃生开关、
systemd 必须显式 `Environment=PIVOTMIND_HOME=` 、以及「长跑脚本隔离铁律」。

---

## 五、验证

- **三台机器、三个编译器** `make clean && make all`：

| 机器 | 架构 | 编译器 | 结果 |
|---|---|---|---|
| Pine64 / Pi 3B | aarch64 | gcc 14.2.0 | 0 error / 0 warning |
| armbian-1（RK3399） | aarch64 | gcc 13.3.0 | 0 error / 0 warning |
| WSL Ubuntu 26.04 | x86_64 | gcc 15.2.0 | 0 error / 0 warning |

- `make check-tools`：**✓ 19/19** 工具产出。
- `make check-version`：**PASS**（版本号 SSOT 一致）。
- 迁移脚本在 Pi 上以「演练模式」对**构造的旧扁平布局沙箱**验证：`--dry-run` 不改动任何文件；
  `--yes` 后 12 个文件全部落位 `data/`，旧位置清空，`data/` 原文件（若有）未被覆盖。

## 六、诚实边界与部署阻塞

- **部署阻塞未解除**：线上真实数据是 `fmt_ver=9` 载荷 + **旧扁平布局**。本次加的是
  **fail-loud 门**（让问题可见）+ **迁移脚本**（让问题可修），但**没有**动状态格式闸门。
  ⇒ 直接部署仍会**启动即拒绝**。正确顺序是：**先跑 `migrate-home-layout.sh` 搬数据**，
  再处理 fmt_ver 闸门（那是另一件事，见 v0.5.28 的降维/版本闸门记录）。
  **本次不解除阻塞，只是把「静默失忆」变成「响亮拒绝」。**
- `snprintf` 体检只覆盖**字面量尺寸**这一类写法。`snprintf(buf, sizeof(buf), ...)` 这类
  本来就正确的写法不在本次范围（也无需改）。
- `pm_legacy_layout_report` 的命中判定是「SSOT 落点不存在 && 旧落点存在」。
  **两边都在**的情况**不算命中**（SSOT 优先，旧文件被忽略）—— 这是刻意的：
  「两边都有」时 SSOT 就是权威，不该报警。代价是「两边都有」的旧文件不会被提示清理。
- 迁移脚本按**文件名清单**搬运，清单是 `src/pivotmind_paths.c` 里 `g_filename[]` 的**副本**。
  这是 SSOT 的代价：**改一处必须同步改另一处**，脚本头注释已写明。
