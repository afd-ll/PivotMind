#!/usr/bin/env bash
# =============================================================================
# deploy/migrate-home-layout.sh —— 数据文件「旧扁平布局 → SSOT 布局」迁移
#
# 背景
#   v0.5.30 起，数据文件落点由路径 SSOT 决定，位置是 <home>/data/<name>；
#   而 v0.5.28 及更早的线上部署把 state / features / ... 直接放在 <home>/ 根下。
#   换上新二进制后，引擎找不到 <home>/data/<name>，会【新建空状态并正常启动、不报错】
#   = 玄枢失忆。v0.5.33 已加 fail-loud 门（gateway 硬拒启动），本脚本负责把旧文件
#   搬到位，让升级后的实例重新「认得」原来的脑子。
#
# 语义
#   **只搬不删、绝不覆盖**。对每个登记数据文件：
#     <home>/<name> 存在，且 <home>/data/<name> 不存在  ⇒ 同盘 mv（原子改名）
#     <home>/<name> 存在，且 <home>/data/<name> 也存在  ⇒ 跳过并告警（人工裁决）
#
# 安全
#   默认【只打印计划】；必须显式 --yes 才动手。检测到在用实例时【拒绝】运行。
#
# 用法
#   deploy/migrate-home-layout.sh [--home DIR] [--yes] [--dry-run]
#
#     --home DIR   数据根（默认 $PIVOTMIND_HOME，再退 $HOME/pivotmind）
#     --yes, -y    真正执行（默认仅演练）
#     --dry-run    显式演练（默认行为，用于脚本内自文档）
#     -h, --help   本帮助
#
# 典型流程
#   deploy/migrate-home-layout.sh --home /home/cx/pivotmind              # ① 先看计划
#   sudo systemctl stop pivotmind                                        # ② 停实例
#   deploy/migrate-home-layout.sh --home /home/cx/pivotmind --yes        # ③ 真搬
#   sudo systemctl start pivotmind                                       # ④ 起实例
#
# 回滚
#   把 <home>/data/<name> 搬回 <home>/<name> 即可 —— 本脚本只做同盘改名，不删任何数据。
# =============================================================================
set -euo pipefail

# ---- 登记数据文件名 ---------------------------------------------------------
# 必须与 src/pivotmind_paths.c 的 g_filename[PM_FILE_COUNT] 逐字一致（12 项）。
# 少一项 ⇒ 漏搬；多一项 ⇒ 误判。改这里必须同步改那里（SSOT 的代价）。
FILES=(
  pivotmind_state.dat
  brain_state.dat
  features.bin
  cross_edges.bin
  memory_seed.dat
  emergent_pos.bin
  pivotmind_config.json
  intent_base.bin
  pfe_strategy.bin
  pfe_workspace.bin
  pretrain_embeddings.bin
  gw_token
)

HOME_DIR="${PIVOTMIND_HOME:-${HOME:-}/pivotmind}"
ACT=0

usage() {
  cat <<'EOF'
deploy/migrate-home-layout.sh —— 数据文件「旧扁平布局 → SSOT 布局」迁移

用法:
  deploy/migrate-home-layout.sh [--home DIR] [--yes] [--dry-run]

  --home DIR   数据根（默认 $PIVOTMIND_HOME，再退 $HOME/pivotmind）
  --yes, -y    真正执行（默认仅演练）
  --dry-run    显式演练（默认行为）
  -h, --help   本帮助

语义: 只搬不删、绝不覆盖。仅搬 <home>/<name>，且 <home>/data/<name> 尚不存在者。
安全: 默认只打印计划；检测到在用实例时拒绝运行。
EOF
}

# ---- 参数解析 ---------------------------------------------------------------
while [ "$#" -gt 0 ]; do
  case "$1" in
    --home)
      if [ "$#" -lt 2 ]; then echo "错误：--home 缺少参数" >&2; exit 2; fi
      HOME_DIR="$2"; shift 2 ;;
    --home=*)  HOME_DIR="${1#*=}"; shift ;;
    --yes|-y)  ACT=1; shift ;;
    --dry-run) ACT=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *)         echo "错误：未知参数 '$1'" >&2; usage >&2; exit 2 ;;
  esac
done

# ---- 数据根合法性（与 pm_home() 同款约束：非空、绝对、不含 '~'、不是 '/'）----
if [ -z "$HOME_DIR" ]; then
  echo "错误：数据根为空（\$PIVOTMIND_HOME 与 \$HOME 都不可用？）" >&2; exit 2
