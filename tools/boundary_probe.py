# -*- coding: utf-8 -*-
"""boundary_probe.py — 词边界在字级统计里能不能被区分出来？

为什么要它（2026-10-27）：
  「公吵」能进词表，是因为 PMI 在跨词边界处**不一定低**：
  语料里「老公吵架」让 公-吵 强相邻，PMI 一样高。
  所以「再加个判据」的思路（扫描、黑名单）治不了本 —— 幺蛾子词总能冒新的。

  真正的问题不是判据，是**词表怎么来的**：现在是「给每个双字串投票」，
  而正确的做法是「让语料总似然最大的切分」（MDL / Unigram LM，SentencePiece 的核心）。
  这条路的可行性取决于一件事：**边界处的字级概率是否真的低于词内**。
  这个脚本就量这个。

判据：
  · 对每个相邻字对算 P(b|a)=c(a,b)/c(a)、P(a|b)=c(a,b)/c(b)、PMI；
  · 词内字对：双向条件概率**都**应当高（互为主要搭档）；
  · 词边界：至少一个方向明显掉下去（a 后面能接很多别的字，或不常接 b）。
"""
import io
import math
import struct
import sys
from collections import defaultdict

PATH = sys.argv[1]

# 每组是一个「跨词边界的串」，我们要看组内相邻字对哪些是词内、哪些是边界
CASES = [
    ("老公吵架", [("老", "公"), ("公", "吵"), ("吵", "架")], {"老公", "吵架"}),
    ("大部分",   [("大", "部"), ("部", "分")],               {"部分"}),
    ("海王星",   [("海", "王"), ("王", "星")],               {"海王", "王星"}),
    ("同性恋",   [("同", "性"), ("性", "恋")],               {"同性", "性恋"}),
    ("火葬场",   [("火", "葬"), ("葬", "场")],               {"火葬"}),
    ("段时间",   [("段", "时"), ("时", "间")],               {"时间"}),
    ("不代表",   [("不", "代"), ("代", "表")],               {"代表"}),
    ("公吵架",   [("公", "吵"), ("吵", "架")],               {"吵架"}),
]


def main():
    data = io.open(PATH, "rb").read()
    magic, ver, n = struct.unpack_from("<III", data, 0)
    assert magic == 0x504D4350, "magic 不对"
    pos = 12
    out_sum = defaultdict(int)                       # c(a,·)
    cnt = {}
    total = 0
    for _ in range(n):
        la = data[pos]; pos += 1
        a = data[pos:pos + la].decode("utf-8", "replace"); pos += la
        lb = data[pos]; pos += 1
        b = data[pos:pos + lb].decode("utf-8", "replace"); pos += lb
        c, = struct.unpack_from("<I", data, pos); pos += 4
        out_sum[a] += c
        cnt[(a, b)] = c
        total += c
    in_sum = defaultdict(int)
    for (a, b), c in cnt.items():
        in_sum[b] += c

    print("[boundary] total=%d 对=%d\n" % (total, len(cnt)))
    print("%-10s %-6s %10s %8s %8s %7s  %s" %
          ("组", "字对", "count", "P(b|a)", "P(a|b)", "PMI", "判读"))
    for word, pairs, real_words in CASES:
        for (a, b) in pairs:
            c = cnt.get((a, b), 0)
            if c == 0:
                print("%-10s %-6s %10d %8s %8s %7s  %s" % (word, a + b, 0, "-", "-", "-", "无共现"))
                continue
            p_ba = c / out_sum[a] if out_sum[a] else 0.0
            p_ab = c / in_sum[b] if in_sum[b] else 0.0
            pmi = math.log2((c * total) / (out_sum[a] * in_sum[b])) if out_sum[a] and in_sum[b] else 0.0
            tag = "词内?" if (a + b) in real_words else "边界?"
            print("%-10s %-6s %10d %8.4f %8.4f %7.2f  %s" %
                  (word, a + b, c, p_ba, p_ab, pmi, tag))
        print()

    print("[boundary] 读法：")
    print("  · 若「词内」字对的双向 P 都明显高于「边界」字对 ⇒ 边界可由字级概率判定，")
    print("    MDL / Unigram LM 这条路成立（切分目标 = 语料总似然最大）。")
    print("  · 若两类混在一起无法区分 ⇒ 单靠字级统计定不了边界，")


main()
