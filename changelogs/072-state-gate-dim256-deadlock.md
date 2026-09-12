# v0.5.28 — 状态加载版本闸门 / 特征维度 512→256 收进 main / 每 11 分钟自死锁修复

> **日期**: 2026-09-11 | **类型**: 修复
> **状态**: **工作区改动（未提交、未部署）** —— HEAD `efcf907`，工作区 **15 个文件脏**（`+99 / −34`），三条线并行改动，**均未 `commit`、均未部署**到任何线上服务。
> **来源**: ① `fix-plans/state-fmt-gate-fix.md`（33 KB / 338 行）② `fix-plans/dim512to256.md`（16 KB / 220 行）③ `fix-plans/deadlock-branchstem-fix.md`（30 KB / 436 行，**A/B 长跑结论已补完并据实回填于「验证」节**）+ `fix-plans/deadlock-rwlock-audit.md`（46 KB）+ `freeze-forensics.md`（现场取证）；另见 `pending-tails.md`、`STATUS.md` §8/§9。
> **说明**: 本文件只写文档。`src/multi_topology.c` 同时承载第 ① 项的「版本闸门」与 `dim512to256.md` 里记的「清单外 48 行改动」——**两者是同一处改动**，不是两条线。

## 概述

本轮三条改动都已在 armbian 上做过真实验证（数字见「验证」节），共同主线是**把「静默」变成「会喊」**：

| # | 改动 | 位置 | 一句话 |
|---|------|------|--------|
| ① | **状态加载版本闸门** | `src/multi_topology.c` `master_load_state()` | 旧代码对不认识的 `fmt_ver` 一律 `p = buf` 回退当 v1 **猜解析**——5.17 MB 的 `fmt_ver=9` 真状态文件被静默读成「1 节点」（事故口径 **3,860 节点无声灭失**）。改后：未来版本 / 非正数 → 显式 `LOG_ERROR` + `return -1`，**绝不再猜** |
| ② | **特征维度 512→256 收进 main** | `include/constants.h` 等 **13 文件** | 真值源降为 256，顺带**收敛 4 处分散定义** + 2 个 Python 工具 |
| ③ | **每 11 分钟必死的自死锁** | `src/brainstem.c` | `brainstem_tick_synapse_scale()` 持 master **读锁**期间调用会取**同一把锁写锁**的 `master_reevaluate_cross_links()` → glibc 同线程「读→写」升级 = **永久自死锁**；只在 `tick%600==0` 进门 ⇒ **启动约 11 分钟必死**。属**修复引入回归**（引入点 `3d2f7cba` / v0.5.24） |

三者独立成条、互不覆盖：① 堵数据「静默丢」，② 收敛维度「静默错读」的根，③ 治引擎「静默停摆」。**上述改动目前均为工作区状态，未经 git 提交、未经部署。**

## 核心变更

### ① 状态加载版本闸门（`src/multi_topology.c` `master_load_state()`）

#### 病根

旧代码（`git diff` 的 `−` 侧）：

```c
    int fmt_ver = 1;
    READ(&fmt_ver, sizeof(int));
    if (fmt_ver != 2 && fmt_ver != 3 && ... && fmt_ver != 8) {
        p = buf;       // 回退到文件头
        fmt_ver = 1;
    }
```

即「**白名单不命中 ⇒ 当成最早期无版本头文件**」。三宗罪（报告 ①）：

1. **未来版本被当旧版本猜解析**：`fmt_ver=9`（磁盘布局未知）落到 `fmt_ver = 1` 分支，按 v1 布局读，读到多少算多少。
2. **零记账**：这条回退路径**不打任何 ERROR/WARN**，上层只看到「加载成功 + 节点数」，无法察觉几乎全丢。
3. **静默丢数据**：实测 5,176,666 B（5.17 MB）的 `fmt_ver=9` 真文件被未改二进制读成 **「完成: 1 节点, 0 链接」**、`LOAD_RC=1`、判 **ACCEPTED**，全程**零 ERROR 零 WARN**（证据：`results2/F1_ctrl__real_v9.dat.out`、`results/gate_harness_ctrl__real_v9.dat.out`）。事故口径 **3,860 节点 → 1 节点无声灭失**（报告已标注该数量级**属引用上一任/上级口径**，非报告作者独立取证）。

> **为什么 v1 兼容路径必须保留**：`fmt_ver` 字段是后加的，最早期文件**没有版本头**，首 4 字节就是首个节点的 `node_id`（历史上常为 0）——所以「无版本头」必须能被识别，但**只能靠显式开关或首字段恰为 1 来识别，绝不能靠「猜不出来的都当它」**。

#### 修复（最终版 / 第二版）

改动**只落在** `master_load_state()` 的版本判定处（`#define STATE_FORMAT_VERSION 8` 在 `:3802`）。三分支：

