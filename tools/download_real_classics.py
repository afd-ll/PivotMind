#!/usr/bin/env python3
"""
真实经典文本下载工具

直接从古诗文网API获取完整的经典文本内容
不使用示例数据，只下载真实文本

使用方法：
    python tools/download_real_classics.py
"""

import os
import sys
import time
import json
import re
from pathlib import Path

try:
    import requests
except ImportError:
    print("需要安装: pip install requests")
    sys.exit(1)


class RealClassicsDownloader:
    """真实经典文本下载器"""
    
    def __init__(self, output_dir: Path):
        self.output_dir = output_dir
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.session = requests.Session()
        self.session.headers.update({
            'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36',
            'Accept': 'application/json, text/javascript, */*',
            'Referer': 'https://www.gushiwen.cn/'
        })
    
    def download_tang_poems_300(self):
        """下载唐诗三百首完整版"""
        print("\n" + "="*60)
        print("📖 唐诗三百首")
        print("="*60)
        
        poems = []
        base_url = "https://www.gushiwen.cn"
        
        # 唐诗三百首页面
        url = "https://www.gushiwen.cn/gushi/tangshi.aspx"
        
        try:
            print(f"正在访问: {url}")
            response = self.session.get(url, timeout=30)
            response.encoding = 'utf-8'
            
            # 提取所有诗文的view链接
            import re
            pattern = r'href="(view_\d+\.aspx)"[^>]*>([^<]+)</a>'
            matches = re.findall(pattern, response.text)
            
            print(f"找到 {len(matches)} 首唐诗")
            
            # 下载每首诗的详细内容
            for i, (view_link, title) in enumerate(matches[:50], 1):  # 先下载前50首测试
                try:
                    poem_url = f"{base_url}/{view_link}"
                    print(f"\r  [{i}/50] {title}...", end='', flush=True)
                    
                    poem_response = self.session.get(poem_url, timeout=10)
                    poem_response.encoding = 'utf-8'
                    
                    # 提取诗文内容
                    content_pattern = r'<div class="contson"[^>]*>(.*?)</div>'
                    content_match = re.search(content_pattern, poem_response.text, re.DOTALL)
                    
                    if content_match:
                        content = content_match.group(1)
                        # 清理HTML标签
                        content = re.sub(r'<[^>]+>', '', content)
                        content = re.sub(r'\s+', '', content)
                        
                        poems.append({
                            'title': title.strip(),
                            'content': content.strip()
                        })
                    
                    time.sleep(0.5)  # 延迟避免被封
                
                except Exception as e:
                    print(f"\n  警告: 下载失败 - {e}")
                    continue
            
            print()  # 换行
            
            if poems:
                # 保存文件
                output_file = self.output_dir / "tang_poems_300.txt"
                with open(output_file, 'w', encoding='utf-8') as f:
                    f.write("# 唐诗三百首\n")
                    f.write(f"# 来源: 古诗文网 (gushiwen.cn)\n")
                    f.write(f"# 数量: {len(poems)} 首\n\n")
                    
                    for poem in poems:
                        f.write(f"【{poem['title']}】\n")
                        f.write(f"{poem['content']}\n\n")
                
                print(f"✅ 成功下载 {len(poems)} 首唐诗")
                print(f"   保存至: {output_file}")
                return True
            else:
                print("❌ 未能获取唐诗内容")
                return False
                
        except Exception as e:
            print(f"❌ 下载失败: {e}")
            return False
    
    def download_song_ci_300(self):
        """下载宋词三百首"""
        print("\n" + "="*60)
        print("📖 宋词三百首")
        print("="*60)
        
        # 类似唐诗的下载逻辑
        poems = []
        url = "https://www.gushiwen.cn/gushi/songci.aspx"
        
        try:
            print(f"正在访问: {url}")
            response = self.session.get(url, timeout=30)
            response.encoding = 'utf-8'
            
            # 提取链接
            pattern = r'href="(view_\d+\.aspx)"[^>]*>([^<]+)</a>'
            matches = re.findall(pattern, response.text)
            
            print(f"找到 {len(matches)} 首宋词")
            
            # 下载前50首
            for i, (view_link, title) in enumerate(matches[:50], 1):
                try:
                    poem_url = f"https://www.gushiwen.cn/{view_link}"
                    print(f"\r  [{i}/50] {title}...", end='', flush=True)
                    
                    poem_response = self.session.get(poem_url, timeout=10)
                    poem_response.encoding = 'utf-8'
                    
                    content_pattern = r'<div class="contson"[^>]*>(.*?)</div>'
                    content_match = re.search(content_pattern, poem_response.text, re.DOTALL)
                    
                    if content_match:
                        content = content_match.group(1)
                        content = re.sub(r'<[^>]+>', '', content)
                        content = re.sub(r'\s+', '', content)
                        
                        poems.append({
                            'title': title.strip(),
                            'content': content.strip()
                        })
                    
                    time.sleep(0.5)
                
                except Exception as e:
                    continue
            
            print()
            
            if poems:
                output_file = self.output_dir / "song_ci_300.txt"
                with open(output_file, 'w', encoding='utf-8') as f:
                    f.write("# 宋词三百首\n")
                    f.write(f"# 来源: 古诗文网 (gushiwen.cn)\n")
                    f.write(f"# 数量: {len(poems)} 首\n\n")
                    
                    for poem in poems:
                        f.write(f"【{poem['title']}】\n")
                        f.write(f"{poem['content']}\n\n")
                
                print(f"✅ 成功下载 {len(poems)} 首宋词")
                print(f"   保存至: {output_file}")
                return True
            else:
                print("❌ 未能获取宋词内容")
                return False
                
        except Exception as e:
            print(f"❌ 下载失败: {e}")
            return False
    
    def download_shijing(self):
        """下载诗经"""
        print("\n" + "="*60)
        print("📖 诗经")
        print("="*60)
        
        url = "https://www.gushiwen.cn/gushi/shijing.aspx"
        
        try:
            print(f"正在访问: {url}")
            response = self.session.get(url, timeout=30)
            response.encoding = 'utf-8'
            
            # 提取诗经篇目
            pattern = r'href="(view_\d+\.aspx)"[^>]*>([^<]+)</a>'
            matches = re.findall(pattern, response.text)
            
            print(f"找到 {len(matches)} 篇诗经")
            
            poems = []
            # 下载前30篇
            for i, (view_link, title) in enumerate(matches[:30], 1):
                try:
                    poem_url = f"https://www.gushiwen.cn/{view_link}"
                    print(f"\r  [{i}/30] {title}...", end='', flush=True)
                    
                    poem_response = self.session.get(poem_url, timeout=10)
                    poem_response.encoding = 'utf-8'
                    
                    content_pattern = r'<div class="contson"[^>]*>(.*?)</div>'
                    content_match = re.search(content_pattern, poem_response.text, re.DOTALL)
                    
                    if content_match:
                        content = content_match.group(1)
                        content = re.sub(r'<[^>]+>', '', content)
                        content = re.sub(r'\s+', '', content)
                        
                        poems.append({
                            'title': title.strip(),
                            'content': content.strip()
                        })
                    
                    time.sleep(0.5)
                
                except:
                    continue
            
            print()
            
            if poems:
                output_file = self.output_dir / "shijing.txt"
                with open(output_file, 'w', encoding='utf-8') as f:
                    f.write("# 诗经\n")
                    f.write(f"# 来源: 古诗文网 (gushiwen.cn)\n")
                    f.write(f"# 数量: {len(poems)} 篇\n\n")
                    
                    for poem in poems:
                        f.write(f"【{poem['title']}】\n")
                        f.write(f"{poem['content']}\n\n")
                
                print(f"✅ 成功下载 {len(poems)} 篇诗经")
                print(f"   保存至: {output_file}")
                return True
            else:
                print("❌ 未能获取诗经内容")
                return False
                
        except Exception as e:
            print(f"❌ 下载失败: {e}")
            return False
    
    def download_all(self):
        """下载所有经典文本"""
        print("\n" + "="*60)
        print("📚 真实经典文本下载工具")
        print("="*60)
        print(f"保存目录: {self.output_dir}")
        print("="*60)
        
        success_count = 0
        
        if self.download_tang_poems_300():
            success_count += 1
        
        if self.download_song_ci_300():
            success_count += 1
        
        if self.download_shijing():
            success_count += 1
        
        # 打印统计
        print("\n" + "="*60)
        print("📊 下载统计")
        print("="*60)
        print(f"✅ 成功下载: {success_count}/3 个文集")
        print(f"📁 保存位置: {self.output_dir}")
        print("="*60)
        
        # 列出文件
        print("\n已下载文件:")
        for txt_file in sorted(self.output_dir.glob("*.txt")):
            size = txt_file.stat().st_size
            print(f"  ✅ {txt_file.name} ({size:,} 字节)")


def main():
    base_dir = Path(__file__).parent.parent
    output_dir = base_dir / "data" / "classics"
    
    # 删除旧的示例文件
    for sample_file in output_dir.glob("*sample*.txt"):
        sample_file.unlink()
        print(f"已删除示例文件: {sample_file.name}")
    
    downloader = RealClassicsDownloader(output_dir)
    
    try:
        downloader.download_all()
    except KeyboardInterrupt:
        print("\n\n下载被用户中断")


if __name__ == "__main__":
    main()
