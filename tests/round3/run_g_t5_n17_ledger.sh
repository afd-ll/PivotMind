#!/usr/bin/env bash
# ============================================================================
# G-T5：N17 扩散账行不变（§4.5）
#
# 判据：diff 空（逐字不变）**且** INVARIANT FAIL 计数 0 **且** lines>0。
# 为什么必须逐字不变：本批**不碰 src/diffusion.c 一行**（N17 已在上一批落地）；
#   账行一旦变了 => R2 的锁改动泄漏进了扩散路径 => 必须回退定位。
#   `候选 == 点亮 + 强度落选` 是 n17-spread-cap.md 的验收口径（不许静默丢弃）。
# 账行格式见 src/diffusion.c:1451-1460；开关 _spread_ledger_on()（默认开，PM_SPREAD_LEDGER=0 关）。
#
# 用法：
#   # 1) 修复前先留档基线（必须在干净基线上跑一次）
#   bash tests/round3/run_g_t5_n17_ledger.sh --capture-baseline
#   # 2) 修复后对比
#   bash tests/round3/run_g_t5_n17_ledger.sh
# ============================================================================
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ART="$ROOT/tests/round3/artifacts"
mkdir -p "$ART"
BIN="${PM_CC_BIN:-$ROOT/build/bin/test_cc}"
BEFORE="$ART/n17_before.txt"

if [ "${1:-}" = "--capture-baseline" ]; then
    echo "[G-T5] 取基线账行（同一输入、同一 seed）-> $BEFORE"
    PM_SPREAD_LEDGER=1 timeout 300 "$BIN" 2>&1 | grep '^\[扩散前沿\]' | tee "$BEFORE"
    echo "[G-T5] 基线行数=$(grep -c . "$BEFORE" || true)"
    exit 0
fi

AFTER="$ART/n17_after_$(date +%Y%m%d_%H%M%S).txt"
echo "[G-T5] 取修复后账行（同一输入、同一 seed）-> $AFTER"
PM_SPREAD_LEDGER=1 timeout 300 "$BIN" 2>&1 | grep '^\[扩散前沿\]' > "$AFTER" || true
echo "[G-T5] 修复后行数=$(grep -c . "$AFTER" || true)"

RC=0
echo ""
echo "===== 2) 与修复前留档对比（期望：diff 空）====="
if [ -f "$BEFORE" ]; then
    if diff "$BEFORE" "$AFTER"; then echo "N17 LEDGER: IDENTICAL"; else echo ">>> N17 LEDGER: DIFF => RED"; RC=1; fi
else
    echo ">>> 缺基线 $BEFORE —— 先跑 --capture-baseline（本项无法判定）"; RC=1
fi

echo ""
echo "===== 3) 不变式自检：候选 == 点亮 + 强度落选 ====="
python3 "$ROOT/tests/round3/n17_ledger_invariants.py" "$AFTER" || RC=1

if [ "$RC" -eq 0 ]; then echo "G-T5 PASS"; else echo "G-T5 RED"; fi
exit "$RC"