- **`fmt_ver > STATE_FORMAT_VERSION`（未来版本）** → `LOG_ERROR` + `free(buf); return -1`。该判断**在开关判断之前**，故 **`PIVOTMIND_ALLOW_LEGACY_STATE` 对未来版本永远无效**——布局未知，放行 = 猜解析 = 静默丢数据。新增格式版本的**正确做法是把 `STATE_FORMAT_VERSION` 抬上去**，而不是靠开关绕过闸门。
- **`fmt_ver < 1`（0 / 负数）** → 默认 `LOG_ERROR` + `return -1`（日志含「请设 `PIVOTMIND_ALLOW_LEGACY_STATE=1` 后重试」提示）；仅当 `PIVOTMIND_ALLOW_LEGACY_STATE` **严格等于字符串 `"1"`** 时，才 `p = buf; fmt_ver = 1;` 走 v1 兼容路径 + `LOG_WARNING` 记账。
- **`fmt_ver == 1`** → **补回 `p = buf`**（v1 无版本头，首 4 字节即首节点 `node_id`）+ `LOG_WARNING` 记账。

开关仿**种子侧先例** `PIVOTMIND_ALLOW_LEGACY_SEED`（「只有显式开关才按旧格式一次性迁移」）。开关语义（报告 ④，逐条有 armbian 实证）：

| 取值 | 行为 | 实证 |
|------|------|------|
| 未设 | 拒绝（ERROR，含开关提示） | B1 / B5 / B7 |
| `0` | **同样拒绝**（不是假值判断） | B9 |
| `yes` | **同样拒绝** | B10 |
| `1` | 放行，按无版本头 v1 解析，WARN 记账 | B2 / B6 / B8 |

三条硬边界：**只对 `fmt_ver<1` 生效**（`fmt_ver==1` 不需要开关）；**对未来版本永远无效**；**开关命中 ≠ 校验通过**（`fmt_ver<1` 下「真 v1」与「损坏文件」内容上无法区分，人造垃圾在开关下也被「成功」读出 1 节点）——语义是「**信任来源 + 显式记账**」。

> **瑕疵 A（第一版 → 第二版的回归）**：第一版只加了拒绝分支、**漏了 `p = buf`**，把真实 v1 文件从 5 节点读成 **0 节点**（`results/gate_harness_fix__genuine_v1_id1.dat.out`、`results2/A1_oldbin_id1.out` 均 `WARN + 完成: 0 节点 + LOAD_RC=0`，而未改对照为 5 节点）。第二版补回 `p = buf` 并把「未来版本永不放行」写成显式分支，才是可交付态。第一版源码未留档、其内容/行号**由输出行号指纹推定**（报告已标注为引用口径），但**其二进制被留作反向验证的对照**（F2 组、A1、B3）。

> **改动后的行为对照**：`fmt_ver 2..8` 正常解析（不变）；`1` 从「静默 v1」变为「WARN 记账 v1」；`0/负数` 从「静默当 v1 猜」变为「ERROR 拒绝 / 显式开关放行」；`>8` 从「静默丢数据」变为「**ERROR 拒绝，开关无效**」。

### ② 特征维度 512→256 收进 main（13 文件）

报告 `dim512to256.md`；`git diff --stat` 记 **14 文件 `+83/−32`**——其中 13 个是降维本身，第 14 个 `src/multi_topology.c` 是**同树另一条线（第 ① 项版本闸门）的 48 行**，与降维无关。

- **真值源** `include/constants.h:37`：`PM_NODE_FEATURE_DIM 512` → **`256`**（附注释说明降维理由）。
- **收敛 4 处分散定义**（报告 ⑨）：
  1. `include/constants.h:37` —— 真值源；
  2. `include/common.h:22` —— `#define NODE_FEATURE_DIM PM_NODE_FEATURE_DIM`（**唯一别名**，本就未改）；
  3. `src/nn/feature_learn.c` —— **删除本地第三份 `#ifndef/#define` 兜底**，改 `#include "common.h"`；
  4. `tests/unit/test_semantic_growth.c:41` —— `512` 字面量 → `PM_NODE_FEATURE_DIM`。
- 另有 **2 个 Python 工具**收敛：`tools/convert_state.py`、`tools/clean_unicode_escapes.py`（各新增 `NODE_FEATURE_DIM = 256`，两处字面量改宏）。
- `include/visual_cortex.h`：`feature_dim` 默认值 `512` → `PM_NODE_FEATURE_DIM`（**值改动 / 配置默认**）；其余 `huarong_topology.h` / `semantic_growth.h` / `emergent_pos.h` / `bptt_learner.h` / `self_learner.c` / `visual_cortex.c` / `tests/README.md` 为注释或文档同步。
- **穷举确认**：对 `include/` + `src/` 全量 grep 字面量 `512` 后逐条分类，**没有残留「裸 512 维度字面量」参与特征向量读写**；其余 512 全为缓冲区 / 容量 / 批大小等**无关常量**（`PM_PATH_BUF`、`PFE_MAX_ANSWER_TEXT`、`CONFIG_PATH_MAX`、`causal_reasoning.c` 的解释串缓冲与优先队列容量、`web_fetch.c` 抓取上限、`pretrain.c` 的 `batch_size=512` 等），报告已列「不许动」清单。
- **受影响产物判定**：`pivotmind_state.dat`（头部含 `feat_dim`，不匹配即 `LOG_ERROR + return -1`）→ **必显式拒绝，安全**；`features.bin`（`d != NODE_FEATURE_DIM` 即 `return -1`）→ **硬拒但无 LOG，较安静**；`emergent_pos.bin` → ⚠ **静默错读风险**（见「已知未修问题」）；`cross_edges.bin` / `memory_seed.dat` 不含向量 → 能读；NN 权重自带 `shape[]` → 自描述（本轮未逐行验证加载端 shape 校验分支）。

