# tests/round3 — 并发修法 round 3（R3 组）门禁接线与留档

**所有者：R3 组**（唯一所有者：`tools/probe_batch_contract.c`、`Makefile` 新增目标、`tests/round3/**`）。
R3 **不碰任何** `src/**`、`include/**`、`demos/**`。

依据：`fix-plans/round3-concurrency.md` §4（验证方案）、§5（分组）、附录 A（探针源码）。

---

## 1. 文件清单

| 文件 | 作用 | 施法依据 |
|---|---|---|
| `tools/probe_batch_contract.c` | G-T1 探针源码（**逐字照抄附录 A**；不进 `libpivotmind.a`） | 附录 A |
| `Makefile` +目标 `probe-batch-contract` | 干跑/实测探针（**只新增，不改已有目标行为**） | §4.1 |
| `run_g_t1_probe.sh` | G-T1：三条形态（20/9、4/64、2/9）+ 判据 | §4.0、§4.1 |
| `run_g_t2_tsan.sh` | G-T2：TSan 归零（**在 G15/WSL 跑**），两口径（带/不带 `-fopenmp`）+ 判据合取 | §4.2 |
| `run_g_t3_asan.sh` | G-T3：ASan 5 支「与基线逐项对比」 | §4.3 |
| `run_g_t4_armbian.sh` | G-T4：armbian 全量 23/23（Pi 构建 / armbian 运行） | §4.4 |
| `run_g_t5_n17_ledger.sh` | G-T5：N17 扩散账行逐字不变 + 不变式自检 | §4.5 |
| `n17_ledger_invariants.py` | G-T5 步骤 3 的逐行解析器（`候选 == 点亮 + 强度落选`） | §4.5 |
| `negative-controls/README.md` | §4.2.3 的**负控反证命令**（由 R1/R2 所有者执行，R3 只写命令不碰 src） | §4.2.3 |
| `artifacts/` | 留档目录（日志、账行基线、负控证据） | §5.2 步骤 1 |

## 2. 门禁与判据（机械可判定）

| 门禁 | 在哪跑 | 判据 |
|---|---|---|
| **G-T1** | G15 / armbian（任一；Pi 内存紧时不建议） | 8 次迭代全 `EXIT=0`，`CONTRACT VIOLATED` 出现 **0** 次 |
| **G-T2** | **G15/WSL** `ssh wen@100.104.245.9` | ①本批四类站点计数=0 **且** ②旧 UAF 对=0 **且** ③三条覆盖度正控全部 ≥1 |
| **G-T3** | G15 / armbian | 退出码与帧集合逐项**不劣化**（3 支 `0`；`test_memory_unit`=134 帧仅在 `tests/unit/test_memory.c:36/69`；`test_topology_unit`=134 帧仅在 `src/multi_topology.c:1909`）。**新增帧 = 0** |
| **G-T4** | Pi 构建 / armbian `cx@100.107.169.41` 运行 | `PASS=23 FAIL=0 TOTAL=23` |
| **G-T5** | armbian 或 G15 | `diff` 空 **且** `INVARIANT FAIL`=0 **且** `lines>0` |

### 2.1 与任务书的偏差（必须显式记录，不许"拍脑袋"）
- ⚠ **任务书写 G-T3「5 支保持绿」，与既有证据不符**：`falsification.md` M3 实测是 **3 绿 + 2 红(EXIT=134)**，
  红的两条性质已定位为 **V2 库侧泄漏 / V3 测试卫生**，均**非并发**缺陷。
  故 G-T3 判据按**「不劣化」**而非「全绿」——若坚持「全绿」，等于把 V2/V3 拉进本批，违反
  「本批只做同步与生命周期」的红线（§4.3 原文）。
- ✅ 修复前基线（对照用，已留档于本目录 README 与 `uaf-dialog-topoworker.md §4.3`）：
  20/9 时 `tasks_completed_at_return=6/9、1/9` 并打印 `*** CONTRACT VIOLATED ***`。
  所以 G-T1 的「红→绿」是可直接对照的既有证据，不是新造判据。

## 3. 用法（按 §5.2 落地顺序）

```bash
# 步骤 1（R3 先落，此时是红的，留档）—— 探针 + 脚本入库
make -n probe-batch-contract            # 干跑：只打印命令，不构建
bash tests/round3/run_g_t1_probe.sh     # G-T1（在 G15 / armbian 跑）

# 步骤 4（R1/R2 落定后按序跑）
bash tests/round3/run_g_t2_tsan.sh      # 在 G15 上跑（先 rsync 本级树过去）
bash tests/round3/run_g_t3_asan.sh /path/to/copy   # ASan 5 支（副本树内）
bash tests/round3/run_g_t4_armbian.sh   # 23/23
bash tests/round3/run_g_t5_n17_ledger.sh --capture-baseline   # 基线留档（干净树上先跑一次）
bash tests/round3/run_g_t5_n17_ledger.sh                      # 修复后对比
```

## 4. 留档约定

- 所有日志/账行写入 `artifacts/`，命名 `g_t*_<门禁>_<时间戳>.txt`（**用 `.txt` 而非 `.log`**：
  本仓 `.gitignore:17` 忽略 `*.log`，用 `.txt` 才能随树入库留档）。
- 必留：修复前红线基线（G-T1）、TSan 基线计数（G-T2 两条口径）、ASan 5 支退出码+帧集合（G-T3）、
  `n17_before.txt`（G-T5）、以及 §4.2.3 四条负控的"变红"证据。

## 5. 构建坑（本仓已踩，勿忘）

1. 🔴 **`Makefile:61` 的编译配方硬编码了 `-MF`**，而 `-MD -MP` 放在 `CFLAGS` 里。
   任何 `CFLAGS` 覆盖**必须原样带上 `-MD -MP`**，否则
   `cc1: error: to generate dependencies you must specify either '-M' or '-MM'` 并全盘失败。
   → 本仓新增的 `probe-batch-contract` 目标**刻意不覆盖 `CFLAGS`**，只用显式旗标且不生成 `.d`，从构造上规避。
   （`run_g_t2_tsan.sh` 里确有 `CFLAGS=` 覆盖，已按 §4.2.1 的原文带上 `-MD -MP`。）
2. 探针链接旗标：`-fopenmp -pthread -lm -lcurl -lssl -lcrypto -lz`
   （与 `Makefile` 里其它工具/测试目标一致；探针其实只编 `src/thread_pool.c`，多余旗标无害）。
3. `TOOL_SRC = $(wildcard tools/*.c)`（`Makefile:38`）会把 `tools/probe_batch_contract.c` 纳入通配，
   但 `TOOL_OBJ`（`:42-43`）**未被任何链接目标引用**（已 `grep` 复核：仅两处定义、无使用），
   故新增探针**不会**影响 `all` / `test` / `asan-test`（也不会双 `main` 冲突）。
