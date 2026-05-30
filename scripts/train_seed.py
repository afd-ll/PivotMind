#!/usr/bin/env python3
"""
PivotMind v0.2 种子训练脚本 v2
更稳定的 pexpect 交互版本
"""

import json
import os
import sys
import time
import pexpect

PROJECT_DIR = "/mnt/work/玄枢-pivotmind"
BINARY_PATH = os.path.join(PROJECT_DIR, "build/bin/digital_life")
KB_PATH = os.path.join(PROJECT_DIR, "data/knowledge_base.json")
BATCH_LEARN = 200
BATCH_LOG = 100
INPUT_TIMEOUT = 180


def main():
    print("=== PivotMind v0.2 种子训练 v2 ===")
    print(f"项目目录: {PROJECT_DIR}")

    if not os.path.exists(KB_PATH):
        print(f"错误: 找不到 {KB_PATH}")
        sys.exit(1)

    with open(KB_PATH, "r", encoding="utf-8") as f:
        qa_pairs = json.load(f)

    total = len(qa_pairs)
    print(f"已加载 {total} 条问答对")

    # 启动
    print(f"\n启动数字生命系统...")
    child = pexpect.spawn(
        BINARY_PATH,
        cwd=PROJECT_DIR,
        encoding='utf-8',
        codec_errors='replace',
        timeout=INPUT_TIMEOUT,
        echo=False,
    )

    # 等待启动完成，输出到屏幕方便观察
    line = child.readline  # 使用 readline 更稳定
    # 跳过启动信息，直到看到 "你:" 提示
    while True:
        try:
            raw = child.read_nonblocking(size=4096, timeout=STARTUP_TIMEOUT)
            # 等待 1 秒再检查
            time.sleep(0.5)
            # 如果缓冲区有 "你:" 算就绪
            if "你:" in child.before:
                break
        except:
            break

    print("系统启动成功")
    print(f"\n开始训练 {total} 条数据...")

    success = 0
    start_time = time.time()

    for i, (question, answer) in enumerate(qa_pairs):
        try:
            # 直接发送教学语句（不等待提示符，用固定间隔）
            teaching = f'当有人问{question}时，你需要回复{answer}'
            child.sendline(teaching)

            # 等待一段时间让系统处理
            waited = 0
            found_feedback = False
            while waited < INPUT_TIMEOUT:
                try:
                    idx = child.expect_exact(["你的评价", "直接回车跳过", "你:"], timeout=5)
                    if idx < 2:  # 找到了评价提示
                        child.sendline("")  # 回车跳过
                        found_feedback = True
                        break
                    elif idx == 2:  # 又看到了"你:"，可能错过了评价
                        break
                except pexpect.TIMEOUT:
                    waited += 5
                    continue

            if not found_feedback:
                child.sendline("")

            success += 1

            # 批次学习
            if (i + 1) % BATCH_LEARN == 0:
                elapsed = time.time() - start_time
                rate = (i + 1) / elapsed * 60 if elapsed > 0 else 0
                print(f"\n  [{i+1}/{total}] {rate:.0f}条/分 — 发送 learn...")
                child.sendline("learn")
                time.sleep(3)
                try:
                    child.read_nonblocking(size=65536, timeout=2)
                except:
                    pass

            # 进度
            if (i + 1) % BATCH_LOG == 0:
                elapsed = time.time() - start_time
                rate = (i + 1) / elapsed * 60 if elapsed > 0 else 0
                print(f"  [{i+1}/{total}] {rate:.0f}条/分")

        except EOFError:
            print(f"\n  [EOF] 第 {i+1} 条进程退出")
            break
        except Exception as e:
            print(f"\n  [!!] 第 {i+1} 条: {e}")
            try:
                child.sendline("")
                time.sleep(2)
            except:
                break

    # 最终学习 + 退出
    elapsed = time.time() - start_time
    print(f"\n训练完成 ({success}/{total})")
    print(f"耗时: {elapsed:.0f}s ({elapsed/60:.1f}min)")

    # 发几次 learn
    for r in range(3):
        child.sendline("learn")
        time.sleep(5)
        try:
            child.read_nonblocking(size=65536, timeout=3)
        except:
            pass

    # 查看统计
    child.sendline("stats")
    time.sleep(2)

    # quit
    print("退出保存...")
    child.sendline("quit")
    time.sleep(3)

    state_file = os.path.join(PROJECT_DIR, "pivotmind_state.dat")
    if os.path.exists(state_file):
        size = os.path.getsize(state_file)
        print(f"\n✓ 模型文件: {state_file} ({size:,} 字节)")
    else:
        print(f"\n× 未生成模型文件")


if __name__ == "__main__":
    STARTUP_TIMEOUT = 60
    main()
