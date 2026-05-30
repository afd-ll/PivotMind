#!/usr/bin/env python3
"""
合并 dialog_lccc_flt.jsonl 到 hermes_knowledge_base.json
格式:
  source: [多轮对话] → 每轮提取连续对话对作为 QA
  target: [[q, a], [q, a], ...]
"""
import json
import sys
import os

SMB_LCCC = "/mnt/work/训练数据/dialog_lccc_flt.jsonl"
EXISTING_QA = "data/hermes_knowledge_base.json"
OUTPUT_QA = "data/hermes_knowledge_base.json"  # overwrite

def load_existing(path):
    with open(path, 'r', encoding='utf-8') as f:
        return json.load(f)

def load_lccc_dialogs(path):
    """Load multi-turn dialogs from LCCC JSONL, extract QA pairs from consecutive turns."""
    qa_pairs = []
    with open(path, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            dialog = json.loads(line)
            if not isinstance(dialog, list) or len(dialog) < 2:
                continue
            # Extract consecutive turn pairs: (turn[i], turn[i+1])
            for i in range(len(dialog) - 1):
                q = dialog[i].strip()
                a = dialog[i+1].strip()
                if q and a:
                    qa_pairs.append([q, a])
    return qa_pairs

def main():
    print(f"[1/3] 加载现有 QA ({EXISTING_QA})...")
    existing = load_existing(EXISTING_QA)
    print(f"  现有: {len(existing)} 条")

    print(f"[2/3] 解析 LCCC 多轮对话 ({SMB_LCCC})...")
    lccc_qas = load_lccc_dialogs(SMB_LCCC)
    print(f"  LCCC 提取: {len(lccc_qas)} 条 QA 对")

    print(f"[3/3] 合并 & 去重...")
    # 用 set 去重 (q, a) 元组
    seen = set()
    merged = []
    for qa in existing:
        key = (qa[0], qa[1])
        if key not in seen:
            seen.add(key)
            merged.append(qa)
    for qa in lccc_qas:
        key = (qa[0], qa[1])
        if key not in seen:
            seen.add(key)
            merged.append(qa)

    print(f"  合并后: {len(merged)} 条 (去重 {len(existing) + len(lccc_qas) - len(merged)} 条重复)")

    print(f"  写入 {OUTPUT_QA}...")
    with open(OUTPUT_QA, 'w', encoding='utf-8') as f:
        json.dump(merged, f, ensure_ascii=False, indent=None)
    print(f"  ✓ 完成! 文件大小: {os.path.getsize(OUTPUT_QA) / 1024 / 1024:.1f} MB")

if __name__ == "__main__":
    main()
