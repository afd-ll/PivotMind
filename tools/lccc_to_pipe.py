# -*- coding: utf-8 -*-
"""LCCC → 玄枢 pipe 格式（`问|答` 每行）。

⚠️ LCCC 是**已分词**的（jieba，空格分隔）。直接吃 token 会把 jieba 的词边界
带进来（等于用别人的分词结果训练自己的分词器）⇒ 必须去掉空格还原成连续文本。

多轮对话拆成相邻轮对：(t0,t1) (t1,t2) (t2,t3) …，与 qa_to_pipe 的口径一致。

用法：
  python3 lccc_to_pipe.py <lccc.jsonl.gz> <out.txt> [最多取多少行，0=全量]
"""
import io, sys, json, gzip

src, dst = sys.argv[1], sys.argv[2]
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 0

n_lines = n_pairs = n_skip = 0
qlen = alen = 0
op = gzip.open if src.endswith(".gz") else io.open
with op(src, "rt", encoding="utf-8", errors="replace") as f, \
     io.open(dst, "w", encoding="utf-8", newline="\n") as out:
    for line in f:
        line = line.strip()
        if not line:
            continue
        try:
            turns = json.loads(line)
        except ValueError:
            n_skip += 1
            continue
        if not isinstance(turns, list) or len(turns) < 2:
            n_skip += 1
            continue
        clean = [t.replace(" ", "").strip() for t in turns if isinstance(t, str)]
        clean = [t for t in clean if t]
        if len(clean) < 2:
            n_skip += 1
            continue
        for i in range(len(clean) - 1):
            q, a = clean[i], clean[i + 1]
            # 太短的不进（单字/符号，对转移模型只是噪声）
            if len(q) < 2 or len(a) < 2:
                continue
            out.write("%s|%s\n" % (q, a))
            n_pairs += 1
            qlen += len(q)
            alen += len(a)
        n_lines += 1
        if limit and n_lines >= limit:
            break

print("读入对话 %d 条，产出轮对 %d（跳过 %d 条坏数据）" % (n_lines, n_pairs, n_skip))
if n_pairs:
    print("平均长度 问=%.1f 答=%.1f 汉字" % (qlen / n_pairs, alen / n_pairs))
