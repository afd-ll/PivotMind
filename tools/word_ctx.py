# -*- coding: utf-8 -*-
"""word_ctx.py — 词的「独立使用证据」探针（治本方案第 0 步）

用途：给一批候选词，扫语料，算它作为**独立单位**的证据强度。

判据（关键）：
  一个字符串能当词，不是因为它"有独立意思"（这个无法测量），
  而是因为它在语料里被**当成整体用过** —— 左右邻分散。
  若某一侧高度集中（如右邻 100% 都是同一个字），它只是某个长串的切片。

输出字段：
  n        出现次数
  LE/RE    左/右邻香农熵（nats）；边界（标点/行首尾）计入 <边界>
  maxL/maxR 左/右邻 top1 占比 %
  bd%      两侧为边界的比例（能独立成单位出现）
  Ltop/Rtop 左/右邻 top3

用法：
  python3 word_ctx.py <语料...> -- 词1 词2 ...
"""
import sys
import math
import collections
import re

HAN = re.compile(r'[\u4e00-\u9fff]+')
BORDER = '<边界>'


def shannon(c):
    n = sum(c.values())
    if n == 0:
        return 0.0
    h = 0.0
    for v in c.values():
        p = v / n
        h -= p * math.log(p)
    return h


def top_str(c, k=3):
    items = c.most_common(k)
    return ' '.join('%s:%d' % (w, v) for w, v in items) if items else '-'


def main():
    args = sys.argv[1:]
    if '--' not in args:
        sys.stderr.write(__doc__)
        return 1
    i = args.index('--')
    files, words = args[:i], args[i + 1:]
    if not files or not words:
        sys.stderr.write(__doc__)
        return 1

    stat = {}
    for w in words:
        stat[w] = {'n': 0, 'L': collections.Counter(), 'R': collections.Counter(),
                   'bd': 0}

    nl = 0
    for fp in files:
        with open(fp, encoding='utf-8', errors='replace') as f:
            for line in f:
                nl += 1
                for seg in HAN.findall(line):
                    L = len(seg)
                    for w in words:
                        wl = len(w)
                        start = 0
                        while True:
                            p = seg.find(w, start)
                            if p < 0:
                                break
                            s = stat[w]
                            s['n'] += 1
                            if p > 0:
                                s['L'][seg[p - 1]] += 1
                            else:
                                s['L'][BORDER] += 1
                                s['bd'] += 1
                            q = p + wl
                            if q < L:
                                s['R'][seg[q]] += 1
                            else:
                                s['R'][BORDER] += 1
                                s['bd'] += 1
                            start = p + 1

    print('语料行数=%d  文件=%s' % (nl, ','.join(files)))
    print('')
    hdr = ('%-10s %8s %6s %6s %6s %6s %6s  %-22s %-22s'
           % ('词', 'n', 'LE', 'RE', 'maxL%', 'maxR%', 'bd%', 'Ltop3', 'Rtop3'))
    print(hdr)
    print('-' * len(hdr.encode('utf-8').decode('utf-8')))
    for w in words:
        s = stat[w]
        n = s['n']
        if n == 0:
            print('%-10s %8d  —— 语料中零出现（词表有、语料无 = 纯晋升产物）' % (w, 0))
            continue
        ln = sum(s['L'].values())
        rn = sum(s['R'].values())
        maxL = 100.0 * s['L'].most_common(1)[0][1] / ln if ln else 0.0
        maxR = 100.0 * s['R'].most_common(1)[0][1] / rn if rn else 0.0
        bd = 100.0 * s['bd'] / (ln + rn) if (ln + rn) else 0.0
        print('%-10s %8d %6.2f %6.2f %6.1f %6.1f %6.1f  %-22s %-22s'
              % (w, n, shannon(s['L']), shannon(s['R']), maxL, maxR, bd,
                 top_str(s['L']), top_str(s['R'])))
    return 0


if __name__ == '__main__':
    sys.exit(main())