### ③ 每 11 分钟必死的自死锁（`src/brainstem.c` `brainstem_tick_synapse_scale()`）

#### 病灶（审计 `deadlock-rwlock-audit.md` §0）

```
src/brainstem.c:394   if (bs->tick_count % 600 != 0) return;        ← 悬崖
src/brainstem.c:396   pthread_rwlock_rdlock(&bs->master->rwlock);   ← 读者 +1
src/brainstem.c:453   master_reevaluate_cross_links(bs->master, 10.0f);
src/multi_topology.c:576   pthread_rwlock_wrlock(&master->rwlock);  ← 同一线程、同一把锁：读→写「升级」
src/brainstem.c:455   pthread_rwlock_unlock(&bs->master->rwlock);   ← 永不执行
```

glibc 的 `pthread_rwlock_t` **不支持同线程读→写升级**：写锁要等「所有读者退出」，而唯一的读者就是本线程自己 → **永久自死锁**；`:455` 的 `rdunlock` 永不执行、读引用永久留驻、其余写者永久饿死。

- **确定性（为什么必然首次显形于 tick=600）**：`brainstem_tick_synapse_scale()` **第一行** `if (bs->tick_count % 600 != 0) return;`，而 `brainstem_loop` **先自增再调用**（`tick_count` 初值 0）⇒ `tick%600==0` **第一次成立就是 tick=600**。故**每次运行跑到 tick=600 必死**，与现场「58/58 次运行全部停在 tick=600、1007 次运行全局最大 tick=600」**逐字吻合**。tick ≈ 1.1 s ⇒ **约 11 分钟**。
- **引入点（代码确证）**：`master_reevaluate_cross_links(...)` 调用 2026-08-01（`127bb9d3` / v0.5.7「跨拓扑重评估」）即存在，**当时函数内没有读锁、不致死锁**；读锁 `pthread_rwlock_rdlock` 由 **commit `3d2f7cba`（2026-09-06，v0.5.24「流式批化存盘根治锁内饿死」）**加入 `brainstem.c:396`。→ 那次「修锁内饿死」的提交**顺手用一把读锁把内部一个会取写锁的调用包住了**，把「长时间持读锁的饿死」**升级成了「同线程自死锁」**：典型的**修复引入回归**（时间也吻合：09-06 12:03 提交，历史日志 09-06 12:13 那次就停在 tick=600）。
- **同类第 2 次复发**：仓库内 `src/self_learner.c:430-433` 早已记录同机制事故（v0.5.14 改成写锁贯穿把它修掉），本次是**同机制的第二次复发**——修复只覆盖了 `self_learner` 那条路径，`brainstem_tick_synapse_scale` 当时无读锁逃过，`3d2f7cba` 给它加上读锁后复活。
- **根因确证**：审计把全仓 **31 个 rdlock 获取点**逐个复核全部退出路径（`return`/`goto`/`break`/`continue`/OOM 分支），**恰好 1 处不配对 = `brainstem.c:396`**，调用链命中 `multi_topology.c:576`；修复后重扫 **0 命中**。全仓 `tryrdlock`/`timedrdlock` **0 处**，即无「获取失败需另判」的分支。
- ⚠ **工具抓不到这个 bug**（报告 §6.7）：这是纯**自死锁**（同一线程、同一把锁、违反 API 契约），**无数据竞争**；TSan/Helgrind 对 rwlock 是语义拦截、自升级在任何实现里都只**跟着一起挂住**、**不会报 report**。**不要把「TSan 0 report」当成本 bug 不存在的证据**；有效判据是「跑到 tick=600 的确定性黑盒」+ `/proc` 线程角色判读 + core 离线锁内存解码。

#### 修法

`git diff -- src/brainstem.c`（全文 39 行改动，**未删任何功能行**）：唯一实质动作是 **把 `pthread_rwlock_unlock(&bs->master->rwlock)` 提前到 `master_reevaluate_cross_links()` 之前**（即把该调用**移出读锁区间**）。另在读锁处补一条**纪律注释**（8 行，位于 `:404 rdlock` 之前），引用 `src/funcword.c:479` 原文：

```
 * ⚠️ 持读锁期间内部不得调用抢 master 写锁的函数（本函数内部安全）。
```

这条规则代码里早已明文存在（同段还有「不能在 master 写锁内调用本函数」），`brainstem.c:453` **直接违反了它**，只是此前没人做全仓校验。审计另指出 `src/article_reader.c:1046-1048` 的「单一出口 + `goto` 统一收尾」范式正是本函数缺少的纪律。

