#!/usr/bin/env python3
"""快速测试：用 pexpect 驱动 digital_life 跑几条教学数据"""
import json
import os
import sys
import time
import pexpect

PROJECT_DIR = "/mnt/work/玄枢-pivotmind"
BINARY_PATH = os.path.join(PROJECT_DIR, "build/bin/digital_life")
KB_PATH = os.path.join(PROJECT_DIR, "data/knowledge_base.json")

with open(KB_PATH, "r", encoding="utf-8") as f:
    qa = json.load(f)

# 只测前 5 条
test_pairs = qa[:5]

child = pexpect.spawn(
    BINARY_PATH,
    cwd=PROJECT_DIR,
    encoding='utf-8',
    codec_errors='replace',
    timeout=30,
)
child.logfile = sys.stdout  # 打印所有输出

print("=== 启动 ===")
try:
    child.expect("你:", timeout=15)
    print("\n=== 系统就绪 ===")
except pexpect.TIMEOUT:
    print("启动超时")
    child.close()
    sys.exit(1)

for i, (q, a) in enumerate(test_pairs):
    teaching = f"当有人问'{q}'时，你需要回复'{a}'"
    print(f"\n=== 教学 [{i+1}/5] ===")
    print(f"发送: {teaching[:60]}...")

    child.sendline(teaching)
    time.sleep(2)

    # 看输出到哪了
    try:
        idx = child.expect_exact(["评价", "你:"], timeout=15)
        if idx == 0:  # 评价提示
            child.sendline("")
            print("  → 跳过评价")
    except pexpect.TIMEOUT:
        print(f"  → 超时")
    except pexpect.EOF:
        print("  → 进程退出")
        break

print("\n=== 退出 ===")
child.sendline("quit")
try:
    child.expect(pexpect.EOF, timeout=5)
except:
    pass

state_file = os.path.join(PROJECT_DIR, "pivotmind_state.dat")
if os.path.exists(state_file):
    print(f"\n✓ 模型已保存: {state_file}")
else:
    print(f"\n× 未生成模型文件")
