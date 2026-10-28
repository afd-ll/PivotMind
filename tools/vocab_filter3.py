#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""vocab_filter3.py — 三字词的三重闸门（CX-45 第三步）

背景：max_len=3 时 EM 收敛出 16.6 万词，直接 emit 是 9.1 万（3 字占 6.3 万），
里面有三种东西混在一起，任何单一统计量都分不开（实测见 CX-45 记录）：

  1) 真词：计算机 MI 9.43/熵 3.73 · 图书馆 12.38/3.56 · 天安门 10.77/1.82 · 大学生 4.48/3.32
  2) 切偏块：工智能 10.18/**0.00** · 文回答 9.58/**0.01** · 拉格朗 12.56/**0.00**
     —— MI 很高（成分都罕见），只有「词整体左右邻分散度」能认出来
  3) 虚词粘连：是一个 3.63/4.20 · 自己的 3.69/4.83 · 之间的 4.74/4.48
     —— 熵很高（位置很自由），只有 MI 能压住

所以三道闸门是**互补**的，必须一起上：
  A. MI ≥ 4.5   —— 对最优二分切法的互信息（压虚词粘连）
  B. min熵 ≥ 1.0 —— 词整体左右邻熵（压切偏块）；由 word_ctx_batch.py 实测提供
  C. 虚词守卫   —— 3 字词首/尾是封闭类虚词字、且剩余部分是词 ⇒ 拒
                   （补 A 的边界盲区：`有一个` MI 4.48 与真词 `大学生` 4.48 撞车）

用法（两阶段，因为 B 需要扫语料）：
  # 阶段 1：出待测熵的候选
  python3 vocab_filter3.py --vocab vocab_new.tsv --emit-candidates /tmp/cand3.txt
  # 阶段 2：喂 word_ctx_batch 的结果，出最终词表
  python3 vocab_filter3.py --vocab vocab_new.tsv --ctx /tmp/cand3_ctx.tsv \
      --base2 /tmp/base2.tsv --out /tmp/vocab_final.tsv

产出格式与 qa_retrain 的 EM 白名单一致：词 \t P \t 期望计数 \t 长度
"""
import argparse
import io
import math
import sys

NL = chr(10)
TAB = chr(9)

# 封闭类虚词字（刻意排除 中/上/下/里/大/小 —— 它们是常见词首，会把 中国/大学 误杀）
FUNC = set(u'的了是在有和与就都也很不没我你他她它个们会能要把被给对从而但又还再才只更最太挺吗呢吧啊呀嘛')


def load_probs(path):
    """读 EM 词表：词 \t P \t 期望计数 \t 长度（容忍头部垃圾行与缺列）"""
    lines = io.open(path, encoding='utf-8', errors='replace').read().split(NL)
    start = 0
    for i, l in enumerate(lines[:8]):
        if l.startswith(u'词' + TAB):
            start = i
            break
    probs, counts = {}, {}
    for l in lines[start + 1:]:
        p = l.split(TAB)
        if len(p) < 3 or not p[0]:
            continue
        try:
            probs[p[0]] = float(p[1])
            counts[p[0]] = float(p[2])
        except ValueError:
            continue
    return probs, counts


def load_base2(path):
    """现有双字词表（已上线验证过的那份）：词 \t P \t 计数 \t 长度"""
    out = {}
    if not path:
        return out
    lines = io.open(path, encoding='utf-8', errors='replace').read().split(NL)
    start = 0
    for i, l in enumerate(lines[:8]):
        if l.startswith(u'词' + TAB):
            start = i
            break
    for l in lines[start + 1:]:
        p = l.split(TAB)
        if len(p) >= 4 and len(p[0]) == 2:
            try:
                out[p[0]] = (float(p[1]), float(p[2]))
            except ValueError:
                continue
    return out


def load_ctx(path):
    """word_ctx_batch 输出：词 n 左熵 右熵 maxL% maxR% bd% Ltop3 Rtop3"""
    out = {}
    lines = io.open(path, encoding='utf-8', errors='replace').read().split(NL)
    start = 0
    for i, l in enumerate(lines[:8]):
        if l.startswith(u'词' + TAB):
            start = i
            break
    for l in lines[start + 1:]:
        p = l.split(TAB)
        if len(p) < 7 or p[1] == '-':
            continue
        try:
            out[p[0]] = (int(p[1]), float(p[2]), float(p[3]))
        except ValueError:
            continue
    return out


def mi_split(w, probs):
    """对最优二分切法的 MI（取所有切法里最小的那个：最保守）"""
    pw = probs.get(w)
    if not pw:
        return None
    best = None
    for cut in range(1, len(w)):
        pa = probs.get(w[:cut])
        pb = probs.get(w[cut:])
        if not pa or not pb:
            continue
        v = math.log(pw / (pa * pb))
        if best is None or v < best:
            best = v
    return best


def func_guard(w, probs):
    """3 字词首/尾是虚词字，且剩余部分是词 ⇒ 判为跨界粘连。返回 True = 拒"""
    if len(w) != 3:
        return False
    if w[0] in FUNC and probs.get(w[1:], 0.0) > 1e-4:
        return True
    if w[2] in FUNC and probs.get(w[:2], 0.0) > 1e-4:
        return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--vocab', required=True, help='EM 全量词表（含单字）')
    ap.add_argument('--base2', default=None, help='现有双字词表（原样并入）')
    ap.add_argument('--ctx', default=None, help='word_ctx_batch 输出的熵表')
    ap.add_argument('--min-pmi', type=float, default=4.5)
    ap.add_argument('--min-ent', type=float, default=1.0)
    ap.add_argument('--min-count', type=float, default=20.0)
    ap.add_argument('--emit-candidates', default=None)
    ap.add_argument('--out', default=None)
    a = ap.parse_args()

    probs, counts = load_probs(a.vocab)
    sys.stderr.write('[load] EM 词表 %d 项' % len(probs) + NL)

    # 阶段 1：3 字候选（MI + 虚词守卫）
    cand, rej_mi, rej_func = [], 0, 0
    for w, p in probs.items():
        if len(w) != 3:
            continue
        if counts.get(w, 0) < a.min_count:
            continue
        m = mi_split(w, probs)
        if m is None or m < a.min_pmi:
            rej_mi += 1
            continue
        if func_guard(w, probs):
            rej_func += 1
            continue
        cand.append(w)
    sys.stderr.write(('[stage1] 3字候选 %d（MI<%.1f 拒 %d · 虚词守卫拒 %d）' + NL)
                     % (len(cand), a.min_pmi, rej_mi, rej_func))

    if a.emit_candidates:
        io.open(a.emit_candidates, 'w', encoding='utf-8', newline=NL).write(
            NL.join(cand) + NL)
        sys.stderr.write('[stage1] 候选已写入 %s' % a.emit_candidates + NL)
        if not a.out:
            return 0

    # 阶段 2：并用熵闸门，出最终词表
    if not a.out:
        return 0
    ctx = load_ctx(a.ctx) if a.ctx else {}
    base2 = load_base2(a.base2)
    keep3, rej_ent = [], 0
    for w in cand:
        st = ctx.get(w)
        if not st:
            rej_ent += 1
            continue
        n, hl, hr = st
        if n < a.min_count or min(hl, hr) < a.min_ent:
            rej_ent += 1
            continue
        keep3.append(w)

    rows = []
    for w, (p, c) in base2.items():
        rows.append((w, p, c, 2))
    for w in keep3:
        rows.append((w, probs[w], counts[w], 3))
    rows.sort(key=lambda r: -r[1])
    with io.open(a.out, 'w', encoding='utf-8', newline=NL) as f:
        f.write(TAB.join([u'词', u'P', u'期望计数', u'长度']) + NL)
        for w, p, c, L in rows:
            f.write(TAB.join([w, '%.10g' % p, '%.1f' % c, str(L)]) + NL)

    sys.stderr.write(('[stage2] 熵<%.1f 或被拒 %d · 3字保留 %d · 双字 %d · 合计 %d'
                      + NL) % (a.min_ent, rej_ent, len(keep3), len(base2), len(rows)))
    d = {}
    for r in rows:
        d[len(r[0])] = d.get(len(r[0]), 0) + 1
    sys.stderr.write('[stage2] 长度分布 %s' % d + NL)
    return 0


if __name__ == '__main__':
    sys.exit(main())
