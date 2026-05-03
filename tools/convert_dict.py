#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import sqlite3
import re
import os

def create_database():
    """创建SQLite数据库和表结构"""
    db_path = "cedict.db"
    conn = sqlite3.connect(db_path)
    cursor = conn.cursor()
    
    # 创建词典表
    cursor.execute('''
    CREATE TABLE IF NOT EXISTS dictionary (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        traditional TEXT NOT NULL,      -- 繁体字
        simplified TEXT NOT NULL,       -- 简体字
        pinyin TEXT NOT NULL,           -- 拼音
        definition TEXT NOT NULL,       -- 英文释义
        category TEXT DEFAULT 'general', -- 词性分类
        frequency INTEGER DEFAULT 1,    -- 词频权重
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    )
    ''')
    
    # 创建索引以提高查询性能
    cursor.execute('CREATE INDEX IF NOT EXISTS idx_simplified ON dictionary(simplified)')
    cursor.execute('CREATE INDEX IF NOT EXISTS idx_traditional ON dictionary(traditional)')
    cursor.execute('CREATE INDEX IF NOT EXISTS idx_pinyin ON dictionary(pinyin)')
    cursor.execute('CREATE INDEX IF NOT EXISTS idx_category ON dictionary(category)')
    
    conn.commit()
    return conn

def parse_cedict_line(line):
    """解析CC-CEDICT单行数据"""
    # 移除行尾的换行符
    line = line.strip()
    
    # 跳过注释行和空行
    if not line or line.startswith('#'):
        return None
    
    # 使用正则表达式解析CC-CEDICT格式
    # 格式：繁体 简体 [拼音] /释义/
    pattern = r'^([^\s]+)\s+([^\s]+)\s+\[([^\]]+)\]\s*/(.+)/$'
    match = re.match(pattern, line)
    
    if not match:
        print(f"无法解析行: {line}")
        return None
    
    traditional = match.group(1)
    simplified = match.group(2)
    pinyin = match.group(3)
    definition = match.group(4)
    
    return {
        'traditional': traditional,
        'simplified': simplified,
        'pinyin': pinyin,
        'definition': definition
    }

def import_cedict_data(conn, cedict_file):
    """导入CC-CEDICT数据到数据库"""
    cursor = conn.cursor()
    count = 0
    
    print("开始导入词典数据...")
    
    try:
        with open(cedict_file, 'r', encoding='utf-8') as f:
            for line in f:
                entry = parse_cedict_line(line)
                if entry:
                    cursor.execute('''
                    INSERT INTO dictionary (traditional, simplified, pinyin, definition)
                    VALUES (?, ?, ?, ?)
                    ''', (entry['traditional'], entry['simplified'], 
                          entry['pinyin'], entry['definition']))
                    count += 1
                    
                    # 每1000条提交一次
                    if count % 1000 == 0:
                        conn.commit()
                        print(f"已导入 {count} 条记录...")
        
        conn.commit()
        print(f"词典数据导入完成！总共导入 {count} 条记录。")
        
    except Exception as e:
        print(f"导入数据时出错: {e}")
        conn.rollback()

def create_search_tables(conn):
    """创建用于分词的辅助表"""
    cursor = conn.cursor()
    
    # 创建分词索引表
    cursor.execute('''
    CREATE TABLE IF NOT EXISTS word_index (
        word TEXT PRIMARY KEY,
        entry_ids TEXT,  -- 逗号分隔的词典ID列表
        word_length INTEGER,
        is_chinese BOOLEAN
    )
    ''')
    
    # 填充分词索引表
    cursor.execute('SELECT id, simplified, LENGTH(simplified) FROM dictionary')
    entries = cursor.fetchall()
    
    print("创建分词索引...")
    for entry_id, word, length in entries:
        # 判断是否为中文字符
        is_chinese = all('\u4e00' <= char <= '\u9fff' for char in word)
        
        cursor.execute('''
        INSERT OR REPLACE INTO word_index (word, entry_ids, word_length, is_chinese)
        VALUES (?, ?, ?, ?)
        ''', (word, str(entry_id), length, is_chinese))
    
    conn.commit()
    print("分词索引创建完成！")

def main():
    """主函数"""
    print("CC-CEDICT 转 SQLite 工具")
    print("=" * 50)
    
    # 检查源文件
    cedict_file = "cedict_ts.u8"
    if not os.path.exists(cedict_file):
        print(f"错误：找不到词典文件 {cedict_file}")
        return
    
    print(f"源文件: {cedict_file}")
    print(f"文件大小: {os.path.getsize(cedict_file):,} 字节")
    
    # 创建数据库
    print("\n创建数据库...")
    conn = create_database()
    
    # 导入数据
    import_cedict_data(conn, cedict_file)
    
    # 创建索引
    create_search_tables(conn)
    
    # 显示统计信息
    cursor = conn.cursor()
    cursor.execute('SELECT COUNT(*) FROM dictionary')
    total_entries = cursor.fetchone()[0]
    
    cursor.execute('SELECT COUNT(*) FROM word_index')
    unique_words = cursor.fetchone()[0]
    
    print("\n" + "=" * 50)
    print("转换完成！")
    print(f"词典条目总数: {total_entries:,}")
    print(f"唯一词数: {unique_words:,}")
    print(f"数据库文件: cedict.db")
    
    conn.close()

if __name__ == "__main__":
    main()