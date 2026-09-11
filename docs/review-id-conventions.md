# PivotMind 审查编号约定

> **用途**：统一 2026-09 全量审查与修复过程中产生的各类编号（报告条目、实施分组、反向验证实验、验证门、门禁漏洞），让报告、方案、changelog、验证记录与**源码注释**里的编号含义唯一、可追溯。
> **用法**：给子代理写指令时只需写一句「编号按 `docs/review-id-conventions.md`」，不必每次重复解释。
> **建立**：2026-09-11（round 3 验证收口后）。

---

## 1. 编号总表

| 前缀 | 含义 | 编号范围 | 权威出处 |
|---|---|---|---|
| `A`–`E` | **基线审查五路报告的 finding**：A=持久化、B=并发与网关、C=拓扑学习、D=认知与语言、E=nn·构建·测试 | `A-P0-1` … | `A-persistence.md`、`B-concurrency-gateway.md`、`C-topology-learning.md`、`D-cognition-language.md`、`E-nn-build-tests.md` |
| `P0` / `P1` | 严重度：P0=必修、P1=应修 | — | 同上 |
| `N` | **红线普查的新发现**（N = New）：限生长 / 静默减少拓扑 / 表示层天花板 | `N1` … `N19` | `redline-sweep.md`、`redline-policy-draft.md` |
| `PB` | **数据保命批**的实施分组 | `PB1`–`PB3` | `fix-plans/batch0-persistence.md` |
| `H` | **并发与生命周期批**的实施分组 | `H1`–`H7` | `fix-plans/batch2-concurrency.md` |
| `I` | **门禁与诚实批**的实施分组 | `I1`–`I5` | `fix-plans/batch3-gates.md` |
| `R` | **round 3 并发修复**的施工分组 | `R1`（批次完成语义）、`R2a`/`R2b`（锁域）、`R3`（探针与门禁） | `fix-plans/round3-concurrency.md` |
| `R3-n` | round 3 的**条目级代码标签**（写在源码注释里，可 grep） | `R3-1`、`R3-2`、`R3-3` | `grep -rn "R3-" src/` |
| `G-Tn` | **验证门**（Gate-Test） | `G-T1` … `G-T5` | `tests/round3/README.md`、`round3-verify-*.md` |
| `V n` | 反向验证挖出的**门禁漏洞**（V = Vulnerability：该门禁"其实没牙"） | `V1` … `V4` | `falsification.md` |
| `C n` / `S n` | 方案内部用：`C`=跨组契约、`S`=方案自检步骤 | — | 各 `fix-plans/*.md` |
| `Bug A` / `Bug B` | 验证期抓到的真 bug（超纲，直接用字母命名） | — | `uaf-dialog-topoworker.md`、`crash-x86-cognitive-controller.md` |

---

## 2. 验证门固定含义（G-T1 … G-T5）

| 门 | 验什么 | 判据（本批实测） |
|---|---|---|
| **G-T1** | 批次完成语义：`thread_pool_batch` 是否"返回即本批已全部执行完" | 探针 **58/58 HOLDS、0 违约**（round2 = **40 违约**、base = **38 违约**） |
| **G-T2** | 数据竞争（TSan） | **本批站点零报告** ＋ **覆盖度正控**（gcov 证明单线程与 OpenMP 两条路径分别被跑到） |
| **G-T3** | ASan / UBSan | 与基线**逐项不劣化**、**新增帧 = 0** |
| **G-T4** | aarch64 回归（armbian） | 全套 **23/23**、**无新增** warning/error |
| **G-T5** | N17 扩散行为不变 | `[扩散前沿]` 账行**逐字节**一致（含顺序无关）＋不变量 `候选 == 点亮 + 强度落选` |

---

## 3. 反向验证实验（M）：两类，必须带类型后缀

反向验证的核心是「**故意把它弄坏，看门禁红不红**」。实验分两类，历史上都写作 `M1…Mn`，故本约定加类型后缀消歧：

| 新写法 | 含义 | 实例 |
|---|---|---|
| **`M-MUT-n`** | **Mutation**：故意弄坏（改源码/改输入），期望门禁变红 | `M-MUT-1` 删 `matrix_ops.c` 的 `memset`；`M-MUT-2` 剪种子文件末尾 16 字节；`M-MUT-3` ASan 实跑；`M-MUT-4` 并发最小复现 |
| **`M-MET-n`** | **Measurement**：实测项（不弄坏，只测） | `batch3-gates.md §6` 的 10 项实测 |

**历史映射（旧文档不改写，按此读）**：
- `falsification.md` 里的 `M1–M4` = 现在的 `M-MUT-1` … `M-MUT-4`。
- `fix-plans/batch3-gates.md §6` 里的 `M1–M10` = 现在的 `M-MET-1` … `M-MET-10`。

---

## 4. 历史撞名的处理：只消歧、不重命名

| 撞名 | 消歧规则 |
|---|---|
| `G1–G3`（保命批分组）与 `G-T1–G-T5`（验证门） | 分组统一写作 **`PB1–PB3`**；**验证门永远带 `-T`**（`G-Tn`），两者不再混用。历史文档中的 `G1–G3` 即 `PB1–PB3` |
| `M`（两种含义） | 见 §3，一律带 `-MUT` / `-MET` 后缀 |

**原则：不回头重命名已产出的报告、changelog 与代码注释**——重命名会制造新的引用漂移，而编号的价值在于稳定。历史文档按本文件的映射规则阅读。

---

## 5. 新增编号的规则

1. 新开一条线 → 取一个**未被占用的前缀**，**先在本文件登记**再使用。
2. 每批修复的**分组前缀必须在方案文件里声明一次**（如 round 3 的 `R`）。
3. 条目级代码标签写进注释时，格式固定为 `前缀-序号: 说明`（便于 `grep -rn "R3-" src/` 追溯）。

---

## 6. 与发布物的关系

- `changelogs/069`–`071` 与 `CHANGELOG.md` 的 `v0.5.25`–`v0.5.27` 节引用上述编号。
- 阅读顺序：**本文件 → 对应 changelog → 对应专题报告**（`A`–`E` / `redline-sweep` / `falsification` / `round3-verify-*`）。
