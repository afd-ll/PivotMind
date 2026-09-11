# tests/round3/artifacts — 门禁留档目录

所有门禁的日志/账行/负控证据落在这里。命名约定：`g_t*_<门禁>_<时间戳>.txt`
（**用 `.txt` 而非 `.log`**：本仓 `.gitignore:17` 忽略 `*.log`，`.txt` 才能随树入库留档）。

## 必留清单（§4.0 / §5.2）

| 文件 | 由谁产出 | 内容 |
|---|---|---|
| `g_t1_probe_<ts>.txt` | `run_g_t1_probe.sh` | G-T1 三条形态的逐迭代行 + `EXIT=`；**修复前的红线基线必须留一份** |
| `tsan_fix_r3_openmp.txt` / `tsan_fix_r3_noopenmp.txt` | `run_g_t2_tsan.sh` | TSan 两口径全量日志（判据 ①②③ 的原始输入） |
| `g_t3_asan_<ts>.txt` | `run_g_t3_asan.sh` | ASan 5 支逐支日志 + `EXIT=` + 帧集合核对 |
| `g_t4_armbian_<ts>.txt` | `run_g_t4_armbian.sh` | armbian 逐支运行结果 + `PASS/FAIL/TOTAL` |
| `n17_before.txt` | `run_g_t5_n17_ledger.sh --capture-baseline` | 修复前 `[扩散前沿]` 账行（逐字对照的基线） |
| `n17_after_<ts>.txt` | `run_g_t5_n17_ledger.sh` | 修复后账行 |
| `tsan_fix_r3_ncNN.txt` | 负控（R1/R2 所有者执行，见 `../negative-controls/README.md`） | 逐处注释加锁后的"变红"证据 |

## 留档台账（每跑一次补一行）

| 时间 | 门禁 | 树/提交 | 结果 | 文件 |
|---|---|---|---|---|
| （待验证阶段填写） | | | | |