fi
case "$HOME_DIR" in
  *"~"*) echo "错误：数据根不许含 '~'（SSOT 契约同款约束）：$HOME_DIR" >&2; exit 2 ;;
  /*)    : ;;
  *)     echo "错误：数据根必须是绝对路径：$HOME_DIR" >&2; exit 2 ;;
esac
if [ "$HOME_DIR" = "/" ]; then
  echo "错误：数据根不许是 /" >&2; exit 2
fi
# 规范化尾部斜杠（与 pm_trim_trailing_slash 一致：避免出现 "//"）
while [ "${HOME_DIR%/}" != "$HOME_DIR" ]; do HOME_DIR="${HOME_DIR%/}"; done
if [ ! -d "$HOME_DIR" ]; then
  echo "错误：数据根不存在或不是目录：$HOME_DIR" >&2; exit 2
fi

echo "数据根      : $HOME_DIR"
echo "目标数据目录: $HOME_DIR/data"
echo "模式        : $([ "$ACT" -eq 1 ] && echo '执行（--yes）' || echo '演练（默认，不改动任何文件）')"
echo ""

# ---- 在用实例检测（v0.5.33：无 pidfile 约定，只能走 /proc）------------------
# 两条判据：① 进程环境含 PIVOTMIND_HOME=<本数据根>；② 进程 cwd 就是本数据根。
live_pids() {
  local d pid tgt
  for d in /proc/[0-9]*; do
    [ -d "$d" ] || continue
    pid="${d#/proc/}"
    if [ -r "$d/environ" ]; then
      if tr '\0' '\n' < "$d/environ" 2>/dev/null | grep -qxF "PIVOTMIND_HOME=$HOME_DIR"; then
        echo "$pid"; continue
      fi
    fi
    if [ -e "$d/cwd" ]; then
      tgt="$(readlink -f "$d/cwd" 2>/dev/null || true)"
      if [ -n "$tgt" ] && [ "$tgt" = "$HOME_DIR" ]; then echo "$pid"; fi
    fi
  done
  return 0
}

PIDS="$(live_pids | tr '\n' ' ' | sed 's/ *$//')"
if [ -n "$PIDS" ]; then
  echo "✗ 拒绝运行：检测到疑似在用实例（PID: $PIDS）。" >&2
  echo "  边跑边搬数据 = 数据竞争。请先停掉它再重跑，例如：" >&2
  echo "      sudo systemctl stop pivotmind      # 或 kill $PIDS" >&2
  exit 3
fi

# ---- 生成计划 ---------------------------------------------------------------
PLAN=()
CONFLICT=()
for f in "${FILES[@]}"; do
  src="$HOME_DIR/$f"
  dst="$HOME_DIR/data/$f"
  if [ -e "$dst" ]; then
    if [ -e "$src" ]; then CONFLICT+=("$f"); fi
    continue
  fi
  if [ -e "$src" ]; then PLAN+=("$f"); fi
done

if [ "${#CONFLICT[@]}" -gt 0 ]; then
  echo "⚠ 两边都存在的文件（跳过，绝不覆盖，请人工裁决）："
  for f in "${CONFLICT[@]}"; do
    echo "    · $f"
    echo "        <home>/     : $(stat -c '%s B  %y' "$HOME_DIR/$f" 2>/dev/null || echo '?')"
    echo "        <home>/data/: $(stat -c '%s B  %y' "$HOME_DIR/data/$f" 2>/dev/null || echo '?')"
  done
  echo ""
fi

if [ "${#PLAN[@]}" -eq 0 ]; then
  echo "无待迁移文件 —— 数据根已是 SSOT 布局，或本就为空（全新部署）。"
  exit 0
fi

echo "待迁移 ${#PLAN[@]} 项（<home>/<name> → <home>/data/<name>）："
for f in "${PLAN[@]}"; do
  printf '    · %-24s %s\n' "$f" "$(stat -c '%s B' "$HOME_DIR/$f" 2>/dev/null || echo '?')"
done
echo ""

if [ "$ACT" -ne 1 ]; then
  echo "演练结束：未改动任何文件。确认无误后加 --yes 真正执行。"
  exit 0
fi

# ---- 执行 -------------------------------------------------------------------
mkdir -p "$HOME_DIR/data"

# 同盘性检查：跨文件系统时 mv 退化为「复制+删除」，不再是原子改名。
DEV_HOME="$(stat -c %d "$HOME_DIR" 2>/dev/null || echo '')"
DEV_DATA="$(stat -c %d "$HOME_DIR/data" 2>/dev/null || echo '')"
if [ -n "$DEV_HOME" ] && [ -n "$DEV_DATA" ] && [ "$DEV_HOME" != "$DEV_DATA" ]; then
  echo "⚠ 注意：<home> 与 <home>/data 不在同一文件系统（设备号 $DEV_HOME vs $DEV_DATA）。" >&2
  echo "  mv 将退化为「复制 + 删除」，不再是原子改名；请确保磁盘余量充足。" >&2
  echo "" >&2
fi

MOVED=0
FAILED=0
for f in "${PLAN[@]}"; do
  src="$HOME_DIR/$f"
  dst="$HOME_DIR/data/$f"
  if [ -e "$dst" ]; then
    echo "  ✗ 跳过（目标已存在，绝不覆盖）：$f" >&2
    FAILED=$((FAILED + 1)); continue
  fi
  if mv -n -- "$src" "$dst" 2>/dev/null; then
    if [ -e "$dst" ] && [ ! -e "$src" ]; then
      echo "  ✓ $f"
      MOVED=$((MOVED + 1))
    else
      echo "  ✗ $f —— mv 返回成功但落点不符（跨设备半途失败？）" >&2
      FAILED=$((FAILED + 1))
    fi
  else
    echo "  ✗ $f —— mv 失败（目标目录不可写？跨设备？）" >&2
    FAILED=$((FAILED + 1))
  fi
done

echo ""
echo "完成：成功 $MOVED 项，失败 $FAILED 项。"
if [ "$FAILED" -ne 0 ]; then
  echo "注意：有 $FAILED 项未迁移。若 pivotmind_state.dat 仍在旧位置，gateway 仍会拒绝启动。" >&2
  exit 1
fi
echo "回滚：把 <home>/data/<name> 搬回 <home>/<name> 即可（本脚本未删除任何数据）。"
