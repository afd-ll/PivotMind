# -*- coding: utf-8 -*-
"""LCCC 词表 vs 现网词表：定点验收 + 规模对比。

用法：python3 lccc_vocab_check.py <新词表> <现网dump>
"""
import io, sys


def load_vocab(path):
    lines = io.open(path, encoding="utf-8", errors="replace").read().split("\n")
    start = 0
    for i, l in enumerate(lines[:6]):
        if l.startswith("词\t"):
            start = i
            break
    d = {}
    for l in lines[start + 1:]:
        p = l.split("\t")
        if len(p) >= 2 and p[0]:
            try:
                d[p[0]] = float(p[1])
            except ValueError:
                continue
    return d


def load_dump(path):
    d = {}
    for l in io.open(path, encoding="utf-8", errors="replace"):
        p = l.rstrip("\n").split("\t")
        if len(p) == 4 and p[0]:
            d[p[0]] = p[0]
    return d


new = load_vocab(sys.argv[1])
now = load_dump(sys.argv[2])

print("新词表 %d 词（双字+）· 现网 %d 词（含单字）" % (len(new), len(now)))

MUST_KEEP = ["你好", "乒乓球", "拍卖", "因为", "时候", "机器", "再见", "学习",
             "天气", "朋友", "工作", "计算机", "音乐", "苹果", "喜欢", "什么"]
MUST_DROP = ["我最", "份工", "性和", "的原", "用太", "公吵", "却很", "最最"]

print("\n--- 应保留（真词）---")
miss_keep = []
for w in MUST_KEEP:
    ok = w in new
    if not ok:
        miss_keep.append(w)
    print("  %-8s %s%s" % (w, "✓ 在" if ok else "✗ 不在",
                           "" if w in now else "   (现网也没有)"))

print("\n--- 应剔除（切片/跨边界）---")
bad_keep = []
for w in MUST_DROP:
    gone = w not in new
    if not gone:
        bad_keep.append(w)
    print("  %-8s %s%s" % (w, "✓ 已清" if gone else "✗ 仍在",
                           "" if w in now else "   (现网本来没有)"))

# 词表差集
sn, so = set(new), set(now)
print("\n--- 与现网差集 ---")
print("  新增（新有旧无）: %d" % len(sn - so))
print("  消失（旧有新无）: %d" % len(so - sn))
gone = sorted(so - sn)
print("  消失样例: " + " ".join(gone[:20]))
added = sorted(sn - so)
print("  新增样例: " + " ".join(added[:20]))

# 高频消失词（现网有边的 → 说明它不是纯噪声）
print("\n应保留缺失: %s" % ("无" if not miss_keep else " ".join(miss_keep)))
print("应剔除仍在: %s" % ("无" if not bad_keep else " ".join(bad_keep)))
