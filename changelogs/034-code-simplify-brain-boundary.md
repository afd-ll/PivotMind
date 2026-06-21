# 0.4.0 — 代码简化 + 脑区边界完善 + 下丘脑新脑区

> **日期**: 2026-06-20 | **类型**: 重构

---

## 概述

v0.4.0 是一次深度架构审查与重构版本。基于对全部 82 个源文件的审查，清理了约 200 行重复代码，修复了 11 处脑区边界越界，升级了布罗卡区为有状态脑区，新增了下丘脑（Hypothalamus）动机调控系统，并修复了联网搜索因缺少 OpenSSL 支持而全部失败的问题。

---

## 改动文件

| 文件 | 变更 |
|------|------|
| `include/cingulate.h` | 新增 `cingulate_diffusion_evaluate()` 公共函数声明；include `diffusion.h` |
| `src/cingulate.c` | 新增 `cingulate_diffusion_evaluate()` 实现；3 处手动查找循环替换为 `master_get_sub_topology_by_type()` |
| `src/prefrontal.c` | 使用 `cingulate_diffusion_evaluate()` 替代内联 diffusion→cingulate 管线 |
| `src/prefrontal_executive.c` | 使用公共函数替代内联管；提取 `pfe_post_process()` 消除 `pfe_reason`/`pfe_resume_reason` 约 100 行重复 |
| `src/hippocampus.c` | 2 处查找循环替换；增加 `THAL_SIG_CONSOLIDATE_NODE` 丘脑信号发送 |
| `src/amygdala.c` | 1 处查找循环替换 |
| `src/perception.c` | 5 处查找循环替换 |
| `src/brainstem.c` | 提取 `brainstem_circadian_phase_name()` 统一 3 处硬编码；Broca 委托 tick；Hypothalamus 调度 |
| `include/broca.h` | `Broca` 结构体：从纯函数库升级为有状态脑区 |
| `src/broca.c` | `broca_create/destroy/tick` 生命周期：自主管理模板构建与衰减调度 |
| `include/hypothalamus.h` | **新增**：下丘脑 — 需求/动机调控系统 |
| `src/hypothalamus.c` | **新增**：drive 动态 + 昼夜耦合 + 对话事件调制 |
| `include/thalamus.h` | `THAL_HYPOTHALAMUS = 8`，`THAL_SUBSYSTEM_COUNT` 8→9 |
| `src/thalamus.c` | `SYS_NAMES` 补充 PFE + Hypothalamus 条目 |
| `demos/pivotmind_gateway.c` | 创建/注册 Broca + Hypothalamus 实例；移除冗余 Broca 直接调用；shutdown 清理 |
| `include/pivotmind_version.h` | 0.3.0 → 0.4.0 |
| `README.md` | 全面重写：中英双语，9 脑区架构，PFE 推理模式表，版本历程 |
| `Makefile` | 添加 `-DHAS_OPENSSL -lssl -lcrypto`（修复联网搜索全部失败） |
| `.github/workflows/ci.yml` | x86_64 加 `libssl-dev`；ARM 加 `-DHAS_OPENSSL -lssl -lcrypto` |

**新增文件**: `include/hypothalamus.h`, `src/hypothalamus.c`  
**净效果**: 消除 ~200 行重复代码；11 处手动循环 → 1 个公共函数调用；新增 1 个脑区；1 个脑区升级；修复联网搜索

---

## 详细变更

### P0 — 消除核心重复代码

#### 1. 提取 `cingulate_diffusion_evaluate()` 公共函数

`prefrontal.c` 和 `prefrontal_executive.c` 中有完全相同的扩散生成+ACC 评估管线（约 40 行）：

```c
// 两处完全一致的代码模式
DiffusionCtx dctx;
diffusion_init(&dctx, topo);
dctx.temperature = ...;
const char* words[DIFF_MAX_SEQUENCE];
int n = diffusion_generate(&dctx, input, words, DIFF_MAX_SEQUENCE);
GeneratedSequence seq = {0};
for (int i = 0; i < n && i < MAX_GENERATED_WORDS; i++)
    seq.words[i] = words[i];
seq.count = ...;
cingulate_evaluate(&seq, topo, input, 5);
```

**修复**：提取为 `cingulate_diffusion_evaluate(MasterTopology*, const char*, float, GeneratedSequence*)`，位于 `cingulate.h/c`。PFC、PFE、DMN 三个调用点统一使用。

#### 2. 提取 `pfe_post_process()` 消除推理后处理重复

`pfe_reason()` 和 `pfe_resume_reason()` 中约 100 行完全相同的后处理：

```
IdeaArena 竞争 (~50行) → 综合输出 → 统计更新 → 策略权重更新 → 推理结束信号
```

**修复**：提取为静态函数 `pfe_post_process(pfe, ws, question, answer_out, max_len)`。

---

### P1 — 脑区边界修复

#### 3. 统一子拓扑查找（11 处）

多个脑区中散落着手动 `for` 循环按 `sub->type` 查找子拓扑：

| 文件 | 数量 | 典型代码 |
|------|------|---------|
| `cingulate.c` | 3 | `for (t=0; t<topo->sub_topo_count; t++) if (sub->type==TOPO_SEMANTIC) ...` |
| `hippocampus.c` | 2 | 同上（查找 TOPO_VOCABULARY） |
| `amygdala.c` | 1 | 同上（查找 TOPO_EMOTION） |
| `perception.c` | 5 | 同上（查找 TOPO_VOCABULARY） |

**修复**：全部替换为 `master_get_sub_topology_by_type(master, TOPO_XXX)` — 一个 O(n) 已实现的公共函数。

#### 4. 海马体增加丘脑信号总线通路

