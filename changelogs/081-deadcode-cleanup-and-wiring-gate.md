# 0.5.37 — 死代码清理（6 删 2 接线）+ `check-wiring` 门禁接入 `test:`/CI

> **日期**: 2026-09-13 | **类型**: 清理 / 修复 / 新增

## 概述

本版只做一件事族；判据是老大定的一句话：

> 一个从没被调用过的配置注入口，等于在宣称一个不存在的能力。**能接线就接线，接不了就删，绝不留着。**

据此把上一轮诊断扫出的 8 个「有定义/声明、零调用点」函数逐条定性：**6 个删（含连带死字段与整个死头文件）+ 2 个接线**（其中
`PIVOTMIND_SEED` 是**修 bug 不是加功能** —— 它早在 CHANGELOG 里当成品交付，实际是空开关）。清完之后 `make check-wiring`
首次全绿，于是把这道门禁从「建了不接」正式接入 `test:` 与两个 CI job。

本版**不含**任何架构改动；`feat/lang-zhchar`（B 类语种口径）刻意不并入（其 A/B 结论为「不合入」，见
`D:\111\pm_wip\玄枢-死代码判定与B类AB结论-20260913.md`）。

## 一、清理：删除 6 个零调用配置注入口

| # | 函数 | 位置 | 定性 | 连带处理 |
|---|------|------|------|----------|
| 1 | `consolidation_set_default_config` | `src/memory_consolidation.c:71` + `include/memory_consolidation.h:125` | 零调用 | `g_default_config` 仍被 `consolidation_get_default_config()`（`:569`/`:618` 两处消费）使用 ⇒ **全局保留**，未产生死静态 |
| 2 | `ewc_set_default_config` | `src/catastrophic_forgetting.c:82` + `include/catastrophic_forgetting.h:197` | 零调用 | `g_default_ewc_config` 仍被 `ewc_get_default_config()`（`:354`/`:405` 消费）使用 ⇒ **全局保留** |
| 3 | `topology_growth_set_default_config` | `src/topology_growth.c:130` + `include/topology_growth.h:125` | 零调用 | 同名 `g_default_config` 仍被 `topology_growth_get_default_config()`（7 处消费）使用 ⇒ **全局保留** |
| 4 | `topobrain_set_config` | `src/topology_brain.c:124` + `include/topology_brain.h:80` | 零调用 | **同时删死字段** `TopoBrainConfig.scan_interval`（全仓仅此一处出现）及其在 `TOPOBRAIN_DEFAULT_CONFIG` 宏里的占位 `600` |
| 5 | `pretrain_state_create_with_config` | `src/nn/pretrain.c:162` + `include/nn/pretrain.h:304` | 零调用 | 只删此一个函数；pretrain 其余 API 一律不动（超范围） |
| 6 | `trainer_create_with_config` | `include/text_trainer.h:93` | **仅声明、无定义** | 先核 `grep -rn text_trainer.h` 全仓只命中该文件自身 ⇒ **整个头文件删除**（156 行） |

**连带死静态的核查结论**：任务书要求「若因此产生死静态变量一并清掉」。逐条核完，三个 `*_get_default_config` 仍在消费各自
的 `g_default_*` 全局变量，因此**没有一个全局被孤立** —— 不删任何全局，本版零新增死静态。

## 二、接线：`PIVOTMIND_SEED` 真正通电（修 bug）

**症状**：`PIVOTMIND_SEED` 在 `CHANGELOG.md`（v0.5.26 / P2-2）里已作为「随机种子可复现」交付，但
`init_random_from_env()` / `init_random_seed()` 全仓**零调用点** —— 一个对外宣称过的能力，实际不存在。
更糟的是 `init_random()` 首次运行会 `srand(time^pid)`，即便调用了置种入口也会被覆盖（`common.h` 原注释自承此坑）。

**修法（两处接线 + 一个 guard）**：

1. `init_random_from_env()` 接到 `demos/pivotmind_gateway.c` `main()` 的**第一句**（约 `:415`）——在任何 `init_random()`/`srand()` 之前。
2. 给 `init_random()` 加 guard：**种子已被显式设置时，不得再用 time/pid 覆盖**。
3. `init_random_seed()` / `init_random_from_env()` 均置位该显式种子标志。

