#!/usr/bin/env python3
"""
人教版小学语文教材自动下载工具

用途：从电子课本网下载人教版小学语文教材PDF（1-6年级上下册）

使用方法：
    python tools/download_textbooks.py              # 下载所有教材
    python tools/download_textbooks.py --grade 1   # 只下载一年级
    python tools/download_textbooks.py --grades 1,2,3  # 下载指定年级

数据来源：电子课本网 (https://www.dzkbw.org)
版权说明：人教版教材版权归人民教育出版社所有，仅用于教育和研究目的
"""

import os
import sys
import re
import time
import argparse
from pathlib import Path
from urllib.parse import urljoin, urlparse

try:
    import requests
    from bs4 import BeautifulSoup
except ImportError:
    print("=" * 60)
    print("错误: 需要安装依赖库")
    print("=" * 60)
    print("\n安装方法:")
    print("  pip install requests beautifulsoup4")
    print("=" * 60)
    sys.exit(1)


# 教材下载配置
TEXTBOOK_DOWNLOAD_CONFIG = {
    1: {
        "up": {
            "name": "一年级语文上册",
            "url": "https://www.dzkbw.org/books/2423.html",  # 2024秋版
            "filename": "一年级语文上册.pdf"
        },
        "down": {
            "name": "一年级语文下册",
            "url": "https://www.dzkbw.org/books/2624.html",  # 2026春版
            "filename": "一年级语文下册.pdf"
        }
    },
    2: {
        "up": {
            "name": "二年级语文上册",
            "url": "https://www.dzkbw.org/books/2550.html",  # 2025秋版
            "filename": "二年级语文上册.pdf"
        },
        "down": {
            "name": "二年级语文下册",
            "url": "https://www.dzkbw.org/books/2197.html",  # 部编版
            "filename": "二年级语文下册.pdf"
        }
    },
    3: {
        "up": {
            "name": "三年级语文上册",
            "url": "https://www.dzkbw.org/books/2551.html",  # 2025秋版
            "filename": "三年级语文上册.pdf"
        },
        "down": {
            "name": "三年级语文下册",
            "url": "https://www.dzkbw.org/books/2198.html",  # 部编版
            "filename": "三年级语文下册.pdf"
        }
    },
    4: {
        "up": {
            "name": "四年级语文上册",
            "url": "https://www.dzkbw.org/books/2199.html",  # 部编版
            "filename": "四年级语文上册.pdf"
        },
        "down": {
            "name": "四年级语文下册",
            "url": "https://www.dzkbw.org/books/2200.html",  # 部编版
            "filename": "四年级语文下册.pdf"
        }
    },
    5: {
        "up": {
            "name": "五年级语文上册",
            "url": "https://www.dzkbw.org/books/2201.html",  # 部编版
            "filename": "五年级语文上册.pdf"
        },
        "down": {
            "name": "五年级语文下册",
            "url": "https://www.dzkbw.org/books/2202.html",  # 部编版
            "filename": "五年级语文下册.pdf"
        }
    },
    6: {
        "up": {
            "name": "六年级语文上册",
            "url": "https://www.dzkbw.org/books/2203.html",  # 部编版
            "filename": "六年级语文上册.pdf"
        },
        "down": {
            "name": "六年级语文下册",
            "url": "https://www.dzkbw.org/books/2204.html",  # 部编版
            "filename": "六年级语文下册.pdf"
        }
    }
}

# 下载配置
DOWNLOAD_CONFIG = {
    "timeout": 30,
    "retry_times": 3,
    "retry_delay": 2,
    "chunk_size": 8192,
    "headers": {
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
        "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8",
        "Accept-Language": "zh-CN,zh;q=0.9,en;q=0.8",
    }
}


