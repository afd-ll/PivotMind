#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pair_gate_probe.py — 直接读 charpairs.bin，按重训器阶段二【同一套判据】算：
   有多少二元对能通过 count>=20 / PMI>=2.5 / min(左熵,右熵)>=2.0

为什么要它：重训器里有两个上限（word_prob 的 600k 建索引上限、晋升缓冲的 400k），
会把候选抽样掉。这里全量算一遍，得到"理论上能晋升多少"的真值。

文件格式（charpair.c v3）：
  magic "PMCP"(u32) · version(u32=3|2) · count(u32)
  逐条：u8 len_a · a · u8 len_b · b · u32 count
"""
import io
import math
import struct
import sys
from collections import defaultdict

MINC = int(sys.argv[2]) if len(sys.argv) > 2 else 20
MINPMI = float(sys.argv[3]) if len(sys.argv) > 3 else 2.5
MINENT = float(sys.argv[4]) if len(sys.argv) > 4 else 2.0
PATH = sys.argv[1]

def main():
    data = io.open(PATH, "rb").read()
    magic, ver, n = struct.unpack_from("<III", data, 0)
    assert magic == 0x504D4350, "magic 不对"
    pos = 12
    pairs = []
    for _ in range(n):
        la = data[pos]; pos += 1
        a = data[pos:pos + la].decode("utf-8", "replace"); pos += la
        lb = data[pos]; pos += 1
        b = data[pos:pos + lb].decode("utf-8", "replace"); pos += lb
        c, = struct.unpack_from("<I", data, pos); pos += 4
        pairs.append((a, b, c))

    total = sum(c for _, _, c in pairs)
    out_sum = defaultdict(int)     # 作为左成员
    left = defaultdict(lambda: defaultdict(int))   # b 的前驱分布
    right = defaultdict(lambda: defaultdict(int))  # a 的后继分布
    for a, b, c in pairs:
        out_sum[a] += c
        left[b][a] += c
        right[a][b] += c

    def ent(dist):
        t = sum(dist.values())
        if not t:
            return 0.0
        h = 0.0
        for v in dist.values():
            p = v / t
            h -= p * math.log2(p)
        return h

    ent_cache = {}
    def ent_l(x):
        k = ("L", x)
        if k not in ent_cache:
            ent_cache[k] = ent(left.get(x, {}))
        return ent_cache[k]
    def ent_r(x):
        k = ("R", x)
        if k not in ent_cache:
            ent_cache[k] = ent(right.get(x, {}))
        return ent_cache[k]

    n_all = len(pairs)
    n_cnt = 0
    n_pmi = 0
    n_ent = 0
    good = []
    for a, b, c in pairs:
        if c < MINC:
            continue
        n_cnt += 1
        oa, ob = out_sum.get(a, 0), out_sum.get(b, 0)
        if not oa or not ob:
            continue
        pmi = math.log2((c / total) / ((oa / total) * (ob / total)))
        if pmi < MINPMI:
            continue
        n_pmi += 1
        mn = min(ent_l(a), ent_r(b))
        if mn < MINENT:
            continue
        n_ent += 1
        good.append((a + b, c, pmi, mn))

    print("charpairs: %d 对 · 计数总和 %d" % (n_all, total))
    print("① count>=%d          : %d" % (MINC, n_cnt))
    print("② +PMI>=%.1f        : %d" % (MINPMI, n_pmi))
    print("③ +min(左熵,右熵)>=%.1f: %d   ← 理论上能晋升的词数" % (MINENT, n_ent))
    good.sort(key=lambda x: -x[1])
    print("\n前 30（词/次数/PMI/min熵）:")
    for w, c, pmi, mn in good[:30]:
        print("  %-6s %6d  PMI=%5.2f  min熵=%5.2f" % (w, c, pmi, mn))
    if good:
        print("\n末 10（刚过门槛的，多为噪声）:")
        for w, c, pmi, mn in good[-10:]:
            print("  %-6s %6d  PMI=%5.2f  min熵=%5.2f" % (w, c, pmi, mn))

if __name__ == "__main__":
    main()
