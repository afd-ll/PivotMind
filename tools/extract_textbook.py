#!/usr/bin/env python3
"""
人教版教材PDF文本提取工具
用途：从下载的PDF教材中提取文本内容用于AI训练

使用方法：
1. 从官网下载人教版教材PDF：
   - https://www.dzkbw.org/stage/rjb/xx/yuwen.html
   - https://jc.pep.com.cn/
   
2. 将PDF放到 data/textbooks/ 目录

3. 运行此脚本提取文本：
   python tools/extract_textbook.py
   
4. 输出格式：
   - data/textbook_grade1.txt
   - data/textbook_grade2.txt
   - ...
"""

import os
import sys
import json
import re
from pathlib import Path

try:
    import fitz  # PyMuPDF
except ImportError:
    print("=" * 60)
    print("错误: 需要安装 PyMuPDF 库")
    print("=" * 60)
    print("\n安装方法:")
    print("  pip install PyMuPDF")
    print("\n或者使用pdfplumber (备选方案):")
    print("  pip install pdfplumber")
    print("=" * 60)
    sys.exit(1)

# 教材配置
TEXTBOOK_CONFIG = {
    "grade1_up": {
        "name": "一年级上册",
        "file_pattern": "*一年级*上册*.pdf",
        "output": "data/textbook_grade1_up.txt"
    },
    "grade1_down": {
        "name": "一年级下册", 
        "file_pattern": "*一年级*下册*.pdf",
        "output": "data/textbook_grade1_down.txt"
    },
    "grade2_up": {
        "name": "二年级上册",
        "file_pattern": "*二年级*上册*.pdf", 
        "output": "data/textbook_grade2_up.txt"
    },
    "grade2_down": {
        "name": "二年级下册",
        "file_pattern": "*二年级*下册*.pdf",
        "output": "data/textbook_grade2_down.txt"
    },
    "grade3_up": {
        "name": "三年级上册",
        "file_pattern": "*三年级*上册*.pdf",
        "output": "data/textbook_grade3_up.txt"
    },
    "grade3_down": {
        "name": "三年级下册",
        "file_pattern": "*三年级*下册*.pdf",
        "output": "data/textbook_grade3_down.txt"
    },
    "grade4_up": {
        "name": "四年级上册",
        "file_pattern": "*四年级*上册*.pdf",
        "output": "data/textbook_grade4_up.txt"
    },
    "grade4_down": {
        "name": "四年级下册",
        "file_pattern": "*四年级*下册*.pdf",
        "output": "data/textbook_grade4_down.txt"
    },
    "grade5_up": {
        "name": "五年级上册",
        "file_pattern": "*五年级*上册*.pdf",
        "output": "data/textbook_grade5_up.txt"
    },
    "grade5_down": {
        "name": "五年级下册",
        "file_pattern": "*五年级*下册*.pdf",
        "output": "data/textbook_grade5_down.txt"
    },
    "grade6_up": {
        "name": "六年级上册",
        "file_pattern": "*六年级*上册*.pdf",
        "output": "data/textbook_grade6_up.txt"
    },
    "grade6_down": {
        "name": "六年级下册",
        "file_pattern": "*六年级*下册*.pdf",
        "output": "data/textbook_grade6_down.txt"
    }
}


def clean_text(text):
    """清理提取的文本"""
    # 移除多余空白
    text = re.sub(r'\n\s*\n', '\n\n', text)
    # 移除页码
    text = re.sub(r'\n\d+\n', '\n', text)
    # 移除特殊字符
    text = re.sub(r'[\x00-\x08\x0b\x0c\x0e-\x1f\x7f-\x9f]', '', text)
    return text.strip()


def extract_text_from_pdf(pdf_path):
    """从PDF提取文本"""
    print(f"\n正在处理: {pdf_path.name}")
    
    try:
        doc = fitz.open(str(pdf_path))
        full_text = []
        
        for page_num, page in enumerate(doc, 1):
            # 提取文本
            text = page.get_text()
            
            if text.strip():
                full_text.append(f"\n--- 第{page_num}页 ---\n")
                full_text.append(text)
                
            # 显示进度
            if page_num % 10 == 0:
                print(f"  已处理 {page_num}/{len(doc)} 页")
        
        doc.close()
        
        result = ''.join(full_text)
        result = clean_text(result)
        
        print(f"  ✅ 提取完成: {len(result)} 字符")
        return result
        
    except Exception as e:
        print(f"  ❌ 提取失败: {e}")
        return ""


