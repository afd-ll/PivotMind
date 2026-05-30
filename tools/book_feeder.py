#!/usr/bin/env python3
"""
书籍喂入工具 — 将书籍文本转为句子对，喂入自主学习器

流程：
1. 读取书籍文本
2. 按标点分成句子
3. 相邻句子组成 (sentence_i, sentence_{i+1}) 对
4. 写入临时文件
5. 调用 batch_learn 处理

用法：
  python3 tools/book_feeder.py <书籍.txt> [状态文件] [epochs]
  默认状态文件: pivotmind_state.dat
  epochs: 每本书喂入次数（默认1）
"""

import sys
import os
import re
import json
import subprocess
import tempfile

def split_sentences(text):
    """将中文文本分成句子"""
    # 按中文句号、问号、感叹号、省略号、换行分割
    sentences = re.split(r'[。！？…\n]+', text)
    # 过滤空串和太短的句子
    sentences = [s.strip() for s in sentences if len(s.strip()) >= 4]
    return sentences

def build_sentence_pairs(sentences):
    """相邻句子组成对"""
    pairs = []
    for i in range(len(sentences) - 1):
        s1 = sentences[i]
        s2 = sentences[i + 1]
        # 过滤太长或太短的句子对
        if len(s1) > 5 and len(s2) > 5 and len(s1) < 500 and len(s2) < 500:
            pairs.append((s1, s2))
    return pairs

def convert_to_qa_format(pairs, output_path):
    """写入为 QA 格式 JSON [['问','答'], ...]"""
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write('[\n')
        for i, (q, a) in enumerate(pairs):
            # JSON 转义
            q_escaped = json.dumps(q, ensure_ascii=False)
            a_escaped = json.dumps(a, ensure_ascii=False)
            comma = ',' if i < len(pairs) - 1 else ''
            f.write(f'  [{q_escaped}, {a_escaped}]{comma}\n')
        f.write(']\n')

def main():
    if len(sys.argv) < 2:
        print("用法: python3 tools/book_feeder.py <书籍.txt> [状态文件] [epochs]")
        sys.exit(1)
    
    book_path = sys.argv[1]
    state_path = sys.argv[2] if len(sys.argv) > 2 else "pivotmind_state.dat"
    epochs = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    
    if not os.path.exists(book_path):
        print(f"文件不存在: {book_path}")
        sys.exit(1)
    
    # 读文本
    print(f"[书籍喂入] 读取: {book_path}")
    with open(book_path, 'r', encoding='utf-8', errors='replace') as f:
        text = f.read()
    
    # 分句
    sentences = split_sentences(text)
    print(f"  句子数: {len(sentences)}")
    
    # 组对
    pairs = build_sentence_pairs(sentences)
    print(f"  句子对数: {len(pairs)}")
    
    if len(pairs) == 0:
        print("  无有效句子对，跳过")
        return
    
    # 写入临时文件
    tmp_file = f"/tmp/book_feed_{os.path.basename(book_path)}.json"
    convert_to_qa_format(pairs, tmp_file)
    print(f"  临时文件: {tmp_file} ({os.path.getsize(tmp_file)} bytes)")
    
    # 调用 batch_learn
    book_name = os.path.basename(book_path).replace('.txt', '')
    print(f"\n[书籍喂入] 喂入: {book_name} ({epochs} epoch)")
    print(f"  命令: ./build/bin/batch_learn {state_path} {tmp_file} {epochs}")
    
    result = subprocess.run(
        ['./build/bin/batch_learn', state_path, tmp_file, str(epochs)],
        capture_output=True, text=True,
        cwd=os.path.dirname(os.path.abspath(__file__)) + '/..'
    )
    
    # 提取关键行
    for line in result.stdout.split('\n'):
        if '║' in line and ('词汇拓扑' in line or '平均' in line or '总边' in line or '完成' in line or '边=' in line):
            print(f"  {line.strip()}")
    
    if result.returncode != 0:
        print(f"  错误: {result.stderr[:500]}")
        return
    
    # 清理临时文件
    os.unlink(tmp_file)
    print(f"\n[书籍喂入] ✓ {book_name} 喂入完成")


if __name__ == '__main__':
    main()