> ⚠ **诚实边界（已核实）**：`deadlock-branchstem-fix.md` 的 **§5.2 对照组 / §5.3 修复组 / §5.4 结束状态 / §6 结论 / §7 未覆盖** 已于本轮补完（报告 **30,243 B / 436 行、0 处「（待填）」**），A/B 长跑结论**据实回填**（来源见报告 **§5.3 / §5.4 / §6**）：**对照组** `final_tick = 600`、自 **20:09:20** 起**冻结约 11 分钟**；**修复组** `final_tick = 1110`、**越过 600 后又前进 510 tick**。**边界不放宽**：修复组只观察到 run 全程结束，**未做小时级长稳观察**，故**不能声称「修复后永不崩溃」**——结论严格限于「**越过了 600 这个已知必死点，且无死锁形态**」（报告 §6.3；详见「验证」节）。

## 把静默变成会喊（本轮主线）

> 作者要求单独写清这一节。本轮三条改动看似互不相关（一个改解析、一个改维度、一个改锁位置），但**它们是同一条主线**：**凡是「悄悄出错」，都要变成「会喊的错误」**。

### 这条主线的三种「静默」

| 静默形态 | 本轮实例 | 静默的代价 | 本轮怎么让它「会喊」 |
|---|---|---|---|
| **静默丢数据** | 旧 `master_load_state()` 对不认识的 `fmt_ver` 回退猜解析 | 5.17 MB / 3,860 节点的真状态文件被读成「1 节点」，**零 ERROR 零 WARN、`LOAD_RC=1` 判 ACCEPTED** ——上层完全无从察觉 | 未来版本 / 非正数 → **显式 `LOG_ERROR` + `return -1`**；v1 兼容路径 → **`LOG_WARNING` 记账**；旧的「无脑回退」整条删除 |
| **静默停摆** | `brainstem.c` 自死锁 | 引擎跑到 tick=600 后**再不前进**，但 HTTP 端口仍在监听、`/health` 仍返回 200——**看门狗探不到**，于是「冻结」被完整暴露并持续 **7.7 小时**（10:37:46 → 次日） | 找到根因、把调用**移出读锁区间**、在读锁处补**纪律注释**；并用「跑到 tick=600 的确定性黑盒 + `/proc` 线程判读」把它变成可复现、可对照的现象 |
| **静默错读** | 维度 512→256 后，`emergent_pos.bin` / `tools/merge_states.py` 等路径仍按旧值或错值解析 | 旧（512）文件被新（256）二进制**静默接受并错读**（每 anchor 少读 256 float → 后续字段全部错位）——与 `pivotmind_state.dat`（有 `feat_dim` 校验、**会拒绝**）形成**不一致的防御强度** | 维度收敛到**单一真值源**并穷举分类；对发现的两个静默点**如实登记为 Known issues**（`TAIL-ACCT-1` / `TAIL-TOOL-1`），不放过、也不谎称已修 |

### 与之配套的两条纪律

- **「丢弃必须记账」**：报告把旧行为定为违反**铁律**——静默丢弃是本次事故的共性。修法一律遵循「**要么显式拒绝并 `LOG_ERROR`，要么显式放行并 `LOG_WARNING`**」，绝不有「既不说、也不拒」的中间态。`TAIL-ACCT-1`（`src/feature_io.c:95` 是特征加载路径上**唯一**的静默丢弃点，无记账）正是这条铁律**尚未落实**的欠账，故单列 Known issue。
- **「显式开关只放行已知旧格式，永不放行未知新格式」**：`PIVOTMIND_ALLOW_LEGACY_STATE` 可以救「无版本头的 v1」，但**对“未来版本”永远无效**——因为救旧格式是「已知布局、信任来源」，放行未来版本是「未知布局、等于猜」。「开关命中 ≠ 校验通过」，语义只能是**信任 + 记账**。

### 教训：「秒级的测试抓不住分钟级的死」

- 这条自死锁**引入于 v0.5.24（`3d2f7cba`，2026-09-06）**，而 **v0.5.26 / v0.5.27 两轮**（含 round 2 全量审查 + round 3 并发修复 + x86_64/TSan 复验 + armbian 全量 23/23）**都没抓住它**。
- 原因不是测试不够多，而是**测试的时长尺度不对**：这条路径要**跑满 `tick%600==0`（约 11 分钟）才会第一次执行到**。所有**秒级**的单测、TSan/ASan、冒烟/回归套件都在 tick=600 之前就已结束——**它们跑不到那行代码，自然抓不到**。
- 更糟的是**这类 bug 对常规并发工具是隐形的**：它是纯**自死锁**（无数据竞争），TSan/Helgrind **不会报 report，只会跟着一起挂住**（审计 §6.7）。所以「TSan 0 条」「23/23 通过」**在这条 bug 面前毫无牙力**。
- **由此得到的教训**：**秒级的测试抓不住分钟级的死**。仅靠「更快的测试」「更多的单测」覆盖不到「只在低频周期分支上第一次执行」的路径。
- **后续建议（待作者拍板，本版未实施）**：加一条**长跑监护用例**——让引擎/沙箱实际跑到 **>600 tick（建议 ≥900 tick，约 ≥15 分钟）**，断言 `tick` **越过 600 且持续增长**（而不是只看退出码）；同时可配一个**锁纪律探针**（把「会取 master 写锁的函数」放进「已持 master 读锁」的环境里跑，超时即判 RED，见审计 §6.5），把这类升级死锁**压到秒级可回归**。此建议同样适用于「只在 `tick%N==0` 才执行的代码」这一类路径。

