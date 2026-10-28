# -*- coding: utf-8 -*-
"""word_ctx_batch.py — 词级统计批量版（独立复核用，与线上 word_ctx.c 同口径）

与 word_ctx.py 的区别：一遍扫描，枚举汉字段内全部 2..6 字子串查字典，
支持上万词的全量复核。判据口径与线上一致：
  · 段 = 连续汉字（U+4E00..U+9FFF）
  · 段首/段尾 = <边界>，计入左右邻分布
  · 左熵/右熵 = 香农熵（nats）

用法:
  python3 word_ctx_batch.py <词表.tsv> <语料...> > 输出.tsv
词表 tsv 第一列是词名（# 开头跳过）。输出列:
  词  n  左熵  右熵  maxL%  maxR%  bd%  Ltop3  Rtop3
"""
import sys
import math
import collections
import re

HAN = re.compile(r'[\u4e00-\u9fff]+')
BORDER = '<边界>'
MAXLEN = 6


def shannon(c, n):
    if n == 0:
        return 0.0
    h = 0.0
    for v in c.values():
        p = v / n
        h -= p * math.log(p)
    return h


def top_str(c, k=3):
    return ' '.join('%s:%d' % kv for kv in c.most_common(k)) if c else '-'


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.stderr.write(__doc__)
        return 1
    vocab_path, corpus_paths = args[0], args[1:]

    words = set()
    with open(vocab_path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            w = line.split('\t')[0].strip()
            if 2 <= len(w) <= MAXLEN:
                words.add(w)
    sys.stderr.write('[batch] 词表加载 %d 词\n' % len(words))

    stat = {w: [0, collections.Counter(), collections.Counter()] for w in words}

    nlines = 0
    for fp in corpus_paths:
        with open(fp, encoding='utf-8', errors='replace') as f:
            for line in f:
                nlines += 1
                for seg in HAN.findall(line):
                    L = len(seg)
                    if L < 2:
                        continue
                    for i in range(L):
                        upper = min(L, i + MAXLEN)
                        for j in range(i + 2, upper + 1):
                            w = seg[i:j]
                            s = stat.get(w)
                            if s is None:
                                continue
                            s[0] += 1
                            s[1][seg[i - 1] if i > 0 else BORDER] += 1
                            s[2][seg[j] if j < L else BORDER] += 1

    sys.stderr.write('[batch] 语料行数 %d，开始输出\n' % nlines)
    out = sys.stdout
    out.write('词\tn\t左熵\t右熵\tmaxL%\tmaxR%\tbd%\tLtop3\tRtop3\n')
    for w, (n, L, R) in stat.items():
        if n == 0:
            out.write('%s\t0\t-\t-\t-\t-\t-\t-\t-\n' % w)
            continue
        ln = sum(L.values())
        rn = sum(R.values())
        maxL = 100.0 * L.most_common(1)[0][1] / ln if ln else 0.0
        maxR = 100.0 * R.most_common(1)[0][1] / rn if rn else 0.0
        bd = 100.0 * (L.get(BORDER, 0) + R.get(BORDER, 0)) / (ln + rn)
        out.write('%s\t%d\t%.3f\t%.3f\t%.1f\t%.1f\t%.1f\t%s\t%s\n'
                  % (w, n, shannon(L, ln), shannon(R, rn), maxL, maxR, bd,
                     top_str(L), top_str(R)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
