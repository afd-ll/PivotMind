#!/usr/bin/env bash
# =============================================================================
# 长跑监护（opt-in，约 15 分钟） —— 「分钟级才现形的死」的长跑护栏
# =============================================================================
# 背景：src/brainstem.c 的 brainstem_tick_synapse_scale() 只在 tick%600==0 时
#   才走到那行代码。一处「持 master 读锁期间调用取 master 写锁的函数」会造成
#   glibc 同线程「读→写」自升级 ⇒ 永久自死锁，表现是【启动约 11 分钟后 tick 冻在
#   600】。它是纯自死锁、无数据竞争 ⇒ TSan/Helgrind 不报；秒级单测结构上也覆盖不到。
#   所以只能靠这条长跑护栏：跑满 ≥900 tick，断言 tick 越过 600 后持续增长。
#
# 用法（**必须在允许运行 PivotMind 产物的机器上跑**，本项目是 armbian）：
#   tests/longrun/run_longrun_guard.sh --bin <pivotmind_gateway 路径> [选项]
#
# 选项：
#   --bin PATH        被测二进制（必填，或用 $PIVOTMIND_GATEWAY）
#   --data DIR        沙箱数据目录（默认 $HOME/pm-lockguard/<tag>）；绝不许指向线上数据目录。
#                     ⚠ 本脚本会把它 export 成 PIVOTMIND_HOME —— v0.5.30 起数据落点由
#                       pm_home() 决定，argv 只喂给 chdir()（语料相对路径）。不设
#                       PIVOTMIND_HOME 时引擎会读写 $HOME/pivotmind（= 线上数据目录），
#                       脚本自以为的「沙箱」就是假的（v0.5.33 修）。
#   --port N          监听端口（默认 8421，与线上 8080 隔离）
#   --minutes M       最长跑多久（默认 17）
#   --tick-target N   tick 目标（默认 900）
#   --interval S      采样间隔秒（默认 20）
#   --stall-tol S     「停滞」容忍秒数（默认 90）—— 超过即判 FAIL
#   --keep            结束后保留沙箱目录（默认保留；本脚本从不删数据）
#
# 退出码：0 = PASS；1 = FAIL；2 = 用法/环境错误
#
# ⚠️ 停止引擎**一律 kill -9**：强杀不触发退出存盘，不会污染沙箱数据。
# ⚠️ 令牌：默认**不读令牌**，tick 一律从日志 `[堆监控] tick=` 抓（脱敏无凭据）。
#     `--use-status` 才会去读数据目录的 gw_token 打 /status；令牌**永不打印**，
#     输出里一律写 [REDACTED]。
# =============================================================================
set -u

BIN=""
DATA=""
PORT=8421
MINUTES=17
TICK_TARGET=900
INTERVAL=20
STALL_TOL=90
USE_STATUS=0
TAG="$(date +%Y%m%d-%H%M%S)"

while [ $# -gt 0 ]; do
    case "$1" in
        --bin) BIN="$2"; shift 2 ;;
        --data) DATA="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --minutes) MINUTES="$2"; shift 2 ;;
        --tick-target) TICK_TARGET="$2"; shift 2 ;;
        --interval) INTERVAL="$2"; shift 2 ;;
        --stall-tol) STALL_TOL="$2"; shift 2 ;;
        --use-status) USE_STATUS=1; shift ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

[ -n "$BIN" ] || BIN="${PIVOTMIND_GATEWAY:-}"
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    echo "用法错误: 需要 --bin <可执行的 pivotmind_gateway 路径>" >&2
    exit 2
fi
BIN="$(readlink -f "$BIN")"

# ---- 环境铁律：本机（受限验证机）禁止运行任何 PivotMind 产物 -----------------
MEM_MB="$(awk '/MemTotal/{printf "%d", $2/1024}' /proc/meminfo 2>/dev/null || echo 99999)"
if [ "$MEM_MB" -lt 2000 ] && [ "${ALLOW_LOWMEM_RUN:-0}" != "1" ]; then
    echo "拒绝运行: 本机总内存 ${MEM_MB}MB < 2000MB —— 这是受限验证机（Pi），" >&2
    echo "          禁止在本机运行 PivotMind 产物（只许编译）。请在 armbian 上跑。" >&2
    echo "          确要覆盖请显式 ALLOW_LOWMEM_RUN=1（不推荐）。" >&2
    exit 2
fi