**为什么标志必须是跨 TU 共享的 `extern`，而不是 `common.h` 里的 `static`**：`common.h` 的 inline 函数在每个翻译单元各有
一份 `static`，而 `srand()` 的种子是 **libc 的进程级全局状态**。标志若按 TU 各存一份，A 里置了位、B 里的 `init_random()`
看不见，仍会覆盖 —— 那就还是「装了开关没通电」。因此新增 `src/random_seed.c` 存放**全进程唯一**的
`bool pm_random_seed_explicit`（`Makefile` 的 `$(wildcard src/*.c)` 自动纳入，无需改构建）。

**行为等价性**：未设置 `PIVOTMIND_SEED`、也没调用过置种入口时，`pm_random_seed_explicit` 保持 `false`，
`init_random()` 的执行路径与 v0.5.36 **逐字节一致**（仍走 `srand(time(NULL) ^ (size_t)&initialized)`）。

## 三、门禁收口：`check-wiring` 从「建了不接」到接入 `test:`/CI

| 动作 | 内容 |
|------|------|
| 白名单 | `tools/wiring_whitelist.txt`：死函数已清 ⇒ **不再登记任何豁免条目**（0 条生效），并把「首次必红」的历史说明更新为「已全绿接入」 |
| `test:` | 在 `check-locks` / `check-version` 之后**新增一段** `make check-wiring`（PASS 计入通过数） |
| CI | `.github/workflows/ci.yml` 的 `build-x86_64` 与 `build-arm` 两个 job **各加一条** `make check-wiring`（紧挨 `make check-tools`） |

**正反例沙箱证据**（证明这道门不是摆设）：

- **反例（拒绝档）**：在副本里新增一个假死函数 `init_fake_dead_probe`（有定义、零调用）⇒ `check-wiring` 报
  `✗ init_fake_dead_probe`、**退出码 1**。
- **放行档**：把该函数登记进白名单 ⇒ 归入「① 白名单豁免」、**退出码 0**。
- **还原**：删除探针与白名单条目 ⇒ 回到 **0 命中 / 退出码 0**。

**已知盲区（本次不扩命名族，仅登记待办）**：① 只扫 `init_*` / `ensure_*` / `*_from_env` / `*_config` 命名的函数族；
② 函数指针 / 回调 / 宏展开出的调用点，静态扫描看不见。（两条都已写进 `Makefile` 的注释与白名单说明。）

## 四、新增单测（第 31 支）

`tests/unit/test_random_seed_unit.c`（**6 组 / 6 通过**）：

| 组 | 断言 |
|----|------|
| T0 | 进程启动时 `pm_random_seed_explicit == false`（未设种 ⇒ 保持旧的 time/pid 播种） |
| T1 | **★守护**：`init_random_seed()` 之后，**本进程首次** `init_random()` 不得覆盖种子（比对 seed 后 3 个 `rand()` 与重放） |
| T2 | 同一 seed 两次初始化 ⇒ `rand()` 序列逐值一致；不同 seed ⇒ 序列不同 |
| T3 | `PIVOTMIND_SEED` 已设置 ⇒ 生效，且其后 `init_random()` 不覆盖 |
| T4 | `PIVOTMIND_SEED` 未设置 ⇒ `init_random_from_env()` 不改变种子 |
| T5 | `PIVOTMIND_SEED` 为空串 ⇒ 同上 |

⚠️ 用例顺序**不可调换**：`init_random()` 内部是一次性 `static`，守护测试 T1 必须是进程内第一次调用它，否则拿不到能变红的证据
（已写在文件头注释里）。

`Makefile` **7 处接入**：`ASAN_TEST_TARGETS` / `ASAN_TEST_BINS`（**两张表**）/ 编译规则 / 别名 `test-random-seed-unit` /
`TEST_BINS` / `test:` 前置依赖 / `.PHONY`。

**反例沙箱证据**（证明守护测试不是空跑）：在副本里把 `init_random()` 的 guard 抽掉重编 ⇒ **T1 FAILED**（
`seed 后 3 值 (611332365,883560269,1172995490) vs 重放 (611332365,433891604,1411323250)`），整测 **RC=1**。

## 改动文件

