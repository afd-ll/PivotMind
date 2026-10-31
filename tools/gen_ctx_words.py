# -*- coding: utf-8 -*-
"""建「每词的左邻 / 右邻具体词表」—— 词类（POS）唯一正确的信号来源。

为什么不能用语义边：sem_edges 是**同句共现**，测的是话题相关（「听音乐」里
听=动词 与 音乐=名词 必然共现）⇒ 用共现做不出词类（实测 Cohen's d = -0.005）。

词类靠 Harris 分布假说：同类词的**邻接环境类型**相似。

🔴 v2 修的关键 bug：`data/word_lm_vocab.tsv` 只有双字/三字，**没有单字**，
   于是「我 / 的 / 吃 / 很」全被当 OOV 跳过 ⇒ 单字种子拿不到邻接数据。
   修法：最长匹配失败时**按单字兜底**（state 里有 6,063 个单字节点）。

输出（长表，按计数降序）：
    词 \t L|R \t 邻词 \t 计数
门槛用计数而不是条数上限：MIN_CTX_COUNT = 3（某一侧与某邻词共现 >= 3 次）
"""
import io
import os
from collections import Counter, defaultdict

VOCAB = os.environ.get("GEN_VOCAB", "/home/ubuntu/PivotMind/data/word_lm_vocab.tsv")
OUT = os.environ.get("CTX_OUT", "/home/ubuntu/PivotMind/data/ctx_words.tsv")

CORPORA = [
    "/home/ubuntu/pm_retrain/all_qa_pipe.txt",
    "/home/ubuntu/lccc/lccc_pipe.txt",
    "/home/ubuntu/PivotMind/data/corpora/xiaohuangji.txt",
]

MIN_DF = 20
MIN_CTX_COUNT = 3
MAX_LEN = 4
MAX_SENT_WORDS = 200
PUNCT = set("。！？；，、,.!?;:：\"'“”‘’（）()《》〈〉【】[]…—－　 \t\n\r")


def load_vocab():
    words = set()
    with io.open(VOCAB, encoding="utf-8", errors="ignore") as f:
        for line in f:
            w = line.split("\t")[0].strip()
            if w:
                words.add(w)
    return words


def tokenize(text, vocab):
    """最长匹配 + 单字兜底（v2）"""
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
            out.append(text[i])   # 单字兜底：词表里没有单字
            i += 1
    return out


def split_sents(line):
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
    print("词表 %d 词（注意：该表无单字，靠兜底）" % len(vocab))

    freq = Counter()
    L = defaultdict(Counter)
    R = defaultdict(Counter)
    nl = 0

    for path in CORPORA:
        if not os.path.exists(path):
            print("跳过 %s（不存在）" % path)
            continue
        print("读 %s ..." % path)
        with io.open(path, encoding="utf-8", errors="ignore") as f:
            for line in f:
                nl += 1
                line = line.replace("|", "。").replace(" ", "")
                for s in split_sents(line):
                    toks = tokenize(s, vocab)
                    m = len(toks)
                    if m < 2 or m > MAX_SENT_WORDS:
                        continue
                    for i, w in enumerate(toks):
                        if w in PUNCT:
                            continue
                        freq[w] += 1
                        if i > 0:
                            p = toks[i - 1]
                            if p not in PUNCT:
                                L[w][p] += 1
                        if i + 1 < m:
                            nx = toks[i + 1]
                            if nx not in PUNCT:
                                R[w][nx] += 1

    total = sum(freq.values())
    keep = {w for w, c in freq.items() if c >= MIN_DF}
    print("语料 %d 行 · 总词次 %d · 保留（df>=%d）%d 词" % (nl, total, MIN_DF, len(keep)))

    nrow = 0
    with io.open(OUT, "w", encoding="utf-8", newline="\n") as f:
        for side, tab in (("L", L), ("R", R)):
            for w, ctr in tab.items():
                if w not in keep:
                    continue
                for nb, c in ctr.most_common():
                    if c < MIN_CTX_COUNT:
                        break
                    if nb not in keep:
                        continue
                    f.write("%s\t%s\t%s\t%d\n" % (w, side, nb, c))
                    nrow += 1
    print("已写 %s：%d 行" % (OUT, nrow))

    print("\n抽查（单字必须也有数据）：")
    for probe in ("我", "你", "的", "了", "吃", "跑", "很", "不", "学习", "音乐", "漂亮", "和", "在"):
        lt = L.get(probe, Counter())
        rt = R.get(probe, Counter())
        if not lt and not rt:
            print("  %s：无数据" % probe)
            continue
        print("  %s  左%d种: %s" % (probe, len(lt),
              " ".join("%s(%d)" % (k, v) for k, v in lt.most_common(6))))
        print("     右%d种: %s" % (len(rt),
              " ".join("%s(%d)" % (k, v) for k, v in rt.most_common(6))))


if __name__ == "__main__":
    main()
