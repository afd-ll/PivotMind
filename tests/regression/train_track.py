#!/usr/bin/env python3
"""train_track.py — 训练效果量化追踪

逐轮训练 → 测试固定问题集 → 记录回复质量指标 → 输出 CSV 趋势

Usage:
    python3 train_track.py [--rounds 10] [--output track.csv] [--port PORT]

Dependencies: helpers.py (HTTP client + metrics)
"""

import sys
import os
import json
import time
import csv
import argparse
import signal

sys.path.insert(0, os.path.dirname(__file__))
from helpers import (
    start_gateway, stop_gateway, chat, status, count_fw,
    PROJECT_ROOT
)

TRACK_QUESTIONS = [
    ("你好", "zh"),
    ("什么是学习", "zh"),
    ("人工智能", "zh"),
    ("科学是什么", "zh"),
    ("hello", "en"),
    ("what is knowledge", "en"),
    ("how does learning work", "en"),
    ("machine learning", "en"),
]


def learn(port, text):
    """POST /learn with text."""
    from urllib.request import urlopen, Request
    data = json.dumps({"msg": text}).encode("utf-8")
    req = Request(f"http://127.0.0.1:{port}/learn", data=data,
                  headers={"Content-Type": "application/json"})
    try:
        with urlopen(req, timeout=15) as r:
            return r.status
    except Exception as e:
        print(f"  [learn error] {e}")
        return -1


def get_total_nodes(port):
    """Get total node count from /status."""
    s = status(port)
    if isinstance(s, dict):
        return s.get("total_nodes", s.get("nodes", s.get("node_count", 0)))
    return 0


def run_one_round(port, round_num):
    """Test all questions and return metrics dict."""
    results = {
        "round": round_num,
        "total_nodes": get_total_nodes(port),
        "questions": len(TRACK_QUESTIONS),
        "replies": 0,
        "empty_replies": 0,
        "total_len": 0,
        "total_en_fw": 0,
        "total_zh_fw": 0,
        "total_words": 0,
    }

    for q, lang in TRACK_QUESTIONS:
        reply = chat(port, q)
        reply_str = str(reply).strip() if reply else ""

        is_empty = not reply_str or reply_str in ("(无回应)", "(no response)", "null", "None")
        if is_empty:
            results["empty_replies"] += 1
        else:
            results["replies"] += 1
            results["total_len"] += len(reply_str)
            en_fw, zh_fw = count_fw(reply_str)
            results["total_en_fw"] += en_fw
            results["total_zh_fw"] += zh_fw
            words = reply_str.split()
            results["total_words"] += len(words) if words else 1

    return results


def load_qa_pairs(filename="data/hermes_knowledge_base.json", max_pairs=50):
    """Load QA pairs from JSON file."""
    path = os.path.join(PROJECT_ROOT, filename)
    if not os.path.exists(path):
        path = os.path.join(PROJECT_ROOT, "data", "qa_zero.json")
    if not os.path.exists(path):
        # Fallback: inline pairs
        return [("什么是学习", "学习是获取知识的过程"),
                ("什么是人工智能", "人工智能是模拟人类智能的技术"),
                ("什么是科学", "科学是探索自然规律的方法")]

    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)

    pairs = []
    if isinstance(data, list):
        for item in data[:max_pairs]:
            if isinstance(item, list) and len(item) >= 2:
                pairs.append((str(item[0]), str(item[1])))
            elif isinstance(item, dict):
                q = item.get("question", item.get("q", item.get("input", "")))
                a = item.get("answer", item.get("a", item.get("output", "")))
                if q and a:
                    pairs.append((str(q), str(a)))
    return pairs


def write_csv(filename, rows):
    """Write list of dicts to CSV."""
    if not rows:
        return
    with open(filename, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=rows[0].keys())
        w.writeheader()
        w.writerows(rows)


def main():
    parser = argparse.ArgumentParser(description="PivotMind Training Quality Tracker")
    parser.add_argument("--rounds", type=int, default=5, help="Training rounds (default: 5)")
    parser.add_argument("--pairs-per-round", type=int, default=10, help="QA pairs per round (default: 10)")
    parser.add_argument("--output", default="train_track.csv", help="Output CSV path")
    parser.add_argument("--port", type=int, default=0, help="Gateway port (0=auto)")
    args = parser.parse_args()

    print("=" * 60)
    print("PivotMind Training Quality Tracker")
    print(f"Rounds: {args.rounds}, Pairs/round: {args.pairs_per_round}")
    print(f"Test questions: {len(TRACK_QUESTIONS)}")
    print("=" * 60)

    # Load QA pairs
    pairs = load_qa_pairs(max_pairs=args.pairs_per_round * args.rounds)
    print(f"\nLoaded {len(pairs)} QA pairs")
    if not pairs:
        print("ERROR: No QA pairs found")
        return 1

    # Start gateway
    binary = os.path.join(PROJECT_ROOT, "build", "bin", "pivotmind_gateway")
    port = args.port if args.port > 0 else None
    try:
        proc, port = start_gateway(binary, port, timeout=30)
    except Exception as e:
        print(f"ERROR: Gateway start failed: {e}")
        return 1

    print(f"Gateway started on port {port}\n")

    signal.signal(signal.SIGINT, lambda *_: stop_gateway(proc))

    all_rows = []

    try:
        # Round 0: baseline (before any training)
        print("── Round 0 (baseline, no training) ──")
        r0 = run_one_round(port, 0)
        all_rows.append(r0)
        print(f"  Nodes: {r0['total_nodes']}, Replies: {r0['replies']}/{r0['questions']}, "
              f"Avg len: {r0['total_len']/max(r0['replies'],1):.0f}")

        # Training rounds
        pair_idx = 0
        for round_num in range(1, args.rounds + 1):
            print(f"\n── Round {round_num}: training {args.pairs_per_round} pairs ──")

            trained = 0
            for _ in range(args.pairs_per_round):
                if pair_idx >= len(pairs):
                    break
                q, a = pairs[pair_idx]
                # Learn each pair as two separate API calls
                learn(port, f"{q} {a}")
                pair_idx += 1
                trained += 1
                time.sleep(0.05)  # rate limit

            print(f"  Trained: {trained} pairs")

            # Test after this round
            time.sleep(0.5)  # let system settle
            metrics = run_one_round(port, round_num)
            all_rows.append(metrics)

            reply_rate = metrics["replies"] / max(metrics["questions"], 1)
            avg_len = metrics["total_len"] / max(metrics["replies"], 1)
            fw_ratio = (metrics["total_en_fw"] + metrics["total_zh_fw"]) / max(metrics["total_words"], 1)

            print(f"  Nodes: {metrics['total_nodes']}, "
                  f"Reply rate: {reply_rate:.0%}, "
                  f"Avg len: {avg_len:.0f}, "
                  f"FW ratio: {fw_ratio:.1%}")

        # Save CSV
        write_csv(args.output, all_rows)
        print(f"\nResults saved to {args.output}")

        # Summary
        if len(all_rows) >= 2:
            first = all_rows[0]
            last = all_rows[-1]
            print("\n── Trend Summary ──")
            print(f"  Nodes:  {first['total_nodes']} → {last['total_nodes']} "
                  f"(+{last['total_nodes']-first['total_nodes']})")
            print(f"  Reply:  {first['replies']}/{first['questions']} → "
                  f"{last['replies']}/{last['questions']}")

    finally:
        stop_gateway(proc)

    return 0


if __name__ == "__main__":
    sys.exit(main())
