#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import sqlite3
import os

def test_database_integrity():
    """测试数据库完整性"""
    print("Database Integrity Test")
    print("=======================\n")
    
    db_path = "cedict.db"
    if not os.path.exists(db_path):
        print(f"ERROR: Database file {db_path} not found")
        return False
    
    conn = sqlite3.connect(db_path)
    cursor = conn.cursor()
    
    # 测试1: 检查表结构
    print("1. Testing table structure...")
    try:
        cursor.execute("SELECT name FROM sqlite_master WHERE type='table'")
        tables = cursor.fetchall()
        table_names = [table[0] for table in tables]
        
        required_tables = ['dictionary', 'word_index']
        for table in required_tables:
            if table not in table_names:
                print(f"ERROR: Missing table '{table}'")
                return False
        
        print("SUCCESS: All required tables exist")
        
    except Exception as e:
        print(f"ERROR: Failed to check table structure: {e}")
        return False
    
    # 测试2: 检查数据完整性
    print("\n2. Testing data integrity...")
    try:
        cursor.execute("SELECT COUNT(*) FROM dictionary")
        dict_count = cursor.fetchone()[0]
        
        cursor.execute("SELECT COUNT(*) FROM word_index")
        index_count = cursor.fetchone()[0]
        
        print(f"Dictionary entries: {dict_count:,}")
        print(f"Word index entries: {index_count:,}")
        
        if dict_count == 0:
            print("ERROR: Dictionary table is empty")
            return False
            
        print("SUCCESS: Data integrity check passed")
        
    except Exception as e:
        print(f"ERROR: Failed to check data integrity: {e}")
        return False
    
    # 测试3: 检查索引
    print("\n3. Testing indexes...")
    try:
        cursor.execute("SELECT name FROM sqlite_master WHERE type='index'")
        indexes = cursor.fetchall()
        index_names = [idx[0] for idx in indexes]
        
        required_indexes = ['idx_simplified', 'idx_traditional', 'idx_pinyin']
        for idx in required_indexes:
            if idx not in index_names:
                print(f"ERROR: Missing index '{idx}'")
                return False
        
        print("SUCCESS: All required indexes exist")
        
    except Exception as e:
        print(f"ERROR: Failed to check indexes: {e}")
        return False
    
    # 测试4: 查询性能测试
    print("\n4. Testing query performance...")
    try:
        import time
        
        # 测试精确查询
        start_time = time.time()
        cursor.execute("SELECT * FROM dictionary WHERE simplified = '你好' LIMIT 5")
        results = cursor.fetchall()
        exact_time = time.time() - start_time
        
        # 测试模糊查询
        start_time = time.time()
        cursor.execute("SELECT * FROM dictionary WHERE simplified LIKE '海%' LIMIT 10")
        results = cursor.fetchall()
        fuzzy_time = time.time() - start_time
        
        # 测试拼音查询
        start_time = time.time()
        cursor.execute("SELECT * FROM dictionary WHERE pinyin LIKE 'xue%' LIMIT 10")
        results = cursor.fetchall()
        pinyin_time = time.time() - start_time
        
        print(f"Exact query time: {exact_time:.4f}s")
        print(f"Fuzzy query time: {fuzzy_time:.4f}s")
        print(f"Pinyin query time: {pinyin_time:.4f}s")
        
        if exact_time > 1.0 or fuzzy_time > 1.0 or pinyin_time > 1.0:
            print("WARNING: Query performance may be slow")
        else:
            print("SUCCESS: Query performance is good")
        
    except Exception as e:
        print(f"ERROR: Failed to test query performance: {e}")
        return False
    
    # 测试5: 示例查询
    print("\n5. Testing sample queries...")
    try:
        test_words = ['你好', '海豚', '学习']
        for word in test_words:
            cursor.execute(
                "SELECT traditional, simplified, pinyin, definition "
                "FROM dictionary WHERE simplified = ? OR traditional = ? LIMIT 1", 
                (word, word)
            )
            result = cursor.fetchone()
            
            if result:
                print(f"  '{word}': {result[2]} - {result[3]}")
            else:
                print(f"  '{word}': Not found")
        
        print("SUCCESS: Sample queries completed")
        
    except Exception as e:
        print(f"ERROR: Failed to run sample queries: {e}")
        return False
    
    conn.close()
    return True

def test_memory_usage():
    """测试内存使用情况"""
    print("\nMemory Usage Test")
    print("=================\n")
    
    try:
        import psutil
        import os
        
        process = psutil.Process(os.getpid())
        memory_mb = process.memory_info().rss / 1024 / 1024
        
        print(f"Current memory usage: {memory_mb:.1f} MB")
        
        # 连接数据库并执行查询
        conn = sqlite3.connect('cedict.db')
        cursor = conn.cursor()
        
        # 执行一些查询
        for word in ['你好', '海豚', '学习', '电脑', '手机']:
            cursor.execute(
                "SELECT * FROM dictionary WHERE simplified = ? OR traditional = ? LIMIT 1",
                (word, word)
            )
            cursor.fetchall()
        
        memory_after = process.memory_info().rss / 1024 / 1024
        print(f"Memory after queries: {memory_after:.1f} MB")
        print(f"Memory increase: {memory_after - memory_mb:.1f} MB")
        
        conn.close()
        
        if memory_after - memory_mb > 100:  # 超过100MB增长
            print("WARNING: High memory usage detected")
        else:
            print("SUCCESS: Memory usage is reasonable")
        
    except ImportError:
        print("SKIP: psutil not available for memory testing")
    except Exception as e:
        print(f"ERROR: Memory test failed: {e}")

if __name__ == "__main__":
    print("CC-CEDICT Database Test Suite")
    print("=" * 50)
    
    success = test_database_integrity()
    test_memory_usage()
    
    print("\n" + "=" * 50)
    if success:
        print("All tests passed! Database is ready for use.")
    else:
        print("Some tests failed. Please check the database.")