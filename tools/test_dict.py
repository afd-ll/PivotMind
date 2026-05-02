#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import sqlite3

def test_dictionary():
    """测试词典数据库功能"""
    conn = sqlite3.connect('cedict.db')
    cursor = conn.cursor()
    
    print("CC-CEDICT Database Test")
    print("=" * 50)
    
    # 基本统计
    cursor.execute('SELECT COUNT(*) FROM dictionary')
    total_entries = cursor.fetchone()[0]
    print(f"Total entries: {total_entries:,}")
    
    cursor.execute('SELECT COUNT(*) FROM word_index')
    unique_words = cursor.fetchone()[0]
    print(f"Unique words: {unique_words:,}")
    
    # 测试查询功能
    print("\nTest Query Function:")
    
    # 查询特定词
    test_words = ['你好', '海豚', '学习', '电脑']
    for word in test_words:
        cursor.execute('''
        SELECT traditional, simplified, pinyin, definition 
        FROM dictionary 
        WHERE simplified = ? OR traditional = ?
        LIMIT 3
        ''', (word, word))
        
        results = cursor.fetchall()
        print(f"\nQuery '{word}':")
        for result in results:
            print(f"  {result[0]} {result[1]} [{result[2]}] /{result[3]}/")
    
    # 测试模糊查询
    print("\nTest Fuzzy Query:")
    cursor.execute('''
    SELECT simplified, pinyin, definition 
    FROM dictionary 
    WHERE simplified LIKE '海%' 
    LIMIT 5
    ''')
    
    results = cursor.fetchall()
    print("\nWords starting with '海':")
    for result in results:
        print(f"  {result[0]} [{result[1]}] /{result[2]}/")
    
    # 测试拼音查询
    print("\nTest Pinyin Query:")
    cursor.execute('''
    SELECT simplified, pinyin, definition 
    FROM dictionary 
    WHERE pinyin LIKE 'hai%' 
    LIMIT 5
    ''')
    
    results = cursor.fetchall()
    print("\nWords with pinyin starting with 'hai':")
    for result in results:
        print(f"  {result[0]} [{result[1]}] /{result[2]}/")
    
    conn.close()

def create_c_interface():
    """创建C语言接口文件"""
    c_header = '''
#ifndef DICTIONARY_H
#define DICTIONARY_H

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int id;
    char* traditional;
    char* simplified;
    char* pinyin;
    char* definition;
    char* category;
    int frequency;
} DictionaryEntry;

typedef struct {
    sqlite3* db;
    sqlite3_stmt* search_stmt;
    sqlite3_stmt* fuzzy_stmt;
    sqlite3_stmt* pinyin_stmt;
} DictionaryDB;

// 初始化词典数据库
DictionaryDB* dict_init(const char* db_path);

// 关闭词典数据库
void dict_close(DictionaryDB* dict);

// 精确查询
DictionaryEntry* dict_search(DictionaryDB* dict, const char* word, int* count);

// 模糊查询（前缀匹配）
DictionaryEntry* dict_search_prefix(DictionaryDB* dict, const char* prefix, int* count);

// 拼音查询
DictionaryEntry* dict_search_pinyin(DictionaryDB* dict, const char* pinyin, int* count);

// 释放查询结果
void dict_free_results(DictionaryEntry* entries, int count);

#endif // DICTIONARY_H
'''
    
    with open('dictionary.h', 'w', encoding='utf-8') as f:
        f.write(c_header)
    
    print("\nCreated C header file: dictionary.h")

if __name__ == "__main__":
    test_dictionary()
    create_c_interface()