`hippocampus_consolidate()` 原先直接调用 `perception_consolidate_node()`。现在同时通过丘脑发送 `THAL_SIG_CONSOLIDATE_NODE` 信号，为未来完全解耦做准备。

---

### P2 — 架构完善

#### 5. 昼夜节律阶段名称统一

`brainstem.c` 中有 3 处硬编码的阶段名称映射（中文日志、英文丘脑、状态 API）：

```c
// 3处完全一致的 if-else 链
(hour >= 0 && hour < 6)  ? "沉睡" / "sleep"
(hour >= 6 && hour < 10) ? "苏醒" / "waking"
...
```

**修复**：提取 `brainstem_circadian_phase_name(hour, lang)` — `lang=0` 英文，`lang=1` 中文。

#### 6. 布罗卡区（Broca）升级

**之前**：纯函数库（`broca_build_templates` / `broca_decay_templates`），无状态，构建间隔由脑干硬编码（`tick_count % 300`）。

**现在**：有状态结构体，自主生命周期：

```c
typedef struct Broca {
    MasterTopology* master;
    int   build_interval_ticks;  // 自主管理构建间隔
    int   max_build_depth;
    int   decay_threshold;
    float decay_rate;
    int   tick_count;
    int   total_builds;
    int   total_new_templates;
} Broca;

Broca* broca_create(MasterTopology*);
void   broca_destroy(Broca*);
int    broca_tick(Broca*);  // 每 tick 调用，内部管理调度
```

脑干不再硬编码 `300` tick 间隔，改为 `broca_tick(broca)` 委托。

---

### P3 — 新增脑区

#### 7. 下丘脑（Hypothalamus）— 需求/动机调控

哺乳动物的下丘脑调节饥饿、渴、体温、内分泌等内稳态需求。在 PivotMind 中映射为 **4 维需求动态调控**：

```c
typedef struct Hypothalamus {
    CognitiveState* state;
    float drive_decay[4];        // 自然衰减率（每 tick）
    float drive_baseline[4];     // 需求基线
    float drive_sensitivity;     // 对外部刺激的敏感度
    float circadian_modulation;  // 昼夜调制幅度
    int   ticks, dialog_events_processed;
    float last_circadian;
} Hypothalamus;
```

**核心机制**：

| 功能 | 实现 | 效果 |
|------|------|------|
| 自然衰减 | `hypothalamus_tick()` | 各 drive 每秒向基线回归 |
| 昼夜耦合 | `hypothalamus_set_circadian()` | 夜间好奇↑社交↓，白天社交↑好奇↓ |
| 对话刺激 | `hypothalamus_on_dialog()` | 正效价→ social↑ comfort↑；新颖度→ curiosity↑ |

**注册**：`THAL_HYPOTHALAMUS = 8`，丘脑子系统总数 8→9。脑干每 30 tick 调用 `hypothalamus_set_circadian()` + `hypothalamus_tick()`。

---

### 修复

#### 联网搜索修复

**问题**：所有 HTTPS 搜索（百度百科/百度搜索/中文维基）全部返回"无响应"。

**根因**：`web_search.c` 中 HTTPS 功能依赖 `#ifdef HAS_OPENSSL`，但 Makefile 从未定义该宏，也未链接 `libssl`/`libcrypto`。HTTPS 请求在 `do_fetch()` 中因无 SSL 实现而直接返回 NULL。

**修复**：
- `Makefile`: `CFLAGS` 加 `-DHAS_OPENSSL`，`LDFLAGS` 加 `-lssl -lcrypto`
- `ci.yml`: 安装 `libssl-dev`，ARM 交叉编译加 `-DHAS_OPENSSL`

#### CI 编译修复

| Bug | 文件 | 修复 |
|-----|------|------|
| `SYS_NAMES[9]` 只有 7 个初始化器 | `thalamus.c` | 补充 PFE + Hypothalamus 条目 |
| Broca 注册为 `NULL` → `broca_tick` 永不执行 | `gateway.c` | 创建 `Broca` 实例并注册 |
| Hypothalamus 未创建/未注册 | `gateway.c` | 创建实例并注册 |
| `brainstem_get_circadian_phase()` 第三处硬编码 | `brainstem.c` | 改用统一函数 |

---

## 架构演进

```
v0.3.0                               v0.4.0
───────                               ───────
8 脑区                               9 脑区
├─ 前额叶                            ├─ 前额叶
├─ 前额叶执行器 (PFE)                ├─ 前额叶执行器 (PFE)
├─ 海马体                            ├─ 海马体
├─ DMN                               ├─ DMN
├─ 感知皮层                          ├─ 感知皮层
├─ 布罗卡区 (纯函数) ❌              ├─ 布罗卡区 (有状态) ✅
├─ 小脑                              ├─ 小脑
├─ 杏仁核                            ├─ 杏仁核
└─                                   └─ 下丘脑 ✅ 新增

11 处手动 for 循环查找子拓扑          → 1 个公共函数调用
2 处 diffusion→cingulate 重复管线    → 1 个公共函数
2 处推理后处理重复 (~100行)           → 1 个静态函数
3 处硬编码昼夜节律名称               → 1 个公共函数
联网搜索全部失败 (缺 OpenSSL)         → 修复
```

---

## 编译验证

| 平台 | 结果 |
|------|------|
| Windows MinGW (GCC 13.2) | ✅ 82 源文件零错误编译，`libpivotmind.a` 创建成功 |
| ARM aarch64 (RK3399, GCC 13.3) | ✅ `pivotmind_gateway` + `seed_builder` 零错误编译，ELF ARM aarch64 |
| ARM 运行时冒烟测试 | ✅ 启动正常，丘脑子系统=9，昼夜节律正常，自学器周期#1 完成 |
