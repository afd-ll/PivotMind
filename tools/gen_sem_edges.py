#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""CX-59：离线算「同句共现 + PPMI」语义边，输出 sem_edges.tsv 供 qa_retrain 阶段三读取

依据（文献）：
  - function word inflation 是共现矩阵的头号失效模式，解法是 PMI（PPMI = max(0, PMI)）
  - 句边界必须遵守（standard practice）
  - PMI 对数据稀疏不敏感 ⇒ 必须配最小共现次数
  - 窗口/同句捕获的是 topical relatedness（玄枢需要的），依存句法给的是 functional similarity（同类），不是我们要的

输出：词A \t 词B \t ppmi \t 共现次数  （按 PPMI 降序，每词最多 MAX_PER_WORD 条）
"""
import io
import math
import os
import sys
from collections import Counter, defaultdict

VOCAB = os.environ.get("GEN_VOCAB", "/home/ubuntu/PivotMind/data/word_lm_vocab.tsv")
OUT = os.environ.get("GEN_OUT", "/home/ubuntu/PivotMind/data/sem_edges.tsv")
_c = os.environ.get("GEN_CORPORA")
CORPORA = _c.split(":") if _c else [
    "/home/ubuntu/PivotMind/corpus/xiaohuangji_pipe.txt",
    "/home/ubuntu/lccc/lccc_300k.txt",
]

MIN_DF = 20          # 词频下限（PMI 对稀疏不敏感，低频词统计不可靠）
MIN_CO = 5           # 最小共现次数（实测 c4 的「出身/及格」是巧合）
PPMI_MIN = 3.0       # PPMI 阈值
MAX_PER_WORD = 40    # 每词最多保留多少条语义边（防爆）
MAX_SENT_WORDS = 40  # 超长句不统计（避免 O(n^2) 爆炸）
MAX_LEN = 4


def load_vocab():
    words = set()
    with io.open(VOCAB, encoding="utf-8", errors="ignore") as f:
        for line in f:
            w = line.split("\t")[0].strip()
            if w:
                words.add(w)
    return words


def tokenize(text, vocab):
    out = []
    i, n = 0, len(text)
    while i < n:
        hit = None
        for L in range(MAX_LEN, 0, -1):
            if i + L <= n and text[i:i + L] in vocab:
                hit = text[i:i + L]
                i += L
                break
        if hit:
            out.append(hit)
        else:
            i += 1
    return out


def split_sents(line):
    """按标点切句（文献：respect sentence boundaries）"""
    sents, cur = [], []
    for ch in line:
        if ch in "。！？；,.!?;、\n\r\t":
            if cur:
                sents.append("".join(cur))
                cur = []
        else:
            cur.append(ch)
    if cur:
        sents.append("".join(cur))
    return sents


def main():
    vocab = load_vocab()
    print("词表 %d 词" % len(vocab))

    # 第一遍：词频
    df = Counter()
    nl = 0
    for path in CORPORA:
        if not os.path.exists(path):
            print("跳过 %s" % path)
            continue
        with io.open(path, encoding="utf-8", errors="ignore") as f:
            for line in f:
                nl += 1
                line = line.replace("|", "。").replace(" ", "")
                for s in split_sents(line):
                    df.update(tokenize(s, vocab))
    total = sum(df.values())
    keep = {w for w, c in df.items() if c >= MIN_DF}
    print("语料 %d 行 · 总词次 %d · 保留（df>=%d）%d 词" % (nl, total, MIN_DF, len(keep)))

    # 第二遍：同句共现（只在 keep 内）
    co = Counter()
    for path in CORPORA:
        if not os.path.exists(path):
            continue
        with io.open(path, encoding="utf-8", errors="ignore") as f:
            for line in f:
                line = line.replace("|", "。").replace(" ", "")
                for s in split_sents(line):
                    toks = [w for w in tokenize(s, vocab) if w in keep]
                    if len(toks) < 2 or len(toks) > MAX_SENT_WORDS:
                        continue
                    uniq = list(dict.fromkeys(toks))     # 句内去重（避免长句刷同一对）
                    for i in range(len(uniq)):
                        for j in range(i + 1, len(uniq)):
                            a, b = uniq[i], uniq[j]
                            co[(a, b) if a < b else (b, a)] += 1
    print("共现对（去重后）%d 种" % len(co))

    # 算 PPMI
    edges = []
    for (a, b), c in co.items():
        if c < MIN_CO:
            continue
        pa, pb, pab = df[a] / total, df[b] / total, c / total
        if pa <= 0 or pb <= 0 or pab <= 0:
            continue
        p = math.log(pab / (pa * pb), 2)
        if p >= PPMI_MIN:
            edges.append((a, b, p, c))
    print("PPMI >= %.1f 且 共现 >= %d 的边: %d" % (PPMI_MIN, MIN_CO, len(edges)))

    # 每词限流
    edges.sort(key=lambda x: -x[2])
    per = defaultdict(int)
    out = []
    for a, b, p, c in edges:
        if per[a] >= MAX_PER_WORD or per[b] >= MAX_PER_WORD:
            continue
        per[a] += 1
        per[b] += 1
        out.append((a, b, p, c))
    print("限流后（每词<=%d）: %d 条" % (MAX_PER_WORD, len(out)))

    with io.open(OUT, "w", encoding="utf-8", newline="\n") as f:
        for a, b, p, c in out:
            f.write("%s\t%s\t%.3f\t%d\n" % (a, b, p, c))
    print("已写 %s" % OUT)

    # 抽检：目标词的邻居
    for t in ["编程", "音乐", "盐", "学习"]:
        nb = [(a if b == t else b, p, c) for a, b, p, c in out if a == t or b == t]
        nb.sort(key=lambda x: -x[1])
        print("\n「%s」的 PPMI 邻居 %d 个:" % (t, len(nb)))
        for w, p, c in nb[:12]:
            print("    %-8s ppmi=%5.2f c=%d" % (w, p, c))


if __name__ == "__main__":
    main()
