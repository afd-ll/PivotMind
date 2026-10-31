# -*- coding: utf-8 -*-
"""构建双层领域表 v3 —— 修正 IDF 的规模依赖（关键）。

v2 的两个 bug：
  ① 分位语义反了：降序排列后取 scored[len*pct] 拿到的是最小值，不是 top-0.3%。
  ② IDF 的 NW 用了候选集大小 ⇒ 3000 词和 8000 词算出的 cos 不可比，阈值没法复用
     （3000 词时 0.20 能出 62 词大簇，8000 词时 0.20 全碎成单点）。

v3 修法：
  - ctxdf 与 NW 一律按**全集**（raw 里所有词）算 ⇒ 同一对词的 cos 与候选集无关
  - 阈值用固定值（因为现在可比了）：强域 0.30 / 弱域 0.20
  - 互惠 kNN 保留（单向 top-K 必塌缩）

用法: domains_build3.py [词数上限=8000]
"""
import io
import math
import os
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _seed_patch import SEEDS, ORDER

CTX = os.environ.get("CTX_IN", "/home/ubuntu/PivotMind/data/ctx_words.tsv")
OUT = os.environ.get("DOM_OUT", "/home/ubuntu/PivotMind/data/domains.tsv")

N_WORDS = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
STRONG = (4, 0.30, "强域")
WEAK = (3, 0.20, "弱域")
PAIR_CAP = 15_000_000


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


def build_graph(keep, V, NORM, ctxdf, topk, mincos, label):
    keep_set = set(keep)
    # 倒排只收 df 低的邻词（高频邻词 idf 低、区分力弱）
    max_df = max(200, int(0.02 * len(keep)))
    inv = defaultdict(list)
    for w, v in V.items():
        for nb, wt in v.items():
            if ctxdf[nb] <= max_df:
                inv[nb].append((w, wt))

    pair = defaultdict(float)
    cap = False
    for nb, lst in inv.items():
        n = len(lst)
        if n < 2:
            continue
        for i in range(n):
            a, wa = lst[i]
            for j in range(i + 1, n):
                b, wb = lst[j]
                k = (a, b) if a < b else (b, a)
                pair[k] += wa * wb
        if len(pair) > PAIR_CAP:
            cap = True
            break
    del inv
    print("  [%s] 倒排后相似对 %d%s (max_df=%d)"
          % (label, len(pair), "（触顶）" if cap else "", max_df))

    nbr = defaultdict(list)
    for (a, b), dot in pair.items():
        d = NORM[a] * NORM[b]
        if d <= 1e-12:
            continue
        s = dot / d
        if s >= mincos:
            nbr[a].append((s, b))
            nbr[b].append((s, a))
    del pair
    print("  [%s] cos>=%.2f 的边 %d" % (label, mincos, sum(len(v) for v in nbr.values()) // 2))

    cand = {a: set(b for s, b in sorted(lst, reverse=True)[:topk]) for a, lst in nbr.items()}
    adj = defaultdict(set)
    for a, bs in cand.items():
        for b in bs:
            if a in cand.get(b, ()):
                adj[a].add(b)
                adj[b].add(a)
    del nbr, cand
    print("  [%s] 互惠后边 %d" % (label, sum(len(s) for s in adj.values()) // 2))

    seen, comp_of, comps = set(), {}, []
    for w in keep:
        if w in seen:
            continue
        stack = [w]
        seen.add(w)
        comp = []
        while stack:
            x = stack.pop()
            comp.append(x)
            for y in adj.get(x, ()):
                if y not in seen:
                    seen.add(y)
                    stack.append(y)
        cid = len(comps)
        comps.append(comp)
        for x in comp:
            comp_of[x] = cid
    sizes = [len(c) for c in comps]
    print("  [%s] 簇 %d（>=5 词 %d），最大 %d，平均 %.2f"
          % (label, len(comps), sum(1 for s in sizes if s >= 5),
             max(sizes) if sizes else 0, sum(sizes) / max(len(sizes), 1)))
    return comp_of, sizes


def main():
    tag_of = load_seeds()

    raw = defaultdict(Counter)
    with io.open(CTX, encoding="utf-8", errors="ignore") as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) < 4:
                continue
            try:
                c = int(p[3])
            except ValueError:
                continue
            raw[p[0]][p[2]] += c
    print("ctx 表 %d 词" % len(raw))

    # 全集统计（关键修正）
    ctxdf = Counter()
    for w, ctr in raw.items():
        for nb in ctr:
            ctxdf[nb] += 1
    NW = len(raw)
    print("全集 NW=%d，ctx 去重 %d" % (NW, len(ctxdf)))

    act = {w: sum(v.values()) for w, v in raw.items()}
    seeds_present = [w for w in tag_of if w in raw]
    others = sorted((w for w in raw if w not in tag_of), key=lambda w: -act[w])
    keep = list(dict.fromkeys(seeds_present + others))[:N_WORDS]
    print("候选 %d（种子 %d）" % (len(keep), len(seeds_present)))

    V = {}
    for w in keep:
        v = {}
        for nb, c in raw[w].items():
            idf = math.log((NW + 1.0) / (ctxdf[nb] + 0.5))
            wt = math.log(1.0 + c) * idf
            if wt > 0:
                v[nb] = wt
        if v:
            V[w] = v
    NORM = {w: math.sqrt(sum(x * x for x in u.values())) for w, u in V.items()}
    print("向量 %d，平均非零维 %.0f" % (len(V), sum(len(v) for v in V.values()) / max(len(V), 1)))

    print("\n[强域] topk=%d cos>=%.2f" % (STRONG[0], STRONG[1]))
    s_of, s_sizes = build_graph(keep, V, NORM, ctxdf, STRONG[0], STRONG[1], STRONG[2])
    print("\n[弱域] topk=%d cos>=%.2f" % (WEAK[0], WEAK[1]))
    w_of, w_sizes = build_graph(keep, V, NORM, ctxdf, WEAK[0], WEAK[1], WEAK[2])

    with io.open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("# word\tstrong_id\tstrong_size\tweak_id\tweak_size\n")
        for w in V:
            si, wi = s_of.get(w, -1), w_of.get(w, -1)
            f.write("%s\t%d\t%d\t%d\t%d\n"
                    % (w, si, s_sizes[si] if si >= 0 else 0,
                       wi, w_sizes[wi] if wi >= 0 else 0))
    print("\n已写 %s（%d 词）" % (OUT, len(V)))

    inv_s, inv_w = defaultdict(list), defaultdict(list)
    for w in V:
        inv_s[s_of.get(w, -1)].append(w)
        inv_w[w_of.get(w, -1)].append(w)
    print("\n抽查（同簇词，最多 10 个）：")
    for probe in ("音乐", "编程", "学习", "焦虑", "跑步", "代码", "结婚", "苹果", "吃"):
        if probe not in s_of:
            print("  %s：不在候选集" % probe)
            continue
        sc = [x for x in inv_s[s_of[probe]] if x != probe]
        wc = [x for x in inv_w[w_of[probe]] if x != probe]
        print("  %-6s strong(%d): %s" % (probe, len(inv_s[s_of[probe]]), " ".join(sc[:10])))
        print("         weak  (%d): %s" % (len(inv_w[w_of[probe]]), " ".join(wc[:10])))


if __name__ == "__main__":
    main()