> ⚠ **诚实边界（已核实）**：上述「长跑对照（对照组 vs 修复组）」的**实测结论已补完并据实回填**（`deadlock-branchstem-fix.md` §5.3/§5.4/§6）——对照组 `final_tick=600`、冻结约 11 分钟，修复组 `final_tick=1110`、越过 600 后又跑 510 tick，唯一变量 = `src/brainstem.c`。本节**机制与确定性**结论（有代码/日志确证）**不因此放宽**；修复组**未做小时级长稳观察**，**不得外推为「永不崩溃」**，结论限于「越过 600 这个已知必死点、无死锁形态」（报告 §6.3），具体数字见下节。

## 改动文件

工作区 `git status --short` 共 **15 个文件脏**、`git diff --stat` 合计 **`+99 / −34`**。按三条线归属如下（`src/multi_topology.c` 归第 ① 项）：

| 文件 | 线 | 变更 |
|------|----|------|
| `src/multi_topology.c` | ① | 版本闸门：`master_load_state()` 三分支拒绝/记账 + `PIVOTMIND_ALLOW_LEGACY_STATE`（48 行） |
| `include/constants.h` | ② | 真值源 `PM_NODE_FEATURE_DIM 512 → 256` + 注释 |
| `include/common.h` | ② | 唯一别名 `NODE_FEATURE_DIM = PM_NODE_FEATURE_DIM`（未改，仅记录） |
| `src/nn/feature_learn.c` | ② | 删本地 `#ifndef/#define` 兜底，改 `#include "common.h"` |
| `tests/unit/test_semantic_growth.c` | ② | `512` 字面量 → `PM_NODE_FEATURE_DIM` |
| `include/visual_cortex.h` | ② | `feature_dim` 默认值 `512` → `PM_NODE_FEATURE_DIM` + 注释 |
| `include/huarong_topology.h` | ② | 注释 512→256 |
| `include/semantic_growth.h` | ② | 注释 512→256 |
| `include/emergent_pos.h` | ② | 注释 512→256（×4） |
| `include/bptt_learner.h` | ② | 注释 512→256 |
| `src/self_learner.c` | ② | 注释 512→256 |
| `src/visual_cortex.c` | ② | 注释 512→256 |
| `tools/convert_state.py` | ② | 新增 `NODE_FEATURE_DIM = 256`，字面量改宏 |
| `tools/clean_unicode_escapes.py` | ② | 新增 `NODE_FEATURE_DIM = 256`，字面量改宏 |
| `tests/README.md` | ② | 文档 512→256 |
| `src/brainstem.c` | ③ | 自死锁修复：`unlock` 提前到 `master_reevaluate_cross_links()` 之前 + 读锁纪律注释 |

> **未做的文件**：`include/pivotmind_version.h` 仍是 **`"0.5.27"`** —— **版本号尚未 bump 到 0.5.28**（本文件按任务约定只写文档、不动代码）。作者应在本轮落定后自行 bump。

## 验证

> 口径：以下均为**报告/原始输出里能读到的数字**，逐条注明来源；**读不到或仍在补写的一律标「待核」**。

### ① 状态加载版本闸门（`state-fmt-gate-fix.md`）

- **对照实验规模**：armbian 上以 harness 对照实验 + 反向验证（falsification）共覆盖 **60 份 `.out`**（第一轮 `results/` 23 份 + 本轮 `results2/` 37 份，**全部逐份读取**）。
- **核心反证**（未改副本 → 病根；修复版 → 有牙）：
  - 未改二进制读真 `fmt_ver=9`：**5.17 MB → 「完成: 1 节点」、`LOAD_RC=1`、ACCEPTED、零 WARN 零 ERROR**（`F1_ctrl__real_v9.dat.out`）。
  - 修复版读同一文件：**`[ERROR] :4876` fmt_ver=9 > 8，`rc=-1`，`EXIT=1`**（`C1_real_v9_noswitch.out`）；**带开关同样 `rc=-1`**（`C2_real_v9_switch.out`，关键负向）。
  - 非法首字段：`0` / `-1` / `9999` / `2147483647` 均 `[ERROR]` + `rc=-1`；未改对照一律 **1 节点 ACCEPTED**（反证「改前无牙」）。
- **开关语义**：`未设` / `0` / `yes` → 拒绝；`1` → WARN + 解析（B1–B10 逐条）。
- **不误伤合法旧版本（D 组）**：`v2` / `v5` / `v8` 均正常读出 3 节点 `rc=3`；开关对合法版本无影响。
- **单测**：`test_memory_unit` **6/6**、`test_topology_unit` **3/3**、`test_diffusion_unit` **3/3**，`EXIT=0`（两轮结论逐字一致）。
- **构建告警**：全量编译 `ctl_build.log` `MAKE_EXIT=0`、`grep -ci warning = 0`；`build_and_warn.txt`：修复树 / 未改树同口径**均 0 warning 0 error**，`diff` 无差异。（⚠ `gatefix_build.log` 只是 **0.93 s 增量构建**（产物 up to date），**不能**当「改后干净全量重编」的证据——报告已如实标注。）
- **v1 误伤复扫（只读）**：本机 38 候选 + armbian 30 候选 + 仓库工作树 / `tests/` / git 全历史扫描——**不存在任何会被本次改动误伤的真实 v1 状态文件**；命中 v1 触发条件的**全是实验自造人造夹具**。
- ⚠ **未做**：**未跑全套测试**（仅 3 支单测，共 12 例）；**x86_64 + ASan/UBSan/TSan 那条腿（G15/WSL）本轮完全没跑**；**未做端到端 v1 迁移演练**；**未部署**（报告 §7）。

