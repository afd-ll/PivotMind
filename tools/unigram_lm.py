# -*- coding: utf-8 -*-
"""unigram_lm.py — Unigram LM（EM + Viterbi）词表学习原型（CX-45）

目的：用「整语料切分总似然最大」学词表，替代「逐候选投票（PMI/熵/次数）」。
本文件是离线原型，不参与线上构建。

口径与线上一致（对齐 word_ctx_batch.py / word_ctx.c）：
  · 段 = 连续汉字 U+4E00..U+9FFF；标点/空白/拉丁字母均为段边界
  · 一律不跨段统计

算法：
  1) 储层抽样（固定 seed，训练/留出一次抽出，互斥）
  2) 候选词 = 全部单字 + 训练集出现 >= min_count 的 2..max_len gram
  3) EM：E 步 Viterbi 求最大似然切分并计次 → M 步 P(w)=c(w)/Σc → 剪枝（期望出现 < 1 次即丢）
  4) 收敛后按概率排序，做「词表规模扫描」：保留 top-K 重估概率 + 用 held-out 打分，
     得到 词表规模 → logP/char 曲线（用来找膝点，并与旧词表在**同规模**下对比）
  5) 若给 --old-vocab，则用同一套 EM 流程训练旧词表（不增词、不剪枝），
     在同一 held-out 上打分，输出对比与定点验收

用法：
  python3 unigram_lm.py train --corpus A.txt B.jsonl ... \
      --lines 100000 --heldout 12000 --seed 42 --min-count 20 --max-len 4 \
      --iters 10 --out /tmp/unigram --old-vocab /tmp/vocab_dump.tsv

产出（--out 目录）：
  vocab_new.tsv     词 \t P \t 期望计数 \t 长度
  curve.tsv         每轮 iter \t logP/char \t 词数 \t 秒
  sweep.tsv         K \t logP/char（新词表 top-K）
  report.txt        汇总（含旧词表对照、定点验收、长度分布、抽样清单）
  added.txt / removed.txt  新词表 vs 旧词表的差集
"""
import argparse
import collections
import math
import os
import random
import re
import sys
import time

HAN = re.compile(r'[\u4e00-\u9fff]+')
NEG_INF = float('-inf')


# ---------------------------------------------------------------- 语料

def iter_segments(paths):
    """按线上口径产出汉字段（标点/空白/拉丁即边界）。"""
    for fp in paths:
        with open(fp, encoding='utf-8', errors='replace') as f:
            for line in f:
                for seg in HAN.findall(line):
                    yield seg


def sample_segments(paths, n_total, seed):
    """储层抽样：一次遍历取出 n_total 个段，前半训练、后半留出（互斥）。"""
    rng = random.Random(seed)
    res = []
    seen = 0
    for seg in iter_segments(paths):
        seen += 1
        if len(res) < n_total:
            res.append(seg)
        else:
            j = rng.randint(0, seen - 1)
            if j < n_total:
                res[j] = seg
    return res


# ---------------------------------------------------------------- 模型

def load_all(paths, n_held, seed, out=sys.stderr):
    """全量模式：储层抽取 n_held 段作留出，其余全部进训练（内存约 30M 汉字的字符串）。"""
    rng = random.Random(seed)
    held = []
    train = []
    seen = 0
    for seg in iter_segments(paths):
        seen += 1
        if len(held) < n_held:
            held.append(seg)
            continue
        j = rng.randint(0, seen - 1)
        if j < n_held:
            train.append(held[j])      # 被顶出的进训练集，一个不丢
            held[j] = seg
        else:
            train.append(seg)
    out.write('[data] 全量模式：训练 %d 段 / 留出 %d 段\n' % (len(train), len(held)))
    return train, held


