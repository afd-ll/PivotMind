#!/usr/bin/env bash
# PivotMind 溯智系统 - 演示脚本 v0.1
# 用法: bash demo.sh

set -e
cd "$(dirname "$0")"

echo "╔═══════════════════════════════════════════════════════════════╗"
echo "║          PivotMind 溯智系统 - 交互演示 v0.1              ║"
echo "║          纯C实现的认知AI框架，持续在线学习               ║"
echo "╚═══════════════════════════════════════════════════════════════╝"
echo ""

# 1. 编译检查
if [ ! -f build/bin/digital_life ]; then
    echo "[1/3] 编译项目..."
    make linux -j1 2>&1 | tail -1
    echo "  ✓ 编译完成"
else
    echo "[1/3] 项目已编译"
fi
echo ""

# 2. 快速启动演示
echo "[2/3] 启动溯智系统（5秒后自动演示对话）..."
echo ""

# 启动系统并自动对话
{
    # 先让系统初始化
    sleep 2
    
    # 演示对话
    echo "你好"
    sleep 1
    echo ""
    sleep 1
    
    echo "什么是神经网络"
    sleep 1
    echo ""
    sleep 1
    
    echo "机器学习有什么应用"
    sleep 1
    echo ""
    sleep 1
    
    echo "树莓派能做什么"
    sleep 1
    echo ""
    sleep 1
    
    echo "quit"
    sleep 1
    echo ""
} | timeout 30 ./build/bin/digital_life 2>&1 || true

echo ""
echo "[3/3] 演示完成"
echo ""
echo "你可以手动运行:"
echo "  ./build/bin/digital_life"
echo "然后开始对话。每次对话系统都在在线学习。"
echo ""
echo "项目信息:"
echo "  https://github.com/afd-ll/PivotMind"
echo ""
