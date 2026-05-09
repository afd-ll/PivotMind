#!/usr/bin/env python3
"""
批量书籍喂入 — 处理所有书籍，混合后一次喂入
每本书取最多 N 对句子，保证多样性
"""
import json, os, re, sys, subprocess

BOOK_DIR = os.path.expanduser("~/本地书库")
MAX_PAIRS_PER_BOOK = 3000  # 每本书最多取3000对
COMBINED_OUT = "/tmp/all_books_feed.json"
STATE_FILE = "pivotmind_state.dat"

def split_sentences(text):
    sentences = re.split(r'[。！？…\n]+', text)
    return [s.strip() for s in sentences if len(s.strip()) >= 4]

def build_pairs(sentences, max_pairs):
    pairs = []
    for i in range(len(sentences) - 1):
        if len(pairs) >= max_pairs:
            break
        s1, s2 = sentences[i], sentences[i+1]
        if len(s1) > 5 and len(s2) > 5 and len(s1) < 500 and len(s2) < 500:
            pairs.append([s1, s2])
    return pairs

def main():
    # 获取所有txt书
    books = sorted([f for f in os.listdir(BOOK_DIR) if f.endswith('.txt')])
    print(f"找到 {len(books)} 本书")
    
    all_pairs = []
    
    # 先加入QA数据(高优先级)
    qa_path = "data/hermes_knowledge_base.json"
    if os.path.exists(qa_path):
        qa = json.load(open(qa_path))
        all_pairs.extend(qa)
        print(f"  QA数据: {len(qa)} 对")
    
    for book in books:
        path = os.path.join(BOOK_DIR, book)
        size_kb = os.path.getsize(path) / 1024
        print(f"  处理 {book} ({size_kb:.0f}KB)...", end=" ")
        
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            text = f.read()
        
        sentences = split_sentences(text)
        pairs = build_pairs(sentences, MAX_PAIRS_PER_BOOK)
        all_pairs.extend(pairs)
        print(f"{len(pairs)} 对")
    
    # 去重
    seen = set()
    unique = []
    for q, a in all_pairs:
        key = q + '|' + a
        if key not in seen:
            seen.add(key)
            unique.append([q, a])
    
    print(f"\n总计: {len(unique)} 对 (去重后)")
    
    # 写出
    with open(COMBINED_OUT, 'w', encoding='utf-8') as f:
        json.dump(unique, f, ensure_ascii=False)
    
    size_mb = os.path.getsize(COMBINED_OUT) / (1024*1024)
    print(f"写入: {COMBINED_OUT} ({size_mb:.1f}MB)")
    
    # 喂入
    print(f"\n喂入 batch_learn...")
    result = subprocess.run(
        ['./build/bin/batch_learn', STATE_FILE, COMBINED_OUT, '1'],
        capture_output=True, text=True,
        cwd=os.path.dirname(os.path.abspath(__file__)) + '/..'
    )
    
    for line in result.stdout.split('\n'):
        if '║' in line and any(k in line for k in ['词汇拓扑', '平均', '总边', '完成', '节点']):
            print(f"  {line.strip()}")
    
    if result.returncode != 0:
        print(f"错误: {result.stderr[:300]}")
        sys.exit(1)
    
    os.unlink(COMBINED_OUT)
    print("\n✓ 全部书籍喂入完成")

if __name__ == '__main__':
    main()
