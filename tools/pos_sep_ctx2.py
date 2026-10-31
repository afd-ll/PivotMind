# -*- coding: utf-8 -*-
"""用「左右邻分布」测词类的可分性 —— v2：加 IDF 加权。

v1 结论：d = 0.212（类内 0.1316 / 类间 0.1170），不可用。
v1 病根：每个词左邻表里「的/我/你」都排在最前，这些高频邻词贡献了几乎全部
余弦 ⇒ 所有词互相都"像"。这是分布相似度的经典陷阱。

v2 修法：TF-IDF 式加权
    weight(w, ctx) = log(1 + count) * log((N + 1) / (df(ctx) + 0.5))
df(ctx) = 多少个词把 ctx 当邻词。到处出现的高频邻词被压到接近 0，
留下的才是「这个词专有的邻接环境」。

输入：data/ctx_words.tsv（gen_ctx_words.py v2，含单字兜底）
判据：d > 1.5 可区分；d < 0.8 换信号。
"""
import io
import math
import os
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _seed_patch import SEEDS, ORDER  # noqa: E402

CTX = os.environ.get("CTX_IN", "/home/ubuntu/PivotMind/data/ctx_words.tsv")
USE_SIDE = os.environ.get("CTX_SIDE", "both")     # both / L / R
PER_CLASS = int(os.environ.get("CTX_PER_CLASS", "40"))


def load_seeds():
    tag_of = {}
    for k in ORDER:
        seen = set()
        for w in SEEDS[k].split():
            if not w or w in seen:
                continue
            seen.add(w)
            if w not in tag_of:
                tag_of[w] = k
    return tag_of


def main():
    tag_of = load_seeds()
    print("种子 %d 个（%d 类）" % (len(tag_of), len(set(tag_of.values()))))

    rows = []                       # (word, neighbor, count)
    nline = 0
    with io.open(CTX, encoding="utf-8", errors="ignore") as f:
        for line in f:
            nline += 1
            p = line.rstrip("\n").split("\t")
            if len(p) < 4:
                continue
            w, side, nb = p[0], p[1], p[2]
            if w not in tag_of:
                continue
            if USE_SIDE != "both" and side != USE_SIDE:
                continue
            try:
                c = int(p[3])
            except ValueError:
                continue
            rows.append((w, nb, c))
    print("读 %d 行；命中种子的邻接对 %d" % (nline, len(rows)))

    # df(neighbor)
    ctxdf = Counter()
    seen = set()
    words = set()
    for w, nb, c in rows:
        words.add(w)
        k = (w, nb)
        if k not in seen:
            seen.add(k)
            ctxdf[nb] += 1
    NW = max(len(words), 1)

    raw = defaultdict(Counter)
    for w, nb, c in rows:
        raw[w][nb] += c

    V = {}
    for w, ctr in raw.items():
        v = {}
        for nb, c in ctr.items():
            idf = math.log((NW + 1.0) / (ctxdf[nb] + 0.5))
            wgt = math.log(1.0 + c) * idf
            if wgt > 0:
                v[nb] = wgt
        if v:
            V[w] = v
    N = {w: math.sqrt(sum(x * x for x in u.values())) for w, u in V.items()}
    print("有效向量 %d 个，平均非零维 %.0f"
          % (len(V), sum(len(v) for v in V.values()) / max(len(V), 1)))

    def cos(a, b):
        ua, ub = V.get(a), V.get(b)
        if not ua or not ub:
            return None
        ka, kb = ua.keys(), ub.keys()
        common = ka & kb if len(ka) < len(kb) else kb & ka
        if not common:
            return 0.0
        dot = sum(ua[k] * ub[k] for k in common)
        d = N[a] * N[b]
        return dot / d if d > 1e-12 else 0.0

    by_tag = defaultdict(list)
    for w, t in tag_of.items():
        if w in V:
            by_tag[t].append(w)
    for k in list(by_tag.keys()):
        by_tag[k] = by_tag[k][:PER_CLASS]

    print("\n各类抽样：", end="")
    for k in ORDER:
        print("%s=%d " % (k, len(by_tag[k])), end="")
    print()

    def stat(vals):
        n = len(vals)
        if not n:
            return (0, 0, 0, 0, 0)
        m = sum(vals) / n
        var = sum((x - m) ** 2 for x in vals) / (n - 1) if n > 1 else 0
        return (n, m, math.sqrt(var), min(vals), max(vals))

    in_vals, out_vals = [], []
    print("\n类内（同类种子两两）:")
    for k in ORDER:
        ws = by_tag[k]
        vals = []
        for i in range(len(ws)):
            for j in range(i + 1, len(ws)):
                s = cos(ws[i], ws[j])
                if s is not None:
                    vals.append(s)
        in_vals.extend(vals)
        n, m, sd, lo, hi = stat(vals)
        print("  %-10s n=%5d mean=%.4f sd=%.4f min=%.3f max=%.3f" % (k, n, m, sd, lo, hi))

    print("\n类间（前 8 对做代表）:")
    shown = 0
    for a_i in range(len(ORDER)):
        for b_i in range(a_i + 1, len(ORDER)):
            wa, wb = by_tag[ORDER[a_i]], by_tag[ORDER[b_i]]
            vals = []
            for x in wa:
                for y in wb:
                    s = cos(x, y)
                    if s is not None:
                        vals.append(s)
            out_vals.extend(vals)
            if shown < 8:
                n, m, sd, lo, hi = stat(vals)
                print("  %-6s x %-6s n=%5d mean=%.4f sd=%.4f" % (ORDER[a_i], ORDER[b_i], n, m, sd))
                shown += 1

    ni, mi, si, _, _ = stat(in_vals)
    no, mo, so, _, _ = stat(out_vals)
    pooled = math.sqrt((si * si + so * so) / 2.0) if (si or so) else 0
    d = (mi - mo) / pooled if pooled > 1e-12 else 0

    print("\n--- 汇总 ---")
    print("类内 : n=%d mean=%.4f sd=%.4f" % (ni, mi, si))
    print("类间 : n=%d mean=%.4f sd=%.4f" % (no, mo, so))
    print("Cohen's d = %.3f   " % d, end="")
    if d >= 1.5:
        print("⇒ 可区分：左右邻分布是有效信号，做稠密表示/半监督传播有救")
    elif d >= 0.8:
        print("⇒ 弱可分：信号在，需要更强加权或半监督传播")
    else:
        print("⇒ 不可区分：这个信号也不成立，得换思路")


if __name__ == "__main__":
    main()
