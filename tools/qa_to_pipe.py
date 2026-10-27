#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""qa_to_pipe.py — 把 corpus/ 下各种 QA 格式归一成 `问|答` 单行格式

支持的结构：
  · `问|答` 纯文本（.txt / .conv 之外的 pipe 文件）—— 原样透传（仍做清洗）
  · JSONL（.jsonl）：每行一个对象
      - alpaca 系：instruction_zh/input_zh/output_zh（也兼容无 _zh 的英文键）
      - hc3  系：question + human_answers[] / chatgpt_answers[]
  · JSON 数组（.json）：
      - [["问","答"], ...]
      - [{"instruction","input","output"}, ...]

清洗（pipe 是行内格式，必须做）：
  · \\r \\n \\t → 空格；竖线 `|` → 空格（否则破坏字段）
  · 折叠连续空白、去首尾空白
  · 长度：问 1..300 字、答 1..800 字；超长截断到上限（不丢整条）
去重：按 (问, 答) 的 sha1 前 16 位去重。

用法：
  python3 qa_to_pipe.py --out /path/all_qa.txt [文件或目录 ...]
  不带文件参数时，默认吃 corpus/ 下全部 QA 源（不含 books_*）。
"""

import argparse
import hashlib
import io
import json
import os
import sys

QMAX, AMAX = 300, 800


def clean(s):
    if not isinstance(s, str):
        s = "" if s is None else str(s)
    for ch in ("\r", "\n", "\t", "\u3000", "|"):
        s = s.replace(ch, " ")
    s = " ".join(s.split())
    return s.strip()


def norm_record(rec):
    """把一条记录归一成 (q, a)；无法识别返回 None。"""
    if isinstance(rec, (list, tuple)) and len(rec) >= 2:
        return clean(rec[0]), clean(rec[1])
    if not isinstance(rec, dict):
        return None

    # alpaca 系（优先中文键）
    if any(k in rec for k in ("instruction_zh", "instruction", "input_zh", "output_zh", "output")):
        q = rec.get("instruction_zh") or rec.get("instruction") or ""
        inp = rec.get("input_zh") or rec.get("input") or ""
        a = rec.get("output_zh") or rec.get("output") or ""
        q = clean(q)
        inp = clean(inp)
        if inp and inp != q:
            q = (q + " " + inp).strip()
        return q, clean(a)

    # hc3 系
    if "question" in rec:
        q = clean(rec.get("question"))
        cands = rec.get("human_answers") or rec.get("chatgpt_answers") or []
        if isinstance(cands, (list, tuple)) and cands:
            return q, clean(cands[0])
        if isinstance(cands, str):
            return q, clean(cands)
        return None
    return None


def emit(out_f, seen, stats, src, q, a):
    if not q or not a:
        stats[src]["empty"] += 1
        return
    if len(q) > QMAX:
        q = q[:QMAX].strip()
    if len(a) > AMAX:
        a = a[:AMAX].strip()
    if not q or not a:
        stats[src]["empty"] += 1
        return
    h = hashlib.sha1((q + "\x00" + a).encode("utf-8")).hexdigest()[:16]
    if h in seen:
        stats[src]["dup"] += 1
        return
    seen.add(h)
    out_f.write("%s|%s\n" % (q, a))
    stats[src]["ok"] += 1


def process_file(path, out_f, seen, stats):
    src = os.path.basename(path)
    stats[src] = {"ok": 0, "dup": 0, "empty": 0, "bad": 0}
    lower = path.lower()
    try:
        if lower.endswith(".jsonl"):
            with io.open(path, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        rec = json.loads(line)
                    except Exception:
                        stats[src]["bad"] += 1
                        continue
                    r = norm_record(rec)
                    if not r:
                        stats[src]["bad"] += 1
                        continue
                    emit(out_f, seen, stats, src, r[0], r[1])
        elif lower.endswith(".json"):
            with io.open(path, "r", encoding="utf-8", errors="replace") as f:
                data = json.load(f)
            if isinstance(data, dict):
                data = [data]
            for rec in data:
                r = norm_record(rec)
                if not r:
                    stats[src]["bad"] += 1
                    continue
                emit(out_f, seen, stats, src, r[0], r[1])
        else:
            # 纯文本：优先按 `问|答` 解析，否则整行当问、无答 ⇒ 跳过
            with io.open(path, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    line = line.rstrip("\n").rstrip("\r")
                    if not line.strip():
                        continue
                    if "|" in line:
                        q, a = line.split("|", 1)
                        emit(out_f, seen, stats, src, clean(q), clean(a))
                    else:
                        stats[src]["bad"] += 1
    except Exception as e:
        print("  !! %s 失败：%s" % (path, e), file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("src", nargs="*")
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    corpus_dir = os.path.dirname(here)

    if args.src:
        files = []
        for s in args.src:
            if os.path.isdir(s):
                for root, _, names in os.walk(s):
                    for n in sorted(names):
                        if n.lower().endswith((".json", ".jsonl", ".txt")):
                            files.append(os.path.join(root, n))
            else:
                files.append(s)
    else:
        files = [
            os.path.join(corpus_dir, "xiaohuangji_pipe.txt"),
            os.path.join(corpus_dir, "xhj_5k.txt"),
        ]
        for sub in ("qa", "health_corpus"):
            d = os.path.join(corpus_dir, sub)
            for root, _, names in os.walk(d):
                for n in sorted(names):
                    if n.lower().endswith((".json", ".jsonl")):
                        files.append(os.path.join(root, n))

    seen = set()
    stats = {}
    total = 0
    with io.open(args.out, "w", encoding="utf-8", newline="\n") as out_f:
        for p in files:
            before = sum(v["ok"] for v in stats.values())
            process_file(p, out_f, seen, stats)
            after = sum(v["ok"] for v in stats.values())
            print("  %-40s +%d" % (os.path.basename(p), after - before))

    print("\n=== 汇总 ===")
    for src, v in stats.items():
        print("  %-34s 收 %7d · 重复 %6d · 空 %6d · 坏 %5d"
              % (src[:34], v["ok"], v["dup"], v["empty"], v["bad"]))
    total = sum(v["ok"] for v in stats.values())
    size = os.path.getsize(args.out) if os.path.exists(args.out) else 0
    print("  合计 %d 对 · 输出 %.1f MB · %s" % (total, size / 1048576.0, args.out))


if __name__ == "__main__":
    main()