# ---- 沙箱目录：绝不许落在线上数据目录 ---------------------------------------
if [ -z "$DATA" ]; then
    DATA="$HOME/pm-lockguard/$TAG"
fi
mkdir -p "$DATA" || { echo "无法创建沙箱目录: $DATA" >&2; exit 2; }
DATA="$(readlink -f "$DATA")"
case "$DATA" in
    /home/cx/pivotmind|/home/cx/pivotmind/*|/mnt/sdcard|/mnt/sdcard/*)
        echo "拒绝运行: 沙箱目录 $DATA 落在线上/坏盘数据目录内。" >&2; exit 2 ;;
esac

# ---- 隔离铁律（v0.5.33）：把路径 SSOT 钉死在沙箱上 -------------------------
# 数据落点由 pm_home() 决定，argv 只喂 chdir()。不设 PIVOTMIND_HOME 时引擎会
# 读写 $HOME/pivotmind（= 线上数据目录），本脚本的「沙箱」形同虚设。
export PIVOTMIND_HOME="$DATA"

LOG="$DATA/gw.log"
SAMPLES="$DATA/samples.txt"
: > "$LOG"
: > "$SAMPLES"

# ---- 令牌文件存在性检查（v0.5.33 更正：不再是编译期绝对路径）----------------
# GW_TOKEN_FILE 现在 = pm_file(PM_FILE_TOKEN) = <pm_home()>/data/gw_token，即沙箱内。
TOKEN_FILE="$DATA/data/gw_token"
if [ ! -r "$TOKEN_FILE" ] && [ "$USE_STATUS" = "1" ]; then
    echo "警告: 令牌文件 $TOKEN_FILE 不可读，--use-status 将被禁用（token=[REDACTED]）" >&2
    USE_STATUS=0
fi

echo "════════════════════════════════════════════════════════════════════"
echo " 长跑监护  bin=$BIN"
echo "           沙箱=$DATA  端口=$PORT  目标 tick>=$TICK_TARGET  最长 ${MINUTES}min"
echo "           PIVOTMIND_HOME=$PIVOTMIND_HOME（SSOT 已钉在沙箱；argv 只喂 chdir）"
echo "           采样=${INTERVAL}s  停滞容忍=${STALL_TOL}s  令牌=[REDACTED]"
echo "════════════════════════════════════════════════════════════════════"

PID=""
cleanup() {
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        echo "[监护] kill -9 $PID（强杀，不触发退出存盘）"
        kill -9 "$PID" 2>/dev/null
        wait "$PID" 2>/dev/null
    fi
}
trap cleanup EXIT INT TERM

# ---- 启动 ---------------------------------------------------------------
cd "$DATA" || exit 2
PIVOTMIND_LOG_FILE="$LOG" "$BIN" "$PORT" "$DATA" >"$LOG.stdout" 2>&1 &
PID=$!
echo "[监护] 已启动 pid=$PID，等待首次 [堆监控] …"

T0="$(date +%s)"
DEADLINE=$((T0 + MINUTES * 60))

latest_tick() {
    # 从日志抓最后一个 [堆监控] tick=N（脱敏：不碰任何凭据）
    grep -oE '\[堆监控\][^0-9]*tick=[0-9]+' "$LOG" 2>/dev/null \
        | tail -1 | grep -oE '[0-9]+$'
}

wchan_hist() {   # $1=pid -> "futex_wait_queue(4) inet_csk_accept(1)" 之类
    [ -d "/proc/$1/task" ] || { echo "-"; return; }
    for t in /proc/"$1"/task/*; do
        [ -r "$t/wchan" ] && cat "$t/wchan" 2>/dev/null; echo
    done | sed '/^$/d' | sort | uniq -c | sort -rn \
        | awk '{printf "%s(%s) ", $2, $1}' | sed 's/ $//'
}

rss_kb() { awk '/VmRSS/{print $2}' "/proc/$1/status" 2>/dev/null; }

prev_tick=""; max_tick=0
max_stall=0; stall_start=0; stall_at_tick=""
last_progress_ts="$T0"
stuck_at_600=0

printf '%-7s %-7s %-7s %-8s %s\n' "elapsed" "tick" "rss_MB" "alive" "wchan_histogram(线程数)" | tee -a "$SAMPLES"

while :; do
    NOW="$(date +%s)"
    ELAPSED=$((NOW - T0))
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "[监护] 引擎已退出（elapsed=${ELAPSED}s）—— 见 $LOG 尾部"
        tail -20 "$LOG"
        echo ""
        echo "LONGRUN-GUARD: FAIL (引擎提前退出，未跑满 $TICK_TARGET tick)"
        exit 1
    fi
    T="$(latest_tick)"; T="${T:-0}"
    [ "$T" -gt "$max_tick" ] && max_tick="$T"

    if [ -n "$prev_tick" ] && [ "$T" = "$prev_tick" ]; then
        stall_for=$((NOW - last_progress_ts))
        [ "$stall_for" -gt "$max_stall" ] && { max_stall="$stall_for"; stall_at_tick="$T"; }
    else
        if [ -n "$prev_tick" ]; then
            stall_for=$((NOW - last_progress_ts))
            [ "$stall_for" -gt "$max_stall" ] && { max_stall="$stall_for"; stall_at_tick="${prev_tick}"; }
        fi
        last_progress_ts="$NOW"
    fi
    prev_tick="$T"

    RSSK="$(rss_kb "$PID")"
    printf '%-7s %-7s %-7s %-8s %s\n' "${ELAPSED}s" "$T" \
        "$(awk -v k="${RSSK:-0}" 'BEGIN{printf "%.1f", k/1024}')" "yes" "$(wchan_hist "$PID")" \
        | tee -a "$SAMPLES"

    if [ "$USE_STATUS" = "1" ]; then
        TOK="$(cat "$TOKEN_FILE" 2>/dev/null)"
        if [ -n "$TOK" ]; then
            ST="$(curl -s -m 5 -H "X-Pivot-Token: $TOK" "http://127.0.0.1:$PORT/status" 2>/dev/null | head -c 400)"
            echo "        /status(token=[REDACTED]): ${ST:0:200}" | tee -a "$SAMPLES"
        fi
        unset TOK
    fi

    [ "$NOW" -ge "$DEADLINE" ] && break
    # 到达目标 tick 后也提前收工（省时间），但至少采到 3 行
    if [ "$T" -ge "$TICK_TARGET" ] && [ "$T" -gt 600 ] && [ "$ELAPSED" -ge $((INTERVAL * 3)) ]; then
        echo "[监护] 已达目标 tick=$T（>= $TICK_TARGET），提前收工"
        break
    fi
    sleep "$INTERVAL"
done

# ---- 判定 ---------------------------------------------------------------
echo ""
echo "──────── 采样序列（$SAMPLES）────────"
cat "$SAMPLES"
echo "──────────────────────────────────"
echo "max_tick=$max_tick  target=$TICK_TARGET"
echo "最长无进展时长=${max_stall}s @ tick=${stall_at_tick:-?}  (容忍 ${STALL_TOL}s)"
echo "线程 wchan 直方图（终态）: $(wchan_hist "$PID")"

FAIL=0; REASON=""
if [ "$max_tick" -lt "$TICK_TARGET" ]; then
    FAIL=1; REASON="tick 未达目标（max=$max_tick < $TICK_TARGET）"
fi
if [ "$max_tick" -le 600 ]; then
    FAIL=1
    REASON="${REASON:+$REASON; }tick 未越过 600 悬崖（max=$max_tick）—— 疑似 tick%600 自死锁"
fi
if [ "$max_stall" -gt "$STALL_TOL" ]; then
    FAIL=1
    REASON="${REASON:+$REASON; }无进展时长 ${max_stall}s > 容忍 ${STALL_TOL}s（冻在 tick=$stall_at_tick）"
fi
# wchan 成排 futex 是「永久等锁」的旁证（不单独定案，但和上面一起报）
FUTEX_CNT="$(wchan_hist "$PID" | grep -oE 'futex_wait_queue\(([0-9]+)\)' | grep -oE '[0-9]+' | head -1)"
if [ -n "${FUTEX_CNT:-}" ] && [ "${FUTEX_CNT:-0}" -ge 3 ]; then
    echo "⚠️ 旁证：有 $FUTEX_CNT 个线程终态停在 futex_wait_queue（成排等锁）"
fi

echo ""
if [ "$FAIL" = "0" ]; then
    echo "LONGRUN-GUARD: PASS (max_tick=$max_tick > 600，持续增长，最长无进展 ${max_stall}s)"
    exit 0
else
    echo "LONGRUN-GUARD: FAIL ($REASON)"
    exit 1
fi