### ② 特征维度 512→256（`dim512to256.md`）

- **构建**：`pm-dim256` 副本全量编译 `EXIT=0`；`/tmp/build256.log` 严检 `grep -nE "error:"` **无输出**、`grep -cE "warning:"` **0**（日志里 2 行含 `error` 的是文件名 `error.o` / `error.d`，非诊断消息——报告已说明）。
- **armbian 逐测试退出码**（`cx@100.107.169.41:~/pm-dim/`，每支 `timeout 60`）：

  | 测试 | 退出码 | 结果尾部 |
  |------|--------|----------|
  | `test_tensor` | **0** | Total 14 / Passed 14 / Failed 0 |
  | `test_topology_unit` | **0** | 3 run, 3 passed, 0 failed |
  | `test_memory_unit` | **0** | 6 run, 6 passed, 0 failed |
  | `test_diffusion_unit` | **0** | 3 run, 3 passed, 0 failed |
  | `test_web_fetch` | **0** | 46/46 通过（9 跳过 = 需 `WEBB_FETCH_TEST_LIVE=1`，设计跳过非失败） |

  **五支全绿，退出码全 0，无红测试。**
- **维度自检 + 反证**（同一 `dim_selfcheck.c` 用两棵树 include 各编一份、在 armbian 运行）：
  - `pm-dim256`：`PM_NODE_FEATURE_DIM = 256`、`sizeof(PMProbeNode) = 1024 bytes`、`PARSE_DIM=256`、`EXIT=0`；
  - `pivotmind-baseline`（仍 512，反证）：`512`、`2048 bytes`、`PARSE_DIM=512`、`EXIT=0`。
  - ⇒ 证明自检确实在测**编译进的维度宏**，不是恒定输出。
- **定义点收敛复核**：全仓 `#define (PM_)?NODE_FEATURE_DIM` 共 8 个定义点，C 侧 4 处已收敛为「单一真值源 + 单一别名」；Python 2 处已收敛；剩 2 处（`tools/merge_states.py`）**值错误**，见 Known issues。
- ⚠ **未做**：报告 §8 明载——仅跑 5 支，**`make test` 全集（含直接受维度影响的 `test_semantic_growth` / `test_visual_cortex` / `test-pfe-unit` 等）未构建未运行**；NN 权重 `shape` 校验路径未逐行验证；**未做 512→256 的真实文件迁移演练**；**无运行期语义质量证据**（降维是否真的「区分度不降」无评测）。

### ③ 自死锁修复（`deadlock-rwlock-audit.md` + `freeze-forensics.md`）

- **机制 / 确定性（代码 + 日志确证）**：
  - `freeze-forensics.md`：`/tmp/pv62.log` **879,368 行 / 1007 次运行**中，有堆监控数据的 **58 次**，**每次最后一个 `[堆监控] tick` 都是 600，没有一次越过 600**；**全部 1007 次运行的最大 tick = 600**（硬悬崖）。
  - 现场（2026-09-11 10:37:46 冻结）：**三个线程**（感觉皮层 worker `dialog_generate.c:675` / 脑干主循环 `brainstem.c:396` / 自学习调度 `self_learner.c:434`）**同卡同一把全局 `master->rwlock`**、均 `wchan=futex_wait_queue`（另有学习队列 worker 在 `pthread_cond_wait`，正常空闲）；锁内存 `__cur_writer=0 / __writers=0 / __writers_futex=3 / __readers=0x0a`。
  - 审计 §0.3 **纠正**原假设「`0x0a` = 10 个读者」：按 glibc 位编码（`READER_SHIFT=3`）`0x0a` 实为 **`WRLOCKED=1` + 读者数 = 1** ⇒ **1 个读者（就是自死锁者本人）+ 它自己当等待中的主写者**，教科书式自升级死锁。
  - 审计 §2.2：全仓 **31 个 rdlock 获取点**逐站复核，**恰好 1 处不配对 = `brainstem.c:396`**；修复后重扫 **0 命中**。
