#!/usr/bin/env bash
# ============================================================================
# G-T3：ASan 5 支「与基线逐项对比，不是全绿」（§4.3）
#
# 判据（对照 falsification.md M3 基线表）：
#   test_tensor        EXIT=0，无报告
#   test_memory_unit   EXIT=134，LSan 20B/6 处，帧全在 tests/unit/test_memory.c:36/69（**不新增帧**）
#   test_diffusion_unit EXIT=0
#   test_topology_unit EXIT=134，LSan 40B/1 处，帧 src/multi_topology.c:1909 → :2284 → test_topology.c:54（**不新增帧**）
#   test_causal_unit   EXIT=0
# ⚠ 任务书写的「5 支保持绿」与既有证据不符：M3 实测是 3 绿 + 2 红(134)。
#   本脚本按「不劣化」判定。V2(库侧泄漏)/V3(测试卫生) 属**另批**，见 §6。
#
# 在哪跑：G15（ASan 可跑）或 armbian。副本树内跑，别污染主树。
# 用法：bash run_g_t3_asan.sh [副本树路径, 默认当前目录]
# ============================================================================
set -u
TREE="${1:-$(pwd)}"
ART="${PM_ART:-$TREE/tests/round3/artifacts}"
mkdir -p "$ART"
cd "$TREE" || exit 2

echo "[G-T3] tree=$TREE"
# 只用**构建目标**，绝不用 asan-test / test: 这类会执行测试的 recipe（§4.3）
make clean >/dev/null 2>&1
make CFLAGS="$(sed -n 's/^ASAN_CFLAGS *= *//p' Makefile)" \
     LDFLAGS="$(sed -n 's/^ASAN_LDFLAGS *= *//p' Makefile)" \
     test-tensor test-memory-unit test-diffusion-unit test-topology-unit test-causal-unit || exit 2

export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1
LOG="$ART/g_t3_asan_$(date +%Y%m%d_%H%M%S).txt"
: > "$LOG"
declare -A EXPECT=( [test_tensor]=0 [test_memory_unit]=134 [test_diffusion_unit]=0 [test_topology_unit]=134 [test_causal_unit]=0 )
BAD=0
for t in test_tensor test_memory_unit test_diffusion_unit test_topology_unit test_causal_unit; do
    echo "########## RUN $t ##########" | tee -a "$LOG"
    timeout 900 ./build/bin/$t >>"$LOG" 2>&1
    rc=$?
    echo "EXIT=$rc  (基线期望 ${EXPECT[$t]})" | tee -a "$LOG"
    if [ "$rc" -ne "${EXPECT[$t]}" ]; then echo ">>> $t 退出码与基线不一致"; BAD=$((BAD+1)); fi
done

# 帧集合核对（不新增帧）：从本支日志里切出各自的段再 grep
echo ""
echo "===== 帧集合核对（不新增帧）====="
# test_memory_unit 的帧只允许出现在 tests/unit/test_memory.c:36 / :69
MEM_FRAMES=$(awk '/RUN test_memory_unit/,/RUN test_diffusion_unit/' "$LOG" | grep -oE '(src|tests|include)/[^ :]*:[0-9]+' | sort -u || true)
echo "test_memory_unit 帧集合:"; echo "$MEM_FRAMES" | sed 's/^/  /'
TOPO_FRAMES=$(awk '/RUN test_topology_unit/,/RUN test_causal_unit/' "$LOG" | grep -oE '(src|tests|include)/[^ :]*:[0-9]+' | sort -u || true)
echo "test_topology_unit 帧集合:"; echo "$TOPO_FRAMES" | sed 's/^/  /'

echo ""
echo "[G-T3] 退出码不符支数=$BAD  log=$LOG"
if [ "$BAD" -eq 0 ]; then echo "G-T3 PASS (不劣化)"; exit 0; fi
echo "G-T3 RED：新增帧 / 退出码变化 = 门禁红（消失也是异常，说明改动触及了不该触的路径）"
exit 1
