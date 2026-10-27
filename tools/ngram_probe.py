#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ngram_probe.py — 量语料里 3/4 字候选词有多少（不建库，只统计）

口径：
  · 只取中文单字（3 字节 UTF-8）连续串内的 n-gram，遇非汉字即断
  · 计数 >= MINC 才算候选
  · PMI 近似：log2( c(ABC) * T^2 / (c(AB)*c(BC)) ) —— 只用相邻对做分母，
    等价于"这个三字串比它的两个相邻对更紧密多少"
  · 剪枝：字典超过 2M 时丢掉 count==1 的项，控制内存

用法: python3 ngram_probe.py <pipe_file> [MINC]
"""
import io
import math
import os
import sys
from collections import Counter

MINC = int(sys.argv[2]) if len(sys.argv) > 2 else 20
PATH = sys.argv[1]
PRUNE_AT = 2000000

def hanzi(ch):
    o = ord(ch)
    return 0x4E00 <= o <= 0x9FFF

def main():
    tri = Counter()
    four = Counter()
    bi = Counter()
    tot = 0
    lines = 0

    with io.open(PATH, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            lines += 1
            for seg in line.replace("|", " ").split():
                if len(seg) < 3:
                    continue
                run = []
                for ch in seg + "\x00":
                    if hanzi(ch):
                        run.append(ch)
                        continue
                    if len(run) >= 2:
                        for i in range(len(run) - 1):
                            bi[run[i] + run[i + 1]] += 1
                            tot += 1
                        for i in range(len(run) - 2):
                            tri[run[i] + run[i + 1] + run[i + 2]] += 1
                        for i in range(len(run) - 3):
                            four[run[i] + run[i + 1] + run[i + 2] + run[i + 3]] += 1
                    run = []
            if lines % 100000 == 0:
                print("  已读 %d 行 · tri=%d four=%d bi=%d" % (lines, len(tri), len(four), len(bi)),
                      flush=True)
            if len(tri) > PRUNE_AT:
                for k in [k for k, v in tri.items() if v == 1]:
                    del tri[k]
            if len(four) > PRUNE_AT:
                for k in [k for k, v in four.items() if v == 1]:
                    del four[k]

    print("行数 %d · 二元 %d 种 / %d 次 · 三元 %d 种 · 四元 %d 种"
          % (lines, len(bi), tot, len(tri), len(four)))

    for name, table, n in (("三字", tri, 3), ("四字", four, 4)):
        cand = [(k, v) for k, v in table.items() if v >= MINC]
        cand.sort(key=lambda x: -x[1])
        print("\n=== %s：计数>=%d 的候选 %d 个（按次数前 20）===" % (name, MINC, len(cand)))
        for k, v in cand[:20]:
            print("  %s  %d" % (k, v))
        # PMI 近似：走相邻对做分母
        good = 0
        for k, v in cand:
            if n == 3:
                a, b = bi[k[0:2]], bi[k[1:3]]
            else:
                a, b = bi[k[0:2]], bi[k[2:4]]
            if not a or not b:
                continue
            pmi = math.log2(v * max(tot, 1) / (a * b))
            if pmi >= 2.5:
                good += 1
        print("  其中 PMI近似>=2.5 的 %d 个（分母口径：首对×尾对，与重训器阶段二不同，仅作量级参考）"
              % good)

if __name__ == "__main__":
    if not os.path.exists(PATH):
        print("找不到 %s" % PATH)
        sys.exit(2)
    main()