- **A/B 二进制构造**（`deadlock-branchstem-fix.md` §1）：两棵树同源复制、唯一变量 = `src/brainstem.c`；源码 md5 `5f988ada…`（对照，HEAD 原样）vs `d14c9ee3…`（修复）；二进制 md5 `53f4a0de…`（control）vs `ca46d176…`（fix），**均不同 ⇒ A/B 有效**。两组全量重编译 `exit=0`、**0 warning / 0 error**（各 70 条 gcc 行）。
- ✅ **长跑对照（A/B）实测结论 —— 已核实**（来源：`deadlock-branchstem-fix.md` **§5.3 / §5.4 / §6**；该报告已于本轮补完，30,243 B / 436 行、**0 处「（待填）」**）：
  - **对照组（CONTROL · 端口 8099 · 二进制 md5 `53f4a0def17943d328e9363e7bd6494e`）**：`final_tick = 600`，tick 序列 `450 → 510 → 540 → 600 → 600 → 600`，**自 20:09:20 起冻结约 11 分钟**；冻结瞬间 3 个 worker 同时转入 futex，下一采样点起 **4 线程成排 `futex_wait_queue` + 1 个 `inet_csk_accept`**；`engine.log` mtime 冻在 **20:09:18**；`end_iso = 20:20:22`（由 runner 脚本 `kill -9` 结束，**非崩溃**）。
  - **修复组（FIX · 端口 8098 · 二进制 md5 `ca46d1767db61912ccbe2016bd50860c`）**：`final_tick = 1110`，tick 序列 `450 → 510 → 540 → 600 → 630 → 690 → 720 → 780 → 810 → 870 → 900 → 960 → 990 → 1050`（末采样 20:19:23），**越过 600 后又前进 510 tick**；`wchan` **从未出现成排 futex**；`engine.log` 到 **20:20:22 仍在写**；`end_iso = 20:20:24`。
  - **唯一变量归因成立**：两棵树仅 `src/brainstem.c` 不同（源码 md5 **`5f988ada…`（对照）vs `d14c9ee3…`（修复）**），**其余改动完全相同**、**同一份起始数据**（`c7a21e48…`）、**同机同时段** ⇒ 对照冻死 / 修复存活。
  - ⚠ **边界（报告 §6.3，不夸大）**：**不能声称「修复后永不崩溃」**——只观察到越过 600 后又跑 510 tick，**未做小时级长稳观察**；结论严格限于「**越过了 600 这个已知必死点，且无死锁形态**」。`[突触缩放]` 两组 count 均为 0，**但不是判据**（`brainstem.c:456` 仅在 `decayed>0 || released>0` 才打印）；**判据是 tick 是否越过 600**。
- ⚠ **未做**：修复版**未部署**（`STATUS.md` §8：unit 现为 `failed + disabled`、无进程；且部署前必须先解决 `fmt_ver=9` 状态文件与写入端版本的匹配问题，见 Known issues）。

## 红线声明

按作者架构红线，本轮三条改动**均未触碰**以下任何逻辑：**限边（每节点出边上限）、截断（候选/结果截断）、周期性稀疏化、跨拓扑上限、队列满时丢弃任务的策略**。

并且：

- **版本闸门**只改「**拒绝 / 记账**」，**不改变任何合法文件的解析结果**——`fmt_ver 2..8` 行为不变（D 组 `v2/v5/v8` 逐条实证 3 节点 `rc=3`）；唯一被拒的是**此前会被静默猜解析的未知版本/非正数**。
- **降维 512→256** 只收敛真值源与分散定义，**不引入新语义、不改算法**；剩余 512 常量均为缓冲区/容量，**未动**。
- **死锁修复**「功能行一字未改」，唯一实质动作是把 `unlock` 提前——**不改扩散语义**（本批未重跑 N17 账行不变式，因 v0.5.27 的 N17 逐字节不变结论基于 round 3 树，本轮 `brainstem.c` 改动需另做 N17 对照，见 Known issues）。
- 本 changelog 本身**只新建 `changelogs/072-*.md` 并在根 `CHANGELOG.md` 顶部加一条摘要**，**不改任何 `src/` / `include/` / `tests/` / `tools/`，不改 `changelogs/` 下已有文件，不做任何 git 写操作**。

## 已知未修问题

> 本项目惯例（见 070 / 071）。以下均为**如实登记、本版未修**的欠账。

### 本轮明确登记的三个尾巴（`pending-tails.md`）

- **`TAIL-LOCK-1`** —— `src/hippocampus.c:63 / :94` **反向不配对（多解锁）**：`rdlock` 在 `if (vocab && vocab->net) {` **内**（`:62→:63`），而 `unlock` 在那个内层 `if` **之外**（`:94`）。因此当 `hc->topology && hc->log_count>0` 成立而 `vocab==NULL` 或 `vocab->net==NULL` 时，会对**一把本线程从未锁过的读锁调用 `unlock`**：若 `__readers` 低位读者数 >0 会误扣**别人的**读引用，若读者数=0 则 `__cur_writer != 自 tid` 返回 `EPERM`。**方向与本轮自死锁相反（它是减、不是加），不是停摆根因**，但属真缺陷，会污染计数、可能误伤他人读锁。修法：单一出口 + `goto` 保证加解锁配对（范式见 `article_reader.c:1045-1049` 注释）。
- **`TAIL-ACCT-1`** —— `src/feature_io.c:95` 是**特征加载路径上唯一的「静默丢弃」点**（无 `LOG_ERROR` / 记账），违反铁律「**丢弃必须记账**」。修法极小：补一条 `LOG_ERROR`（含文件名、期望/实际维度）。降维改动**没有**动它（不属本次范围）。
- **`TAIL-TOOL-1`** —— `tools/merge_states.py` 的 `NODE_FEATURE_DIM = 24`：**既非 512 也非 256**，且 `:25` 注释谎称「与 C 代码 `PM_NODE_FEATURE_DIM` 一致」。经复核它是**真维度且会静默错读**——`:88/:89/:91` 按 24 读、`:205/:206/:209` 按 24 写，而 `:86` 从文件头读到的 `feat_dim` 在 `:88` **被完全忽略**（本应 `f.read(feat_dim*4)`）。可达路径 `tools/run_parallel_train.sh:156`（需人工触发的多机并行训练合并流程，**不是 Makefile/CI 目标**）。**拿真状态文件跑会按 24 解析 → 静默错读**。该 bug 自远端仓库初始提交 `887e2c8` 起即存在，与 512→256 降维**正交**（降维既没引入它、也没修好它）。修法：改成「以文件头读到的 `feat_dim` 为准」，兜底默认值引用 256 并**显式记账**。