def find_textbook_files(directory):
    """查找教材PDF文件"""
    pdf_files = list(directory.glob("*.pdf"))
    
    if not pdf_files:
        return {}
    
    matched = {}
    for grade_key, config in TEXTBOOK_CONFIG.items():
        pattern = config["file_pattern"]
        matches = [f for f in pdf_files if Path(f.name).match(pattern)]
        
        if matches:
            matched[grade_key] = matches[0]
            print(f"找到 {config['name']}: {matches[0].name}")
    
    return matched


def create_training_data(output_dir):
    """创建训练数据文件"""
    output_dir.mkdir(parents=True, exist_ok=True)
    
    # 创建README
    readme = output_dir / "README.md"
    with open(readme, 'w', encoding='utf-8') as f:
        f.write("""# 人教版教材数据目录

## 下载教材

从以下网站下载人教版教材PDF：

1. **电子课本网**: https://www.dzkbw.org/stage/rjb/xx/yuwen.html
2. **人教社官方**: https://jc.pep.com.cn/
3. **中小学课本网**: https://www.kebenwang.cn/books/rjb

## 使用方法

1. 下载教材PDF文件到此目录
2. 运行提取脚本: `python tools/extract_textbook.py`
3. 生成的文本文件用于训练

## 文件命名规范

- `*一年级*上册*.pdf` → textbook_grade1_up.txt
- `*一年级*下册*.pdf` → textbook_grade1_down.txt
- ... (以此类推)

## 版权说明

人教版教材版权归人民教育出版社所有
仅用于教育和研究目的
""")
    
    print(f"\n✅ 已创建目录和说明文件: {output_dir}")


def main():
    """主函数"""
    print("=" * 60)
    print("人教版教材PDF文本提取工具")
    print("=" * 60)
    
    # 设置目录
    base_dir = Path(__file__).parent.parent
    textbook_dir = base_dir / "data" / "textbooks"
    output_dir = base_dir / "data"
    
    # 创建目录
    create_training_data(textbook_dir)
    
    # 查找PDF文件
    print("\n查找教材PDF文件...")
    matched_files = find_textbook_files(textbook_dir)
    
    if not matched_files:
        print("\n" + "=" * 60)
        print("未找到教材PDF文件")
        print("=" * 60)
        print("\n请按以下步骤操作:")
        print("1. 访问网站下载人教版教材PDF:")
        print("   - https://www.dzkbw.org/stage/rjb/xx/yuwen.html")
        print("   - https://jc.pep.com.cn/")
        print(f"2. 将下载的PDF文件放到: {textbook_dir}")
        print("3. 重新运行此脚本")
        print("=" * 60)
        return
    
    # 提取文本
    print("\n" + "=" * 60)
    print("开始提取文本...")
    print("=" * 60)
    
    stats = {}
    for grade_key, pdf_path in matched_files.items():
        config = TEXTBOOK_CONFIG[grade_key]
        
        # 提取文本
        text = extract_text_from_pdf(pdf_path)
        
        if text:
            # 保存文件
            output_path = base_dir / config["output"]
            output_path.parent.mkdir(parents=True, exist_ok=True)
            
            with open(output_path, 'w', encoding='utf-8') as f:
                f.write(f"# {config['name']} - 人教版语文教材\n")
                f.write(f"# 来源: {pdf_path.name}\n\n")
                f.write(text)
            
            stats[grade_key] = {
                "name": config["name"],
                "chars": len(text),
                "output": str(output_path)
            }
    
    # 打印统计
    print("\n" + "=" * 60)
    print("提取完成统计")
    print("=" * 60)
    
    total_chars = 0
    for grade_key, info in stats.items():
        print(f"{info['name']}: {info['chars']:,} 字符")
        total_chars += info['chars']
    
    print(f"\n总计: {total_chars:,} 字符")
    print(f"输出目录: {output_dir}")
    
    # 保存统计信息
    stats_file = output_dir / "extraction_stats.json"
    with open(stats_file, 'w', encoding='utf-8') as f:
        json.dump(stats, f, ensure_ascii=False, indent=2)
    
    print(f"统计信息: {stats_file}")
    print("=" * 60)


if __name__ == "__main__":
    main()
