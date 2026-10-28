# -*- coding: utf-8 -*-
"""离线重算 MI 阈值：不重跑 EM，直接用收敛词表（含单字）算不同阈值下的剪枝效果。
关注三件事：
  1. `你好` 的 PMI 到底多少（为什么被 2.5 剪掉）
  2. `一X` 碎片在不同阈值下的数量
  3. 真词（拍卖/因为/时候/再见…）在哪个阈值开始被误杀
"""
import io, sys, math

path = sys.argv[1]
lines = io.open(path, encoding="utf-8", errors="replace").read().split("\n")
start = 0
for i, l in enumerate(lines[:6]):
    if l.startswith("词\t"):
        start = i
        break
P = {}
for l in lines[start + 1:]:
    p = l.split("\t")
    if len(p) >= 2 and p[0]:
        try:
            P[p[0]] = float(p[1])
        except ValueError:
            continue
print("收敛词表 %d 词（单字 %d）" % (len(P), sum(1 for w in P if len(w) == 1)))

d2 = [w for w in P if len(w) == 2]


def pmi(w):
    a, b = w[0], w[1]
    pa, pb = P.get(a), P.get(b)
    if not pa or not pb or not P.get(w):
        return None
    return math.log(P[w] / (pa * pb))


print("\n--- 关键词的 P 与 PMI ---")
for w in ["你好", "拍卖", "因为", "时候", "再见", "学习", "天气", "朋友", "工作",
          "什么", "喜欢", "音乐", "份工", "却很", "最最", "一下", "一起", "一定"]:
    if w in P:
        m = pmi(w)
        print("  %-6s P=%.3e  PMI=%s" % (w, P[w], "%.2f" % m if m is not None else "缺成分"))
    else:
        print("  %-6s （不在收敛词表）" % w)

print("\n--- 各阈值下的剪枝效果 ---")
KEEP = ["你好", "拍卖", "因为", "时候", "再见", "学习", "天气", "朋友", "工作",
        "什么", "喜欢", "音乐", "苹果"]
for th in (1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 5.0):
    kept = [w for w in d2 if (pmi(w) or -9) >= th]
    yi = [w for w in kept if w[0] == "一"]
    kill = [w for w in KEEP if w in P and (pmi(w) or -9) < th]
    print("  MI>=%.1f : 保留 %-6d（一X %-5d）  误杀 %d %s"
          % (th, len(kept), len(yi), len(kill), " ".join(kill)))
