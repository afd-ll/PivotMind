#!/usr/bin/env bash
# ============================================================================
# G-T4：armbian 全量 23/23（§4.4）
#   构建机 = 树莓派（内存紧：**一律 -j1**，见 falsification.md「环境备注」）
#   运行机 = armbian（cx@100.107.169.41，aarch64 6 核）
# 判据：PASS=23 FAIL=0 TOTAL=23，汇总 Failed: 0。
#   23 = Makefile:311 的 TEST_BINS 清单，必须与 Makefile:314 `test:` 的 23 个前置逐项一致。
# 用法（在本机/Pi 上）：bash tests/round3/run_g_t4_armbian.sh
# ============================================================================
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ART="$ROOT/tests/round3/artifacts"
mkdir -p "$ART"
HOST="${PM_ARMB_HOST:-cx@100.107.169.41}"
REMOTE_DIR="${PM_ARMB_DIR:-~/pm-r3}"
LOG="$ART/g_t4_armbian_$(date +%Y%m%d_%H%M%S).txt"

cd "$ROOT" || exit 2

# --- 清单自检：TEST_BINS 与 test: 前置必须逐项一致（Makefile:307-310 明写的纪律）---
echo "[G-T4] 清单自检"
TESTBINS_N=$(make -p 2>/dev/null | sed -n 's/^TEST_BINS *= *//p' | tr ' ' '\n' | grep -c 'test_')
PREREQ_N=$(sed -n 's/^test: \(.*\)$/\1/p' Makefile | tr ' ' '\n' | grep -c '^test-')
echo "  TEST_BINS 条目=$TESTBINS_N  test: 前置目标=$PREREQ_N （两者都应为 23）"

# --- 构建：Pi 上 -j1 ---
echo "[G-T4] 构建（Pi，-j1）： make clean && make -j1 test-build（不存在则回退 libpivotmind.a 并逐支构建）"
make clean >/dev/null 2>&1
if ! make -j1 test-build 2>/dev/null; then
    echo "  test-build 目标不存在 -> 回退 libpivotmind.a（方案 §4.4 的写法）"
    make -j1 libpivotmind.a || exit 2
    make -j1 test-tensor test-tensor-broadcast test-model test-metrics test-trainer \
        test-chinese test-web-fetch test-dialog-unit test-diffusion-unit test-topology-unit \
        test-memory-unit test-learner-unit test-causal-unit test-forgetting-unit \
        test-media-reader test-visual-cortex test-pure test-search test-pfe-unit \
        test-regression test-semantic-growth test-integration test-cc || exit 2
fi

# --- 运行：armbian，逐支 + 退出码 ---
echo "[G-T4] scp -> $HOST:$REMOTE_DIR 并逐支运行"
ssh "$HOST" "mkdir -p $REMOTE_DIR"
scp build/bin/test_* "$HOST:$REMOTE_DIR/" || exit 2

ssh "$HOST" 'cd '"$REMOTE_DIR"' && PASS=0; FAIL=0; for t in test_tensor test_tensor_broadcast test_model \
  test_metrics test_trainer test_chinese test_web_fetch test_dialog_unit test_diffusion_unit \
  test_topology_unit test_memory_unit test_learner_unit test_causal_unit test_forgetting_unit \
  test_media_reader test_visual_cortex test_pure test_search test_pfe_unit test_regression \
  test_semantic_growth test_integration test_cc; do timeout 300 ./$t >/dev/null 2>&1 && PASS=$((PASS+1)) || { echo "RED: $t"; FAIL=$((FAIL+1)); }; done; echo "PASS=$PASS FAIL=$FAIL TOTAL=$((PASS+FAIL))"' \
  | tee "$LOG"

echo "[G-T4] log=$LOG"
if grep -q 'PASS=23 FAIL=0 TOTAL=23' "$LOG"; then echo "G-T4 PASS"; exit 0; fi
echo "G-T4 RED -> 见 §6.3（必须真跑才能确认的条目）"
exit 1
