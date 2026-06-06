#!/usr/bin/env python3
"""下载中文训练语料 - 使用urllib (无需外部依赖)"""

import os, sys, gzip, re
from pathlib import Path
from urllib.request import urlopen, Request
from urllib.error import URLError

DATA_DIR = Path("D:/work/玄枢-pivotmind/data")

def download(url, dest, desc):
    if dest.exists():
        sz = dest.stat().st_size
        if sz > 0:
            print(f"  [{desc}] 已存在 ({sz/1024/1024:.1f}MB)")
            return True
    print(f"  [{desc}] 下载中...")
    try:
        req = Request(url, headers={'User-Agent': 'Mozilla/5.0'})
        with urlopen(req, timeout=120) as resp:
            data = resp.read()
            dest.write_bytes(data)
            print(f"  [{desc}] OK ({len(data)/1024/1024:.1f}MB)")
            return True
    except Exception as e:
        print(f"  [{desc}] FAILED: {e}")
        return False

# ========== 四大名著（古腾堡）==========
print("\n=== 四大名著 ===")
novel_dir = DATA_DIR / "corpora" / "multigenre-chinesenovel"
novel_dir.mkdir(exist_ok=True, parents=True)

novels = [
    ("https://www.gutenberg.org/cache/epub/25225/pg25225.txt", novel_dir/"sanguo.txt", "三国演义"),
    ("https://www.gutenberg.org/cache/epub/23950/pg23950.txt", novel_dir/"xiyouji.txt", "西游记"),
    ("https://www.gutenberg.org/cache/epub/24261/pg24261.txt", novel_dir/"hongloumeng.txt", "红楼梦"),
    ("https://www.gutenberg.org/cache/epub/24066/pg24066.txt", novel_dir/"shuihuzhuan.txt", "水浒传"),
]
for url, dest, name in novels:
    download(url, dest, name)

# ========== 解压 LCCC ==========
print("\n=== LCCC 对话解压 ===")
lccc_gz = DATA_DIR / "lccc_base_train.jsonl.gz"
lccc_txt = DATA_DIR / "lccc_base_train.jsonl"
if lccc_gz.exists() and not lccc_txt.exists():
    import gzip
    print("  解压中...")
    with gzip.open(lccc_gz, 'rb') as fin:
        data = fin.read()
    lccc_txt.write_bytes(data)
    print(f"  Done ({len(data)/1024/1024:.1f}MB)")

# ========== 统计 ==========
print("\n=== 语料总览 ===")
total = 0
for f in sorted(DATA_DIR.rglob("*")):
    if f.is_file():
        sz = f.stat().st_size
        if sz > 1024:
            total += sz
            print(f"  {f.relative_to(DATA_DIR)}: {sz/1024/1024:.1f}MB")
print(f"\n总计: {total/1024/1024:.0f}MB ({total/1024/1024/1024:.1f}GB)")
print("Done.")
