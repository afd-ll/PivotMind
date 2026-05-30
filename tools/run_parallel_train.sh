#!/bin/bash
# run_parallel_train.sh — 在 Dell G15 上启动 4 路并行训练
# 用法: 在 Dell 上的 MSYS2 bash 中执行
#
# 流程:
#   1. 把 data.json 拆成 4 份 (data_part_0.json ~ data_part_3.json)
#   2. 同时启动 4 个 batch_learn_new.exe，各从空拓扑开始
#   3. 等所有进程结束
#   4. 用 merge_states.py 合并成 final_state.dat

DATA_FILE="data.json"
STATE_PREFIX="state_p"
EPOCHS=100
PARTS=4

echo "╔═══════════════════════════════════════════╗"
echo "║  并行训练启动脚本                          ║"
echo "║  数据: $DATA_FILE                        ║"
echo "║  轮数: $EPOCHS                           ║"
echo "║  并行: $PARTS 路                          ║"
echo "╚═══════════════════════════════════════════╝"
echo ""

# --- 1. 检查基础文件 ---
EXE="batch_learn_new.exe"
if [ ! -f "$EXE" ]; then
    # 试试旧版名
    EXE="batch_learn_new"
    if [ ! -f "$EXE" ]; then
        echo "✗ 找不到 batch_learn_new[.exe]"
        exit 1
    fi
fi
echo "✓ 可执行文件: $EXE"

if [ ! -f "$DATA_FILE" ]; then
    echo "✗ 找不到 $DATA_FILE"
    exit 1
fi
echo "✓ 数据文件: $DATA_FILE"

# --- 2. 拆分数据 ---
echo ""
echo "[1/4] 拆分数据..."
TOTAL_LINES=$(python3 -c "
import json
with open('$DATA_FILE', 'r', encoding='utf-8') as f:
    data = json.load(f)
print(len(data))
")
echo "  总 QA 数: $TOTAL_LINES"

python3 -c "
import json, math

with open('$DATA_FILE', 'r', encoding='utf-8') as f:
    data = json.load(f)

part_size = math.ceil(len(data) / $PARTS)
for p in range($PARTS):
    start = p * part_size
    end = min((p + 1) * part_size, len(data))
    part = data[start:end]
    with open(f'data_part_{p}.json', 'w', encoding='utf-8') as f:
        json.dump(part, f, ensure_ascii=False, indent=None)
    print(f'  分区 {p}: {len(part)} 条 QA → data_part_{p}.json')
"

echo ""

# --- 3. 并行启动训练 ---
echo "[2/4] 启动 $PARTS 路训练..."
echo ""

PIDS=()
for p in $(seq 0 $((PARTS - 1))); do
    STATE_OUT="${STATE_PREFIX}${p}.dat"
    LOG_OUT="train_p${p}.log"
    echo "  进程 $p: ./$EXE $STATE_OUT data_part_${p}.json $EPOCHS > $LOG_OUT 2>&1"
    ./"$EXE" "$STATE_OUT" "data_part_${p}.json" "$EPOCHS" > "$LOG_OUT" 2>&1 &
    PIDS[$p]=$!
    echo "    PID: ${PIDS[$p]}"
done

echo ""
echo "  所有进程已在后台启动。"
echo "  日志: train_p0.log ~ train_p$(($PARTS - 1)).log"
echo "  状态: state_p0.dat ~ state_p$(($PARTS - 1)).dat"
echo ""

# --- 4. 等待 ---
echo "[3/4] 等待所有进程完成..."
echo ""

start_time=$(date +%s)
while true; do
    all_done=true
    running=0
    for p in $(seq 0 $((PARTS - 1))); do
        if kill -0 ${PIDS[$p]} 2>/dev/null; then
            all_done=false
            running=$((running + 1))
        fi
    done

    elapsed=$(( $(date +%s) - start_time ))
    elapsed_min=$(( elapsed / 60 ))
    elapsed_sec=$(( elapsed % 60 ))

    echo -ne "\r  运行中: $running/$PARTS  耗时: ${elapsed_min}m${elapsed_sec}s   "

    if $all_done; then
        echo ""
        echo ""
        echo "  ✓ 所有进程已完成！"
        break
    fi

    sleep 30
done

total_time=$(( $(date +%s) - start_time ))
total_min=$(( total_time / 60 ))
echo "  总耗时: ${total_min} 分钟"

# --- 5. 检查退出码 ---
echo ""
echo "  检查进程退出状态..."
all_ok=true
for p in $(seq 0 $((PARTS - 1))); do
    wait ${PIDS[$p]}
    rc=$?
    if [ $rc -ne 0 ]; then
        echo "  ✗ 进程 $p 异常退出 (code=$rc)"
        all_ok=false
    else
        echo "  ✓ 进程 $p 正常完成"
    fi
done

if ! $all_ok; then
    echo ""
    echo "✗ 部分进程异常退出，跳过合并"
    exit 1
fi

# --- 6. 合并 ---
echo ""
echo "[4/4] 合并状态文件..."

STATE_FILES=""
for p in $(seq 0 $((PARTS - 1))); do
    STATE_FILES="$STATE_FILES ${STATE_PREFIX}${p}.dat"
done

python3 tools/merge_states.py $STATE_FILES final_state.dat

echo ""

# --- 7. 摘要 ---
echo "╔═══════════════════════════════════════════╗"
echo "║  并行训练完成！                            ║"
echo "╠═══════════════════════════════════════════╣"
echo "║  轮数: $EPOCHS × $PARTS 路                    ║"
echo "║  耗时: ${total_min} 分钟                     ║"
echo "║  最终状态: final_state.dat                  ║"
echo "║  各分区日志: train_p0~3.log                ║"
echo "╚═══════════════════════════════════════════╝"