### 降维复核中一并发现（本版未改）

- **`emergent_pos.bin` 的静默错读风险（本轮新发现，静态分析结论）**：其文件头**只校验 magic + version，不含维度字段**，降维**不会**改变 version 号，故旧（512）文件会被新（256）二进制**静默接受并错读**（每 anchor 少读 256 float → 后续字段全部错位、解析崩坏）——与 `pivotmind_state.dat`（有 `feat_dim` 校验、**会拒绝**）形成**不一致的防御强度**。**这是本轮降维引入的、最可能造成静默数据错误的路径。**建议（属产品改动，仅提出未实施）：给头加 `feat_dim` 字段并按值校验，或把 version 提到 2 使旧文件被显式拒收。⚠ 该结论**未构造 512 版文件做端到端复现**（受"本机不许运行产物"约束）。
- **`features.bin` 的拒绝较安静**：`load_features` 在 `d != NODE_FEATURE_DIM` 时 `return -1` 但**无 `LOG`**（硬拒、安全但无记账）。

### 验证覆盖面（本版未达成的）

- **版本闸门 + 降维两线均未取得跨架构/跨 libc 交付依据**：闸门线**未跑 `make test` 全量**、**G15/WSL 的 x86_64 + ASan/UBSan/TSan 那条腿完全没跑**；降维线仅跑 5 支测试（`test_semantic_growth` / `test_visual_cortex` / `test-pfe-unit` 等**直接受维度影响的用例未构建未运行**），且**无运行期语义质量证据**（降维是否真的「区分度不降」无任何评测）。技能明载「**armbian 绿了不能单独作交付依据**」。
- **自死锁修复的长跑 A/B 对照结论已核实并据实回填**（`deadlock-branchstem-fix.md` 已补完：30,243 B / 436 行、0 处「（待填）」；数字见本文件「验证」节 ③ 与报告 §5.3/§5.4/§6）：对照组 `final_tick=600`（冻结约 11 分钟）、修复组 `final_tick=1110`（越过 600 后又跑 510 tick），唯一变量 = `src/brainstem.c`。⚠ 该「已核实」**不放宽报告 §6.3 的边界**：**未做小时级长稳观察**，**不得声称「永不崩溃」**。
- **修复版未部署、未提交**：三条改动均在工作区；`include/pivotmind_version.h` 仍为 `"0.5.27"`，**版本号未 bump**。

### 部署前置（新发现，别踩）

- 真实 `fmt_ver=9` 载荷共 **4 份副本**（含 armbian `~/pivotmind/pivotmind_state.dat`，5,176,666 B），**新闸门会「有意拒绝」它们**——现在把修复二进制直接部署上去，表现是 **「启动即拒绝状态加载、空壳运行」**（`rc=-1`）。这是**设计意图**（写入端与读取端版本必须匹配），但部署顺序必须先解决：**要么把写入端同步到 v9，要么等批 1 的 v10 读端（读 2..10）一起上**，**不能靠开关绕过**（开关对未来版本无效）。⚠「该文件属于 gateway」一点属**引用技能文档/上级口径**，报告作者亲自核实到的仅是「它存在于 armbian 且首字段=9」。

### 其他未清尾巴（承接，未在本版处理）

- `TAIL-GATE-6`~`TAIL-GATE-11`（`make test` 吞失败证据、堆投毒自我抵消、泄漏门禁不区分库/测试、scratch 文件编码损坏、`digital_life.c` 引不存在头文件等）——**门禁与测试债**，见 `pending-tails.md`。
- `TAIL-RED-1`~`TAIL-RED-5`（红线普查缺批次表、`diffusion.c:783` 度数截断待核、生成端「步间状态层」未实施等）。
- **锁开销正式基准仍未测**（`TAIL-PERF-1`：只有 `%CPU 1.9` 的 sanity，无耗时数据）。
- **N17 账行不变式未在本轮重跑**：v0.5.27 的「扩散账行逐字节不变」结论基于 round 3 树，本轮 `brainstem.c` 改动后**应另做一次 N17 对照**（属建议，未实施）。
- **本文件即 `TAIL-DOC-1` 的落地**：v0.5.28 changelog（`changelogs/072-*.md` + 根 `CHANGELOG.md` 顶部摘要）已写；`pending-tails.md` 里 `TAIL-DOC-1` 提到的「批 1 的 v10 格式升级」**另起一篇，不在本文件范围内**。
