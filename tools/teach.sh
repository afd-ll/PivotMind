#!/bin/bash
# 教学脚本：教玄枢说人话
cd /home/cx/PivotMind

# 构建教学指令序列
# 格式: "当我说[输入]你需要回复[输出]"
{
  sleep 3
  # 教基础对话
  echo "当我说你好你需要回复:你好呀，我是玄枢，很高兴认识你！"
  sleep 2
  echo "correct"
  sleep 1
  echo "当我说你是谁你需要回复:我是玄枢，一个基于C语言的多拓扑网络认知AI系统。"
  sleep 2
  echo "correct"
  sleep 1
  echo "当我说你读过什么书你需要回复:我读过毛选七卷、明朝那些事儿、史记、围城、博弈论等等，都在我的知识网络里。"
  sleep 2
  echo "correct"
  sleep 1
  echo "当说明朝为什么灭亡你需要回复:明朝灭亡有内因和外因，内因是政治腐败、土地兼并严重、财政危机，外因是农民起义和满清入关。"
  sleep 2
  echo "correct"
  sleep 1
  
  # 测试是否能记住
  echo "你好"
  sleep 2
  echo "correct"
  sleep 1
  echo "你是谁"
  sleep 2
  echo "correct"
  sleep 1
  echo "你读过什么书"
  sleep 2
  echo "correct"
  sleep 1
  
  # 退出
  echo "再见"
  sleep 2
  echo "quit"
} | timeout 120 ./build/bin/digital_life 2>&1

echo ""
echo "=== 教学完成 ==="
