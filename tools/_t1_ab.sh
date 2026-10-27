#!/bin/bash
# 温度调度 A/B 采集：同一批输入 × N 次，把回复逐条落成 TSV。
# 用法: _t1_ab.sh <标签> <重复次数>
# 输出: /tmp/t1_<标签>.tsv   列: 序号 \t 输入 \t 回复(压成一行)
set -u
TAG="${1:-A}"
REP="${2:-3}"
T=$(cat /home/ubuntu/PivotMind/data/gw_token)
OUT="/tmp/t1_${TAG}.tsv"
: > "$OUT"

INPUTS=(
  "什么是爱"          # 熵最低档（确定=查询）
  "为什么要睡觉"       # 确定=解释
  "怎么学习编程"       # 确定=如何
  "你好啊"            # 含糊（闲聊/查询）
  "我想学英语"         # 最含糊
  "盐"
  "山中落雪"
  "电脑屏幕坏了"
  "今天天气不错我们去爬山吧"
  "人工智能是什么"
  "心理压力大怎么办"
  "猫和狗哪个好"
  "学习有什么用"
  "朋友吵架了"
  "介绍一下你自己"
  "时间过得真快"
  "为什么会下雨"
  "怎么才能早起"
  "我不开心"
  "这本书好看吗"
)

i=0
for rep in $(seq 1 "$REP"); do
  for m in "${INPUTS[@]}"; do
    i=$((i+1))
    R=$(curl -s -m 90 -X POST -H "X-Pivot-Token: $T" \
        -H 'Content-Type: application/json' \
        -d "{\"msg\":\"$m\"}" http://127.0.0.1:8080/chat)
    # 只取 reply 字段，去掉换行和制表符
    REPLY=$(printf '%s' "$R" | sed -n 's/.*"reply":"\(.*\)","nodes".*/\1/p' | tr '\n\t' '  ')
    [ -z "$REPLY" ] && REPLY="(PARSE_FAIL)$R"
    printf '%d\t%s\t%s\n' "$i" "$m" "$REPLY" >> "$OUT"
  done
done
echo "DONE tag=$TAG rep=$REP rows=$(wc -l < "$OUT")"
