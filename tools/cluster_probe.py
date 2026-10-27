#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""cluster_probe.py — 验证「邻词分布能否自动分出词类」

背景：玄枢的 emergent_pos 用 512 维 features 算余弦做分类，但重训时 features 是
随机噪声（rand()*0.1-0.05），所以分类等于随机。而 dist_sig[26]（左右邻 POS 分布）
在重训时压根没写，线上只累积了 0-18 次。

本探针要回答一个问题：**只用「一个词旁边出现哪些字」这个信息，能不能自动分出
虚词 / 名词 / 动词 / 形容词**？如果能，方案就成立（把表示从随机特征换成邻词分布）。

方法照 Schütze (1995) 的分布聚类，简化版：
  1. 特征维度 = 频次 top F 的单字（Zipf 定律：这批几乎全是虚词/高频功能字）
  2. 每个 target 词的向量 = [左邻在特征词上的计数] + [右邻在特征词上的计数]
  3. PPMI 加权（压掉高频噪声，保留有信息的方向）
  4. 余弦 + 贪心增量聚类（leader clustering，不需要预设类数）
  5. 打印每个簇的前若干成员，人工看它像不像一个词类

用法: python3 cluster_probe.py <charpairs.bin> [F] [T] [阈值]
"""
import io
import math
import struct
import sys
from collections import defaultdict

PATH = sys.argv[1]
F = int(sys.argv[2]) if len(sys.argv) > 2 else 150     # 特征词数
T = int(sys.argv[3]) if len(sys.argv) > 3 else 400     # target 词数
TH = float(sys.argv[4]) if len(sys.argv) > 4 else 0.35  # 归入已有簇的余弦阈值


def load_pairs(path):
    data = io.open(path, "rb").read()
    magic, ver, n = struct.unpack_from("<III", data, 0)
    assert magic == 0x504D4350, "magic 不对：%x" % magic
    pos = 12
    out = []
    for _ in range(n):
        la = data[pos]; pos += 1
        a = data[pos:pos + la].decode("utf-8", "replace"); pos += la
        lb = data[pos]; pos += 1
        b = data[pos:pos + lb].decode("utf-8", "replace"); pos += lb
        c, = struct.unpack_from("<I", data, pos); pos += 4
        out.append((a, b, c))
    return ver, out


def main():
    ver, pairs = load_pairs(PATH)
    print("[读入] version=%d 对=%d" % (ver, len(pairs)))

    # 只保留单字（CJK），两边都必须单字
    def is1(s):
        return len(s) == 1 and '\u4e00' <= s <= '\u9fff'

    freq = defaultdict(int)
    left = defaultdict(lambda: defaultdict(int))
    right = defaultdict(lambda: defaultdict(int))
    kept = 0
    for a, b, c in pairs:
        if not (is1(a) and is1(b)):
            continue
        kept += 1
        freq[a] += c
        freq[b] += c
        left[b][a] += c
        right[a][b] += c
    print("[过滤] 单字对=%d 不同字=%d" % (kept, len(freq)))

    ranked = sorted(freq.items(), key=lambda kv: -kv[1])
    feats = [w for w, _ in ranked[:F]]                       # 特征词
    targets = [w for w, _ in ranked[:T]]                     # 待聚类词
    fset = {w: i for i, w in enumerate(feats)}
    print("[特征] 前 %d 频率字: %s" % (F, " ".join(feats[:40])))

    total = sum(c for _, _, c in pairs) or 1
    # 特征词自身的边缘频次（做 PPMI 用）
    f_freq = [freq[w] for w in feats]

    def vec(w):
        v = [0.0] * (2 * F)
        for nb, c in left.get(w, {}).items():
            i = fset.get(nb)
            if i is not None:
                v[i] += c
        for nb, c in right.get(w, {}).items():
            i = fset.get(nb)
            if i is not None:
                v[F + i] += c
        # PPMI
        for k in range(2 * F):
            if v[k] <= 0:
                v[k] = 0.0
                continue
            j = k % F
            pmi = math.log((v[k] * total) / (freq[w] * f_freq[j] + 1e-9) + 1e-9)
            v[k] = max(0.0, pmi)
        # L2 归一化
        nrm = math.sqrt(sum(x * x for x in v))
        if nrm > 1e-9:
            v = [x / nrm for x in v]
        return v

    def cos(u, v):
        return sum(a * b for a, b in zip(u, v))

    # 贪心增量聚类（leader clustering）
    clusters = []   # [中心向量, [成员]]
    for w in targets:
        v = vec(w)
        if sum(v) <= 1e-9:
            continue                      # 没有任何特征词邻居 ⇒ 跳过
        best, bi = -1.0, -1
        for i, (cv, _) in enumerate(clusters):
            s = cos(v, cv)
            if s > best:
                best, bi = s, i
        if bi >= 0 and best >= TH:
            clusters[bi][1].append((w, best))
            # 中心缓慢漂移（成员越多越稳 = 先入者主导，对应「第一印象」）
            n = len(clusters[bi][1])
            lr = 1.0 / (n + 2.0)
            clusters[bi][0] = [(1 - lr) * a + lr * b for a, b in zip(clusters[bi][0], v)]
            nrm = math.sqrt(sum(x * x for x in clusters[bi][0]))
            if nrm > 1e-9:
                clusters[bi][0] = [x / nrm for x in clusters[bi][0]]
        else:
            clusters.append([v, [(w, 1.0)]])

    clusters.sort(key=lambda c: -len(c[1]))
    print("\n[聚类] 得 %d 个簇（阈值 %.2f）" % (len(clusters), TH))
    print("=" * 68)
    for i, (cv, mem) in enumerate(clusters[:14]):
        names = " ".join(w for w, _ in mem[:16])
        print("簇%-2d n=%-4d %s" % (i + 1, len(mem), names))
    print("=" * 68)
    small = [c for c in clusters if len(c[1]) == 1]
    print("[单例] %d 个（占 %.0f%%）⇒ 越大说明分布区分度不足" %
          (len(small), 100.0 * len(small) / max(1, len(clusters))))

    # 关键判据：人工检查「虚词是否自己抱团」
    for probe in ["的", "了", "在", "是", "人", "吃", "大"]:
        for i, (cv, mem) in enumerate(clusters):
            if any(w == probe for w, _ in mem):
                hit = " ".join(w for w, _ in mem[:12])
                print("[定位] %s 在簇%d（n=%d）: %s" % (probe, i + 1, len(mem), hit))
                break


main()