def count_candidates(train, max_len, min_count, out=sys.stderr):
    """候选词计数（Apriori 剪枝）。

    依据：任 n-gram 的出现次数 ≤ 其任一前缀的出现次数 ⇒ 前缀不足 min_count 的长串
    必然不足，可安全跳过。这是**精确**剪枝，不是近似，能避免 3/4-gram 的计数爆炸
    （全量语料下 4-gram 唯一串可达千万级）。
    """
    c1 = collections.Counter()
    c2 = collections.Counter()
    for seg in train:
        L = len(seg)
        for i in range(L):
            c1[seg[i]] += 1
            if i + 1 < L:
                c2[seg[i:i + 2]] += 1
    keep = {w for w, c in c2.items() if c >= min_count}
    cnt = collections.Counter()
    cnt.update(c1)
    cnt.update({w: c2[w] for w in keep})
    out.write('[cand] 单字 %d · 2gram %d（留存 %d）\n' % (len(c1), len(c2), len(keep)))
    del c2

    prev = keep
    for ln in range(3, max_len + 1):
        cur = collections.Counter()
        for seg in train:
            L = len(seg)
            for i in range(L - ln + 1):
                if seg[i:i + ln - 1] in prev:
                    cur[seg[i:i + ln]] += 1
        good = {w for w, c in cur.items() if c >= min_count}
        cnt.update({w: cur[w] for w in good})
        out.write('[cand] %dgram %d（留存 %d）\n' % (ln, len(cur), len(good)))
        del cur
        prev = good

    chars = {w for w in cnt if len(w) == 1}
    vocab = set(cnt)
    return cnt, vocab, chars


def viterbi(seg, probs, max_len, floor_logp):
    """最大似然切分。返回 (tokens, logp, oov_chars)。"""
    n = len(seg)
    best = [NEG_INF] * (n + 1)
    back = [1] * (n + 1)
    oovmark = [0] * (n + 1)
    best[0] = 0.0
    for i in range(1, n + 1):
        bv = NEG_INF
        bl = 1
        boov = 0
        upper = min(max_len, i)
        for L in range(1, upper + 1):
            prev = best[i - L]
            if prev == NEG_INF:
                continue
            w = seg[i - L:i]
            p = probs.get(w)
            if p is None:
                if L != 1:
                    continue
                sc = prev + floor_logp
                oov = oovmark[i - 1] + 1
            else:
                sc = prev + math.log(p)
                oov = oovmark[i - L]
            if sc > bv:
                bv, bl, boov = sc, L, oov
        best[i] = bv
        back[i] = bl
        oovmark[i] = boov
    # 回溯
    toks = []
    i = n
    while i > 0:
        L = back[i]
        toks.append(seg[i - L:i])
        i -= L
    toks.reverse()
    return toks, best[n], oovmark[n]


def estep(data, probs, max_len, floor_logp, penalty=0.0):
    """E 步：累计计数 + 总对数似然 + 字符数 + OOV 数。"""
    counts = collections.Counter()
    total_lp = 0.0
    nchar = 0
    noov = 0
    get = probs.get
    log = math.log
    for seg in data:
        n = len(seg)
        nchar += n
        best = [NEG_INF] * (n + 1)
        back = [1] * (n + 1)
        oovmark = [0] * (n + 1)
        best[0] = 0.0
        for i in range(1, n + 1):
            bv = NEG_INF
            bl = 1
            boov = 0
            upper = min(max_len, i)
            for L in range(1, upper + 1):
                prev = best[i - L]
                if prev == NEG_INF:
                    continue
                w = seg[i - L:i]
                p = get(w)
                pen = penalty * (L - 1)      # 长度代价：压掉「整块吞短语」的倾向
                if p is None:
                    if L != 1:
                        continue
                    sc = prev + floor_logp
                    oov = oovmark[i - 1] + 1
                else:
                    sc = prev + log(p) - pen
                    oov = oovmark[i - L]
                if sc > bv:
                    bv, bl, boov = sc, L, oov
            best[i] = bv
            back[i] = bl
            oovmark[i] = boov
        noov += oovmark[n]
        i = n
        plain = 0.0
        while i > 0:
            L = back[i]
            w = seg[i - L:i]
            counts[w] += 1
            p = get(w)
            plain += log(p) if p else floor_logp
            i -= L
        total_lp += plain
    return counts, total_lp, nchar, noov