| 文件 | 变更 |
|------|------|
| `include/common.h` | 加 `extern bool pm_random_seed_explicit`；`init_random()` 加 guard；两个置种入口置位 |
| `src/random_seed.c` | **新增**：标志的唯一实例（20 行，含「为什么必须跨 TU」的说明） |
| `demos/pivotmind_gateway.c` | `main()` 顶部接 `init_random_from_env()` |
| `src/memory_consolidation.c` / `include/memory_consolidation.h` | 删 `consolidation_set_default_config` |
| `src/catastrophic_forgetting.c` / `include/catastrophic_forgetting.h` | 删 `ewc_set_default_config` |
| `src/topology_growth.c` / `include/topology_growth.h` | 删 `topology_growth_set_default_config` |
| `src/topology_brain.c` / `include/topology_brain.h` | 删 `topobrain_set_config` + 死字段 `scan_interval` + 宏占位 |
| `src/nn/pretrain.c` / `include/nn/pretrain.h` | 删 `pretrain_state_create_with_config` |
| `include/text_trainer.h` | **整个删除**（156 行；只有声明无定义） |
| `tests/unit/test_random_seed_unit.c` | **新增**：第 31 支契约单测（169 行，6 组） |
| `Makefile` | 新单测 7 处接入；`check-wiring` 挂进 `test:`；门禁注释更新 |
| `.github/workflows/ci.yml` | 两个 job 各加 `make check-wiring` |
| `tools/wiring_whitelist.txt` | 更新说明；无生效豁免条目 |
| `include/pivotmind_version.h` | 真值源 `0.5.36` → `0.5.37` |
| `README.md` / `README.zh-CN.md` / `ARCHITECTURE.md` | 7 处版本锚点由 `make sync-version` 改写 |
| `CHANGELOG.md` / `changelogs/081-*.md` / `changelogs/README.md` | 本版记录 |

## 编译验证

- **WSL（x86_64，gcc 15.2.0，20 核）**：`make clean && make -j8 all` **0 error / 0 warning**；`make check-tools`
  **19/19**；`make check-wiring` **0 命中 / RC=0**；`make test` **32 通过 / 0 失败**（29 支二进制 + `check-locks` +
  `check-version` + `check-wiring`）；`make asan-test` **15/15 PASS**（含新单测）。
- **armbian-1（aarch64，gcc 13.3.0，6 核）**：见本文件同目录的发布记录；全程 `nice -n 19`、`-j2`，线上实例
  `pid 2215289` 与线上数据**未触碰**。
- 新单测直跑：**6 运行 / 6 通过 / 0 失败**，两机一致。

## 诚实边界

1. **本版没修任何「功能」，只删死代码 + 给一个宣称过的开关通电。** `PIVOTMIND_SEED` 之前是空开关，现在真的生效；
   这不等于「玄枢的随机性/可复现性被解决了」—— 它只是让一个已有入口名副其实。
2. **`check-wiring` 覆盖面有限**：只扫上述 4 个命名族，且看不见函数指针 / 回调 / 宏展开的调用点。它抓的是
   「机制在、没接线」这一类**高发**缺陷，不是全部。本次**刻意不扩命名族**（避免误报淹没真信号）。
3. **`make asan-test` 下存在 14 条预存 `-Wformat-truncation` 警告**，全部落在本版**未改动**的文件
   （`autonomic_learner.c` / `multi_topology.c` / `visual_cortex.c` / `tests/unit/test_paths_unit.c`），仅在 ASan 旗标
   （`-O1`）下出现；默认出货构建（`-O2`）**0 warning**。本版未处理它们（不在范围内），如实记录。
4. **两笔提交的中间态**：第一笔（代码）提交后、第二笔（文档，含活文档版本锚点）提交前，`make check-version` 会**红**
   —— 因为真值源已升到 v0.5.37 而活文档锚点还没落。**最终 tip（含文档提交）转绿**；这是「代码 / 文档」两笔提交设计的
   必然中间态，不是缺陷，也不是「先红后绿蒙混」。
5. **本版未上线部署**（遵铁律）。armbian 的 `pm-v0537-arm` 仅做编译验证，未替换线上二进制、未动线上数据。
6. **本版未并入 `feat/lang-zhchar`**（B 类语种口径），其 A/B 结论为「不合入」，保留为实验记录。
