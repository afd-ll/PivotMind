#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""N17 扩散账行不变式自检（G-T5 步骤 3，方案 §4.5）。

字段名固定，见 src/diffusion.c:1451-1460。
不变式：一跳 候选 == 点亮 + 强度落选；两跳同理（不许静默丢弃）。
用法：python3 n17_ledger_invariants.py <账行文件>
判据：bad == 0 且 lines > 0 -> N17 OK
"""
import re
import sys

pat = re.compile(r'一跳: 候选(\d+) 点亮(\d+) 强度落选(\d+).*?两跳: 候选(\d+) 点亮(\d+) 强度落选(\d+)')


def main(path):
    bad = 0
    n = 0
    for line in open(path, encoding='utf-8', errors='replace'):
        m = pat.search(line)
        if not m:
            print("FORMAT FAIL:", line.rstrip())
            bad += 1
            continue
        n += 1
        c1, l1, d1, c2, l2, d2 = map(int, m.groups())
        if c1 != l1 + d1:
            print("INVARIANT FAIL(一跳):", line.rstrip())
            bad += 1
        if c2 != l2 + d2:
            print("INVARIANT FAIL(两跳):", line.rstrip())
            bad += 1
    print(f"[n17] lines={n} bad={bad}")
    if bad == 0 and n > 0:
        print("N17 OK")
        return 0
    print("N17 RED")
    return 1


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
