#!/usr/bin/env python3
"""
PDF批量下载辅助工具

用途：当用户已获取PDF直链后，批量下载多个PDF文件

使用方法：
1. 创建链接文件 data/textbooks/download_list.txt
   格式：每行一个链接，格式为 "文件名|URL"
   例如：
     一年级语文上册.pdf|https://example.com/grade1_up.pdf
     一年级语文下册.pdf|https://example.com/grade1_down.pdf

2. 运行脚本：
   python tools/batch_download_pdfs.py
"""

import os
import sys
import time
from pathlib import Path

try:
    import requests
except ImportError:
    print("=" * 60)
    print("错误: 需要安装 requests 库")
    print("=" * 60)
    print("\n安装方法:")
    print("  pip install requests")
    print("=" * 60)
    sys.exit(1)


# 下载配置
DOWNLOAD_CONFIG = {
    "timeout": 60,
    "retry_times": 3,
    "retry_delay": 3,
    "chunk_size": 8192,
    "headers": {
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
    }
}


class BatchPDFDownloader:
    """批量PDF下载器"""
    
    def __init__(self, output_dir: Path):
        self.output_dir = output_dir
        self.output_dir.mkdir(parents=True, exist_ok=True)
    
    def download_file(self, url: str, output_path: Path) -> bool:
        """下载单个文件"""
        
        # 检查是否已存在
        if output_path.exists():
            file_size = output_path.stat().st_size
            print(f"  ⏭️  已存在 ({file_size:,} 字节)，跳过")
            return True
        
        # 重试机制
        for attempt in range(1, DOWNLOAD_CONFIG["retry_times"] + 1):
            try:
                print(f"  📥 开始下载...")
                
                response = requests.get(
                    url,
                    headers=DOWNLOAD_CONFIG["headers"],
                    stream=True,
                    timeout=DOWNLOAD_CONFIG["timeout"]
                )
                response.raise_for_status()
                
                # 获取文件总大小
                total_size = int(response.headers.get('content-length', 0))
                
                # 开始下载
                downloaded = 0
                temp_path = output_path.with_suffix('.tmp')
                
                with open(temp_path, 'wb') as f:
                    for chunk in response.iter_content(chunk_size=DOWNLOAD_CONFIG["chunk_size"]):
                        if chunk:
                            f.write(chunk)
                            downloaded += len(chunk)
                            
                            # 显示进度
                            if total_size:
                                percent = (downloaded / total_size) * 100
                                print(f"\r    进度: {percent:.1f}% ({downloaded:,}/{total_size:,} 字节)", 
                                      end='', flush=True)
                            else:
                                print(f"\r    已下载: {downloaded:,} 字节", end='', flush=True)
                
                print()  # 换行
                
                # 下载完成，重命名文件
                temp_path.rename(output_path)
                print(f"  ✅ 下载完成: {output_path.name}")
                return True
                
            except KeyboardInterrupt:
                print("\n  ⏸️  下载暂停")
                # 删除临时文件
                if temp_path.exists():
                    temp_path.unlink()
                return False
                
            except Exception as e:
                print(f"\n  ⚠️  第{attempt}次下载失败: {e}")
                if attempt < DOWNLOAD_CONFIG["retry_times"]:
                    print(f"  等待{DOWNLOAD_CONFIG['retry_delay']}秒后重试...")
                    time.sleep(DOWNLOAD_CONFIG["retry_delay"])
                else:
                    print("  ❌ 达到最大重试次数，下载失败")
                    # 删除临时文件
                    if temp_path.exists():
                        temp_path.unlink()
                    return False
        
        return False
    
    def download_from_list(self, list_file: Path):
        """从列表文件批量下载"""
        
        if not list_file.exists():
            print(f"❌ 列表文件不存在: {list_file}")
            print(f"\n请创建文件: {list_file}")
            print("格式：每行一个链接，格式为 '文件名|URL'")
            print("示例：")
            print("  一年级语文上册.pdf|https://example.com/grade1_up.pdf")
            print("  一年级语文下册.pdf|https://example.com/grade1_down.pdf")
            return
        
        # 读取下载列表
        with open(list_file, 'r', encoding='utf-8') as f:
            lines = f.readlines()
        
        # 解析下载任务
        tasks = []
        for line in lines:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            
            if '|' in line:
                filename, url = line.split('|', 1)
                tasks.append((filename.strip(), url.strip()))
            else:
                print(f"⚠️  无效格式: {line}")
        
        if not tasks:
            print("❌ 未找到有效的下载任务")
            return
        
        # 打印任务列表
        print("\n" + "=" * 60)
        print("📚 批量下载任务列表")
        print("=" * 60)
        print(f"总任务数: {len(tasks)}")
        print(f"保存目录: {self.output_dir}")
        print("=" * 60)
        
        for i, (filename, url) in enumerate(tasks, 1):
            print(f"{i}. {filename}")
        
        print("=" * 60)
        
        # 开始下载
        success_count = 0
        failed_count = 0
        
        for i, (filename, url) in enumerate(tasks, 1):
            print(f"\n[{i}/{len(tasks)}] {filename}")
            print(f"  URL: {url[:60]}...")
            
            output_path = self.output_dir / filename
            
            if self.download_file(url, output_path):
                success_count += 1
            else:
                failed_count += 1
            
            # 延迟，避免请求过快
            time.sleep(1)
        
        # 打印统计
        print("\n" + "=" * 60)
        print("📊 下载统计")
        print("=" * 60)
        print(f"✅ 成功: {success_count}")
        print(f"❌ 失败: {failed_count}")
        print(f"📁 保存位置: {self.output_dir}")
        print("=" * 60)
        
        # 列出下载的文件
        print("\n已下载的文件:")
        for pdf_file in sorted(self.output_dir.glob("*.pdf")):
            size = pdf_file.stat().st_size
            print(f"  ✅ {pdf_file.name} ({size:,} 字节)")


def create_example_list_file(list_file: Path):
    """创建示例列表文件"""
    example_content = """# PDF下载列表
# 格式: 文件名|URL
# 每行一个任务，以#开头的行为注释

# 示例（请替换为实际URL）：
# 一年级语文上册.pdf|https://example.com/grade1_up.pdf
# 一年级语文下册.pdf|https://example.com/grade1_down.pdf

# 提示：
# 1. 从国家中小学智慧教育平台获取PDF直链
# 2. 将链接粘贴到此文件
# 3. 运行 python tools/batch_download_pdfs.py
"""
    
    if not list_file.exists():
        with open(list_file, 'w', encoding='utf-8') as f:
            f.write(example_content)
        print(f"✅ 已创建示例列表文件: {list_file}")
        print("请编辑该文件，添加实际的PDF下载链接")


def main():
    """主函数"""
    print("=" * 60)
    print("📥 PDF批量下载辅助工具")
    print("=" * 60)
    
    # 设置目录
    base_dir = Path(__file__).parent.parent
    output_dir = base_dir / "data" / "textbooks"
    list_file = base_dir / "data" / "textbooks" / "download_list.txt"
    
    # 创建示例文件（如果不存在）
    create_example_list_file(list_file)
    
    # 创建下载器
    downloader = BatchPDFDownloader(output_dir)
    
    # 开始下载
    try:
        downloader.download_from_list(list_file)
    except KeyboardInterrupt:
        print("\n\n⏸️  下载被用户中断")
        print("临时文件已保存，可重新运行继续下载")


if __name__ == "__main__":
    main()
