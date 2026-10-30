# -*- coding: utf-8 -*-
"""洗语义边第一步：统计 sem_edges 端点词里有多少「碎片」

判据来自文献（Mel'čuk 意义-文本理论）：
  lexeme（澡、音乐）能独立成词条；collocation 的前件（泡个、有着）不能。
  可操作的机器判据 = CX-40 用过的「左右邻熵」：碎片只出现在固定搭配里
  （泡个→澡），所以左右邻的取值几乎唯一、熵≈0；真词左右邻分散、熵高。

输出：每条边的两个端点各自的 min(左熵,右熵)，以及碎片边占比。
"""
import io
import math
import os
import sys
from collections import defaultdict

VOCAB = os.environ.get("GEN_VOCAB", "/home/ubuntu/PivotMind/data/word_lm_vocab.tsv")
SEM = os.environ.get("SEM_IN", "/home/ubuntu/PivotMind/data/sem_edges.tsv")
CORPORA = [
    "/home/ubuntu/pm_retrain/all_qa_pipe.txt",
]

def load_vocab():
    vs = set()
    for ln in io.open(VOCAB, encoding="utf-8", errors="ignore"):
        w = ln.split("\t")[0].strip()
        if w:
            vs.add(w)
    return vs

def load_endpoints():
    ends = set()
    edges = []
    for ln in io.open(SEM, encoding="utf-8", errors="ignore"):
        p = ln.rstrip("\n").split("\t")
        if len(p) < 2:
            continue
        a, b = p[0], p[1]
        ends.add(a); ends.add(b)
        try:
            w = float(p[2])
        except Exception:
            w = 0.0
        edges.append((a, b, w))
    return ends, edges

def segment(line, vocab, maxlen=4):
    """最长匹配分词（按字符），只切出现在 vocab 里的词；切不出来就丢单字。"""
    out = []
    i, n = 0, len(line)
    while i < n:
        ch = line[i]
        if ch in "|\u3000 \t\r\n":
            i += 1
            continue
        hit = None
        for L in range(min(maxlen, n - i), 0, -1):
            cand = line[i:i + L]
            if cand in vocab:
                hit = cand
                break
        if hit:
            out.append(hit)
            i += len(hit)
        else:
            i += 1
    return out

def entropy(counter):
    tot = sum(counter.values())
    if tot <= 1:
        return 0.0
    h = 0.0
    for c in counter.values():
        p = c / tot
        h -= p * math.log(p, 2)
    return h

def main():
    vocab = load_vocab()
    ends, edges = load_endpoints()
    print("词表 %d · 语义边 %d · 端点词（去重）%d" % (len(vocab), len(edges), len(ends)))
    sys.stdout.flush()

    # 只统计端点词（其余词不进计数器，省内存）
    left = defaultdict(lambda: defaultdict(int))
    right = defaultdict(lambda: defaultdict(int))

    for path in CORPORA:
        if not os.path.exists(path):
            print("  跳过不存在:", path)
            continue
        nline = 0
        with io.open(path, encoding="utf-8", errors="ignore") as f:
            for ln in f:
                nline += 1
                seg = segment(ln, vocab)
                L = len(seg)
                for i, w in enumerate(seg):
                    if w in ends:
                        if i > 0:
                            left[w][seg[i - 1]] += 1
                        if i + 1 < L:
                            right[w][seg[i + 1]] += 1
                if nline % 200000 == 0:
                    print("  ...%d 行" % nline)
                    sys.stdout.flush()
        print("  读完 %s（%d 行）" % (os.path.basename(path), nline))

    # 算每个端点词的 min 熵
    ment = {}
    for w in ends:
        lh = entropy(left[w]) if w in left else 0.0
        rh = entropy(right[w]) if w in right else 0.0
        ment[w] = min(lh, rh)

    # 分档
    bands = [0.0, 0.5, 1.0, 2.0, 3.0, 5.0, 99]
    cnt = [0] * (len(bands) - 1)
    for w in ends:
        v = ment[w]
        for k in range(len(bands) - 1):
            if bands[k] <= v < bands[k + 1]:
                cnt[k] += 1
                break
    print("\n端点词 min(左熵,右熵) 分布：")
    for k in range(len(bands) - 1):
        print("  [%.1f, %.1f) : %d" % (bands[k], bands[k + 1], cnt[k]))

    # 边级别：任一端是碎片
    for thr in (0.5, 1.0, 2.0):
        frag = sum(1 for a, b, w in edges if ment.get(a, 0) < thr or ment.get(b, 0) < thr)
        print("  阈值 %.1f ⇒ 碎片边 %d / %d = %.1f%%" % (thr, frag, len(edges), 100.0 * frag / len(edges)))

    # 举例
    print("\n低熵端点样例（< 1.0，按边权降序前 20）：")
    bad = [(a, b, w) for a, b, w in edges if ment.get(a, 0) < 1.0 or ment.get(b, 0) < 1.0]
    bad.sort(key=lambda x: -x[2])
    for a, b, w in bad[:20]:
        print("  %s(%.2f) → %s(%.2f)  ppm i=%.2f" % (a, ment.get(a, 0), b, ment.get(b, 0), w))

    print("\n高熵端点样例（>= 3.0，按边权降序前 12）：")
    good = [(a, b, w) for a, b, w in edges if ment.get(a, 99) >= 3.0 and ment.get(b, 99) >= 3.0]
    good.sort(key=lambda x: -x[2])
    for a, b, w in good[:12]:
        print("  %s(%.2f) → %s(%.2f)  ppm i=%.2f" % (a, ment.get(a, 0), b, ment.get(b, 0), w))

    # 落盘供下一步过滤
    out = os.environ.get("MENT_OUT", "/home/ubuntu/PivotMind/data/sem_ment.tsv")
    with io.open(out, "w", encoding="utf-8", newline="\n") as f:
        for w, v in sorted(ment.items(), key=lambda x: x[1]):
            f.write("%s\t%.4f\n" % (w, v))
    print("\n已写 %s（%d 词）" % (out, len(ment)))

if __name__ == "__main__":
    main()