class TextbookDownloader:
    """教材下载器"""
    
    def __init__(self, output_dir: Path):
        self.output_dir = output_dir
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.session = requests.Session()
        self.session.headers.update(DOWNLOAD_CONFIG["headers"])
        
    def get_pdf_download_url(self, page_url: str) -> str | None:
        """从教材详情页提取PDF下载链接"""
        try:
            print(f"  正在获取下载链接: {page_url}")
            response = self.session.get(
                page_url, 
                timeout=DOWNLOAD_CONFIG["timeout"]
            )
            response.raise_for_status()
            
            soup = BeautifulSoup(response.text, 'html.parser')
            
            # 尝试多种方式查找下载链接
            # 方式1: 查找包含"下载"文字的链接
            for link in soup.find_all('a', href=True):
                href = link['href']
                text = link.get_text(strip=True)
                
                # 检查是否是PDF下载链接
                if '下载' in text or 'download' in text.lower():
                    if href.endswith('.pdf') or '.pdf' in href:
                        if not href.startswith('http'):
                            href = urljoin(page_url, href)
                        return href
            
            # 方式2: 查找PDF链接
            for link in soup.find_all('a', href=True):
                href = link['href']
                if href.endswith('.pdf') or '.pdf' in href:
                    if not href.startswith('http'):
                        href = urljoin(page_url, href)
                    return href
            
            # 方式3: 查找onclick中的下载链接
            for link in soup.find_all('a', onclick=True):
                onclick = link['onclick']
                pdf_match = re.search(r'(https?://[^\s\'"]+\.pdf)', onclick)
                if pdf_match:
                    return pdf_match.group(1)
            
            print("  ❌ 未找到PDF下载链接")
            return None
            
        except Exception as e:
            print(f"  ❌ 获取下载链接失败: {e}")
            return None
    
    def download_file(self, url: str, output_path: Path, desc: str) -> bool:
        """下载文件（支持断点续传和进度显示）"""
        
        # 检查是否已存在
        if output_path.exists():
            file_size = output_path.stat().st_size
            print(f"  ⏭️  文件已存在 ({file_size:,} 字节)，跳过下载")
            return True
        
        # 临时文件
        temp_path = output_path.with_suffix('.tmp')
        resume_pos = 0
        
        # 断点续传
        if temp_path.exists():
            resume_pos = temp_path.stat().st_size
            print(f"  📁 断点续传: {resume_pos:,} 字节")
        
        # 重试机制
        for attempt in range(1, DOWNLOAD_CONFIG["retry_times"] + 1):
            try:
                headers = DOWNLOAD_CONFIG["headers"].copy()
                if resume_pos > 0:
                    headers["Range"] = f"bytes={resume_pos}-"
                
                response = self.session.get(
                    url, 
                    headers=headers,
                    stream=True,
                    timeout=DOWNLOAD_CONFIG["timeout"]
                )
                
                # 检查响应状态
                if response.status_code == 416:  # Range Not Satisfiable
                    # 文件已完成
                    temp_path.rename(output_path)
                    return True
                
                response.raise_for_status()
                
                # 获取文件总大小
                total_size = int(response.headers.get('content-length', 0))
                if resume_pos > 0 and response.status_code == 206:
                    total_size += resume_pos
                
                # 开始下载
                print(f"  📥 开始下载: {desc}")
                print(f"  文件大小: {total_size:,} 字节" if total_size else "  文件大小: 未知")
                
                mode = 'ab' if resume_pos > 0 else 'wb'
                downloaded = resume_pos
                
                with open(temp_path, mode) as f:
                    for chunk in response.iter_content(chunk_size=DOWNLOAD_CONFIG["chunk_size"]):
                        if chunk:
                            f.write(chunk)
                            downloaded += len(chunk)
                            
                            # 显示进度
                            if total_size:
                                percent = (downloaded / total_size) * 100
                                print(f"\r  进度: {percent:.1f}% ({downloaded:,}/{total_size:,})", end='', flush=True)
                            else:
                                print(f"\r  已下载: {downloaded:,} 字节", end='', flush=True)
                
                print()  # 换行
                
                # 下载完成，重命名文件
                temp_path.rename(output_path)
                print(f"  ✅ 下载完成: {output_path.name}")
                return True
                
            except KeyboardInterrupt:
                print("\n  ⏸️  下载暂停")
                return False
                
            except Exception as e:
                print(f"\n  ⚠️  第{attempt}次下载失败: {e}")
                if attempt < DOWNLOAD_CONFIG["retry_times"]:
                    print(f"  等待{DOWNLOAD_CONFIG['retry_delay']}秒后重试...")
                    time.sleep(DOWNLOAD_CONFIG["retry_delay"])
                else:
                    print("  ❌ 达到最大重试次数，下载失败")
                    if temp_path.exists():
                        print(f"  临时文件保留: {temp_path}")
                    return False
        
        return False
    
    def download_textbook(self, grade: int, semester: str) -> bool:
        """下载单本教材"""
        if grade not in TEXTBOOK_DOWNLOAD_CONFIG:
            print(f"❌ 无效的年级: {grade}")
            return False
        
        if semester not in ["up", "down"]:
            print(f"❌ 无效的学期: {semester}")
            return False
        
        config = TEXTBOOK_DOWNLOAD_CONFIG[grade][semester]
        name = config["name"]
        page_url = config["url"]
        filename = config["filename"]
        output_path = self.output_dir / filename
        
        print(f"\n{'='*60}")
        print(f"📖 {name}")
        print(f"{'='*60}")
        
        # 获取PDF下载链接
        pdf_url = self.get_pdf_download_url(page_url)
        if not pdf_url:
            print("  ❌ 无法获取PDF下载链接")
            return False
        
        print(f"  ✅ 找到下载链接: {pdf_url}")
        
        # 下载文件
        return self.download_file(pdf_url, output_path, name)
    
    def download_all(self, grades: list[int] | None = None):
        """批量下载教材"""
        if grades is None:
            grades = list(range(1, 7))
        
        print("\n" + "="*60)
        print("📚 人教版小学语文教材下载工具")
        print("="*60)
        print(f"目标年级: {', '.join(f'{g}年级' for g in grades)}")
        print(f"保存目录: {self.output_dir}")
        print("="*60)
        
        success_count = 0
        failed_count = 0
        
        for grade in grades:
            for semester in ["up", "down"]:
                if self.download_textbook(grade, semester):
                    success_count += 1
                else:
                    failed_count += 1
                
                # 礼貌延迟
                time.sleep(1)
        
        # 打印统计
        print("\n" + "="*60)
        print("📊 下载统计")
        print("="*60)
        print(f"✅ 成功: {success_count} 本")
        print(f"❌ 失败: {failed_count} 本")
        print(f"📁 保存位置: {self.output_dir}")
        print("="*60)
        
        # 列出下载的文件
        print("\n已下载的文件:")
        for pdf_file in sorted(self.output_dir.glob("*.pdf")):
            size = pdf_file.stat().st_size
            print(f"  - {pdf_file.name} ({size:,} 字节)")


