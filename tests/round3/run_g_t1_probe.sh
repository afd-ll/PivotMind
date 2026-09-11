#!/usr/bin/env bash
# ============================================================================
# G-T1：批次完成语义探针（R3 交付物；源码 = tools/probe_batch_contract.c，方案附录 A）
#
# 判据（§4.0）：每次 batch() 返回时 tasks_completed == tasks；
#              全部迭代 EXIT=0 且 "CONTRACT VIOLATED" 出现 0 次。
# 修复前基线（为对照）：uaf-dialog-topoworker.md §4.3 —— 20/9 时出现
#              tasks_completed_at_return=6/9、1/9 并打印 *** CONTRACT VIOLATED ***。
#
# 在哪跑：§4.0 说「任一机器（G15 或本机实测树）」。本机是 Pi 3B/905MB，
#         建议在 G15 或 armbian 上跑（构建+运行都轻，1 秒量级）。
# 用法：  bash tests/round3/run_g_t1_probe.sh [输出二进制路径]
# ============================================================================
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ART="$ROOT/tests/round3/artifacts"
mkdir -p "$ART"
BIN="${1:-/tmp/probe_batch_contract}"
CC_BIN="${CC:-gcc}"
LOG="$ART/g_t1_probe_$(date +%Y%m%d_%H%M%S).txt"

echo "[G-T1] tree=$ROOT"
echo "[G-T1] build: $CC_BIN -O1 -g -pthread -Iinclude -o $BIN tools/probe_batch_contract.c src/thread_pool.c"
# 注意：探针只编 src/thread_pool.c（契约单元测试），**不链接 libpivotmind**。
if ! ( cd "$ROOT" && $CC_BIN -O1 -g -pthread -Iinclude -o "$BIN" \
        tools/probe_batch_contract.c src/thread_pool.c ); then
    echo "[G-T1] BUILD FAILED (rc=$?)"
    exit 2
fi

{
    echo "### G-T1 $(date -Is)"
    echo "### tree=$ROOT bin=$BIN"
    echo "--- 20 workers / 9 tasks / 3 iters（复刻 dialog_system 的 9 拓扑形态）---"
    "$BIN" 20 9 3 ; echo "EXIT=$?"
    echo "--- 4 workers / 64 tasks / 50 iters（复刻 M4）---"
    "$BIN" 4 64 50 ; echo "EXIT=$?"
    echo "--- 2 workers / 9 tasks / 5 iters（工人比活少的最恶劣形态）---"
    timeout 60 "$BIN" 2 9 5 ; echo "EXIT=$?"
} 2>&1 | tee "$LOG"

VIOL=$(grep -c 'CONTRACT VIOLATED' "$LOG" || true)
BADC=$(grep -c 'BAD RC' "$LOG" || true)
ITERS=$(grep -c 'CONTRACT HOLDS' "$LOG" || true)
echo ""
echo "[G-T1] CONTRACT VIOLATED=$VIOL  BAD RC=$BADC  HOLDS=$ITERS"
echo "[G-T1] log=$LOG"
if [ "$VIOL" -eq 0 ] && [ "$BADC" -eq 0 ] && [ "$ITERS" -eq 8 ]; then
    echo "G-T1 PASS (0 violations / 8 held iters [3+50+5])"
    exit 0
fi
echo "G-T1 RED -> 见 §4.2.3 负控：把判据改回 workers_done 必须立刻变红"
exit 1
