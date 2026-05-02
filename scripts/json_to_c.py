#!/usr/bin/env python3
"""把知识库转为C数组格式"""
import json
import os

INPUT_FILE = "data/knowledge_base.json"
OUTPUT_FILE = "data/knowledge_qa.c"

def main():
    with open(INPUT_FILE, 'r', encoding='utf-8') as f:
        qa_list = json.load(f)

    with open(OUTPUT_FILE, 'w', encoding='utf-8') as f:
        f.write("const char* qa_pairs[][2] = {\n")
        for q, a in qa_list:
            f.write(f'    {{"{q}", "{a}"}},\n')
        f.write("};\n")
        f.write(f"int qa_count = {len(qa_list)};\n")

    print(f"已生成 {OUTPUT_FILE}")
    print(f"共 {len(qa_list)} 个问答对")

if __name__ == "__main__":
    main()