def main():
    """主函数"""
    parser = argparse.ArgumentParser(
        description="人教版小学语文教材下载工具",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  python tools/download_textbooks.py              # 下载所有教材
  python tools/download_textbooks.py --grade 1   # 只下载一年级
  python tools/download_textbooks.py --grades 1,2,3  # 下载指定年级
        """
    )
    
    parser.add_argument(
        '--grade',
        type=int,
        help='下载指定年级的教材 (1-6)'
    )
    
    parser.add_argument(
        '--grades',
        type=str,
        help='下载指定多个年级的教材，用逗号分隔 (如: 1,2,3)'
    )
    
    args = parser.parse_args()
    
    # 确定要下载的年级
    if args.grade:
        grades = [args.grade]
    elif args.grades:
        grades = [int(g.strip()) for g in args.grades.split(',')]
    else:
        grades = None  # 下载全部
    
    # 设置输出目录
    base_dir = Path(__file__).parent.parent
    output_dir = base_dir / "data" / "textbooks"
    
    # 创建下载器
    downloader = TextbookDownloader(output_dir)
    
    # 开始下载
    try:
        downloader.download_all(grades)
    except KeyboardInterrupt:
        print("\n\n⏸️  下载被用户中断")
        print("临时文件已保存，下次运行将自动续传")
        sys.exit(0)


if __name__ == "__main__":
    main()