def mstep(counts, chars, floor=1e-7):
    """M 步：重估概率；保留全部单字（退路），多字词期望出现 < 1 次即剪掉。

    ⚠️ 单字必须给概率下限：若某轮所有位置都被多字词切走，单字计数会是 0，
    一旦被丢掉，held-out 里任何未登录组合都无法回退 ⇒ OOV 100%（原型首版踩过）。
    """
    tot = float(sum(counts.values())) or 1.0
    probs = {}
    for w, c in counts.items():
        if len(w) == 1 or c >= 1.0:
            probs[w] = c / tot
    miss = [w for w in chars if w not in probs]
    if miss:
        mass = min(0.5, floor * len(miss))
        scale = 1.0 - mass
        probs = {w: v * scale for w, v in probs.items()}
        for w in miss:
            probs[w] = floor
    return probs


def em(train, vocab, chars, max_len, iters, init_counts=None, penalty=0.0, out=sys.stderr):
    """在固定候选集上跑 EM（旧词表也走同一条路径，保证口径一致）。"""
    # 初始化：用原始 n-gram 计数当先验（init_counts 已有则直接复用，省一趟全量扫描）
    if init_counts is None:
        init = collections.Counter()
        for seg in train:
            L = len(seg)
            for i in range(L):
                upper = min(L, i + max_len)
                for j in range(i + 1, upper + 1):
                    w = seg[i:j]
                    if w in vocab:
                        init[w] += 1
    else:
        init = {w: init_counts[w] for w in vocab if init_counts.get(w)}
    tot = float(sum(init.values())) or 1.0
    probs = {w: c / tot for w, c in init.items() if w in vocab}
    for w in chars:                       # 单字兜底
        probs.setdefault(w, 1e-9)

    hist = []
    prev_lp = None
    for it in range(1, iters + 1):
        t0 = time.time()
        counts, total_lp, nchar, noov = estep(train, probs, max_len, math.log(1e-12), penalty)
        new_probs = mstep(counts, chars)
        # 词表收缩：候选集里未被选中的直接消失（EM 的自剪枝）
        probs = {w: p for w, p in new_probs.items() if w in vocab}
        per_char = total_lp / max(nchar, 1)
        hist.append((it, per_char, len(probs), time.time() - t0))
        out.write('[em] iter %d  logP/char=%.4f  词数=%d  %.1fs\n'
                  % (it, per_char, len(probs), time.time() - t0))
        if prev_lp is not None and abs(per_char - prev_lp) / max(abs(prev_lp), 1e-9) < 1e-3:
            out.write('[em] 收敛（相对变化 < 0.1%%）\n')
            break
        prev_lp = per_char
    return probs, hist


def score(data, probs, max_len, penalty=0.0):
    """在 held-out 上打分，返回 (logP/char, OOV 字符占比, token 数)。"""
    _, total_lp, nchar, noov = estep(data, probs, max_len, math.log(1e-12), penalty)
    return total_lp / max(nchar, 1), noov / max(nchar, 1), nchar


def sweep(train, held, probs, max_len, ks, penalty=0.0):
    """词表规模扫描：保留 top-K 后重估概率，再在 held-out 打分。"""
    order = sorted(probs.items(), key=lambda kv: -kv[1])
    chars = {w for w in probs if len(w) == 1}
    rows = []
    for K in ks:
        keep = set(chars)
        for w, _ in order[:K]:
            keep.add(w)
        sub = {w: p for w, p in probs.items() if w in keep}
        Z = sum(sub.values()) or 1.0
        sub = {w: p / Z for w, p in sub.items()}
        # 用训练集重估 2 轮，避免「只截断不重估」造成概率失真
        for _ in range(1):
            counts, _, _, _ = estep(train, sub, max_len, math.log(1e-12), penalty)
            tot = float(sum(counts.values())) or 1.0
            sub = {w: c / tot for w, c in counts.items() if w in keep}
            for w in chars:
                sub.setdefault(w, 1e-9)
        lp, oov, n = score(held, sub, max_len, penalty)
        rows.append((len(sub), lp, oov))
        sys.stderr.write('[sweep] K=%-6d 实际词数=%-6d logP/char=%.4f OOV=%.4f%%\n'
                         % (K, len(sub), lp, 100 * oov))
    return rows


def length_dist(vocab):
    d = collections.Counter(len(w) for w in vocab)
    tot = sum(d.values()) or 1
    return {k: (d.get(k, 0), 100.0 * d.get(k, 0) / tot) for k in range(1, 7)}


CHECK_DEAD = ['公吵', '用太', '的原', '性和']
CHECK_ALIVE = ['你好', '乒乓球', '拍卖', '卖', '因为', '时候', '机器', '再见']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', choices=['train'])
    ap.add_argument('--corpus', nargs='+', required=True)
    ap.add_argument('--lines', type=int, default=100000)
    ap.add_argument('--all', action='store_true', help='全量语料（只留 heldout 段）')
    ap.add_argument('--len-penalty', type=float, default=0.0,
                    help='每多一个汉字扣的 nats，压掉整块吞短语')
    ap.add_argument('--heldout', type=int, default=12000)
    ap.add_argument('--seed', type=int, default=42)
    ap.add_argument('--min-count', type=int, default=20)
    ap.add_argument('--max-len', type=int, default=4)
    ap.add_argument('--iters', type=int, default=10)
    ap.add_argument('--out', required=True)
    ap.add_argument('--old-vocab', default=None, help='vocab_coverage --dump 的 tsv')
    ap.add_argument('--emit-vocab', default=None,
                    help='额外产出给 qa_retrain 用的词表（词	P	计数	长度）')
    ap.add_argument('--indep-pmi', type=float, default=0.0,
                    help='成分独立率闸门：PMI(切分后) 低于本值的双字词剪掉；0=关')
    ap.add_argument('--no-sweep', action='store_true', help='跳过词表规模扫描（省时间）')
    ap.add_argument('--emit-min-count', type=float, default=20.0,
                    help='emit-vocab 的期望计数下限（默认 20，与旧 min_count 对齐）')
    ap.add_argument('--emit-max-len', type=int, default=2,
                    help='emit-vocab 收进词表的最大汉字数（默认 2；3=放开三字词）')
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    t_all = time.time()

    # ---------------- 抽样
    if a.all:
        train, held = load_all(a.corpus, a.heldout, a.seed)
    else:
        pool = sample_segments(a.corpus, a.lines + a.heldout, a.seed)
        if len(pool) < a.lines + a.heldout:
            sys.stderr.write('[warn] 语料段不足，实际 %d 段\n' % len(pool))
        train = pool[:a.lines]
        held = pool[a.lines:]
    train = [s for s in train if s]
    held = [s for s in held if s]
    nchar_train = sum(len(s) for s in train)
    sys.stderr.write('[data] 训练段 %d（%.1f 万汉字）· 留出段 %d\n'
                     % (len(train), nchar_train / 1e4, len(held)))

    # ---------------- 候选 + EM
    cnt, vocab, chars = count_candidates(train, a.max_len, a.min_count)
    sys.stderr.write('[cand] 候选 %d（单字 %d）\n' % (len(vocab), len(chars)))
    probs, hist = em(train, vocab, chars, a.max_len, a.iters, init_counts=cnt,
                     penalty=a.len_penalty)
    new_vocab = set(probs)
    # ---------------- 成分独立率闸门（CX-45 第二步；第三步推广到多字）
    # 2 字：PMI = log[ P(ab) / (P(a)·P(b)) ]，概率取自同一次 EM 的切分结果。
    #   实测碎片 性和 2.12 / 的原 0.77 / 是在 1.01 / 用太 -1.09；
    #   真词 拍卖 8.68 / 因为 7.74 / 时候 12.16 / 机器 6.75；边界 你好 3.23 / 我就 3.57
    #   ⇒ 只砍「能证明是碎片」的低段（默认 2.5），中间地带不动（分不开就别硬分）。
    # 3+ 字：取【所有二分切法】里最保守的那个 MI（最小值）。
    #   道理：真词（计算机）怎么切，成分概率乘积都低 ⇒ MI 高；
    #   短语块（是一个 = 是|一个）只要有一种切法能拼出高频组合，那一项 MI 就很低 ⇒ 被压掉。
    #   这正是上次 max_len=3 时「3 字词 68.9%、一半是短语」的解法。
    if a.indep_pmi > 0:
        dropped = []
        for w in list(probs):
            if len(w) < 2:
                continue
            pw = probs[w]
            if pw <= 0:
                dropped.append((w, -99.0))
                continue
            best = None
            for cut in range(1, len(w)):
                pa = probs.get(w[:cut])
                pb = probs.get(w[cut:])
                if not pa or not pb:
                    continue
                v = math.log(pw / (pa * pb))
                if best is None or v < best:
                    best = v
            if best is not None and best < a.indep_pmi:
                dropped.append((w, best))
        for w, _ in dropped:
            probs.pop(w, None)
        Z = sum(probs.values()) or 1.0
        probs = {w: p / Z for w, p in probs.items()}
        dropped.sort(key=lambda kv: -len(kv[0]))
        sys.stderr.write(('indep: MI<%.2f 剪掉 %d 词，剩 %d（例：%s）' + chr(10))
                         % (a.indep_pmi, len(dropped), len(probs),
                            ' '.join(w for w, _ in dropped[:12])))
    sys.stderr.write('[new] 收敛词表 %d 词\n' % len(new_vocab))

    with open(os.path.join(a.out, 'vocab_new.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('词\tP\t期望计数\t长度\n')
        for w, p in sorted(probs.items(), key=lambda kv: -kv[1]):
            f.write('%s\t%.10g\t%.1f\t%d\n' % (w, p, p * nchar_train, len(w)))
    with open(os.path.join(a.out, 'curve.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('iter\tlogP_per_char\tvocab\tsec\n')
        for it, lp, n, sec in hist:
            f.write('%d\t%.6f\t%d\t%.1f\n' % (it, lp, n, sec))

    # ---------------- 给 qa_retrain 的正式词表（只取双字词，按期望计数下限截断）
    if a.emit_vocab:
        n_emit = 0
        NL = chr(10)
        TAB = chr(9)
        with open(a.emit_vocab, 'w', encoding='utf-8', newline=NL) as f:
            f.write(TAB.join(['词', 'P', '期望计数', '长度']) + NL)
            for w, p in sorted(probs.items(), key=lambda kv: -kv[1]):
                if len(w) < 2 or len(w) > a.emit_max_len:
                    continue
                cnt = p * nchar_train
                if cnt < a.emit_min_count:
                    continue
                f.write(TAB.join([w, '%.10g' % p, '%.1f' % cnt, str(len(w))]) + NL)
                n_emit += 1
        sys.stderr.write('[emit] 词表 %d 词（2~%d 字 · 期望计数>=%.0f）-> %s%s'
                         % (n_emit, a.emit_max_len, a.emit_min_count, a.emit_vocab, NL))

    # ---------------- 规模扫描
    ks = [5000, 10000, 15000, 17609, 20000, 30000, 50000, len(probs)]
    sweep_rows = [] if a.no_sweep else sweep(train, held, dict(probs), a.max_len, ks, a.len_penalty)
    with open(os.path.join(a.out, 'sweep.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('K\t实际词数\tlogP_per_char\tOOV\n')
        for n, lp, oov in sweep_rows:
            f.write('%d\t%d\t%.6f\t%.6f\n' % (n, n, lp, oov))

    # ---------------- 旧词表对照
    old_lines = []
    old_vocab = set()
    old_score = None
    if a.old_vocab and os.path.exists(a.old_vocab):
        with open(a.old_vocab, encoding='utf-8', errors='replace') as f:
            for line in f:
                line = line.rstrip('\n')
                if not line or line.startswith('#'):
                    continue
                w = line.split('\t')[0].strip()
                if w and len(w) <= a.max_len:
                    old_vocab.add(w)
        old_chars = {w for w in old_vocab if len(w) == 1}
        old_gen = old_vocab - old_chars
        sys.stderr.write('[old] 词表 %d 词（单字 %d）\n' % (len(old_vocab), len(old_chars)))
        old_probs, old_hist = em(train, old_vocab, old_chars or chars, a.max_len, a.iters,
                                 penalty=a.len_penalty)
        old_lp, old_oov, _ = score(held, old_probs, a.max_len, a.len_penalty)
        old_score = (old_lp, old_oov, len(old_probs), old_hist)
        sys.stderr.write('[old] held-out logP/char=%.4f OOV=%.4f%%\n' % (old_lp, 100 * old_oov))

        added = sorted(new_vocab - old_vocab)
        removed = sorted(old_vocab - new_vocab)
        with open(os.path.join(a.out, 'added.txt'), 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(added) + '\n')
        with open(os.path.join(a.out, 'removed.txt'), 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(removed) + '\n')

    # ---------------- 报告
    new_lp, new_oov, _ = score(held, probs, a.max_len, a.len_penalty)
    dist = length_dist(new_vocab)
    lines = []
    lines.append('# CX-45 Unigram LM 原型读数\n')
    lines.append('语料: %s' % ' '.join(a.corpus))
    lines.append('抽样: seed=%d 训练 %d 段 / 留出 %d 段 / 训练汉字 %.0f'
                 % (a.seed, len(train), len(held), nchar_train))
    lines.append('参数: min_count=%d max_len=%d iters=%d len_penalty=%.2f'
                 % (a.min_count, a.max_len, a.iters, a.len_penalty))
    lines.append('')
    lines.append('## EM 收敛')
    lines.append('| iter | logP/char | 词数 | 秒 |')
    lines.append('|---|---|---|---|')
    for it, lp, n, sec in hist:
        lines.append('| %d | %.4f | %d | %.1f |' % (it, lp, n, sec))
    lines.append('')
    lines.append('## 词表规模扫描（新词表 top-K）')
    lines.append('| K | 实际词数 | logP/char | OOV |')
    lines.append('|---|---|---|---|')
    for n, lp, oov in sweep_rows:
        lines.append('| %d | %d | %.4f | %.3f%% |' % (n, n, lp, 100 * oov))
    lines.append('')
    lines.append('## held-out 主判据')
    lines.append('- 新词表: logP/char=%.4f OOV=%.4f%% 词数=%d' % (new_lp, 100 * new_oov, len(new_vocab)))
    if old_score:
        lines.append('- 旧词表: logP/char=%.4f OOV=%.4f%% 词数=%d'
                     % (old_score[0], 100 * old_score[1], old_score[2]))
        delta = new_lp - old_score[0]
        lines.append('- **差值: %+.4f**（%.2f%%）' % (delta, 100 * delta / abs(old_score[0])))
    lines.append('')
    lines.append('## 长度分布（新词表）')
    for k in range(1, 7):
        n, pct = dist[k]
        lines.append('- %d 字: %d（%.1f%%）' % (k, n, pct))
    lines.append('')
    lines.append('## 定点验收')
    for w in CHECK_DEAD:
        lines.append('- %s: %s' % (w, '在（✗ 应消失）' if w in new_vocab else '已消失（✓）'))
    for w in CHECK_ALIVE:
        lines.append('- %s: %s' % (w, '在（✓）' if w in new_vocab else '不在（✗ 应保留）'))
    if a.old_vocab and old_vocab:
        lines.append('')
        lines.append('## 差集')
        lines.append('- 新增 %d 词 / 消失 %d 词（明细见 added.txt / removed.txt）'
                     % (len(new_vocab - old_vocab), len(old_vocab - new_vocab)))
    lines.append('')
    lines.append('## 耗时 total %.1fs' % (time.time() - t_all))
    report = '\n'.join(lines) + '\n'
    with open(os.path.join(a.out, 'report.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(report)
    sys.stderr.write('\n' + report)
    return 0


if __name__ == '__main__':
    sys.exit(main())
