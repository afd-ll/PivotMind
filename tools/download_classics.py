#!/usr/bin/env python3
"""
中国经典文本自动下载工具

用途：从公开网站下载中国经典文本（古诗词、古文、经典著作等）
用于AI多拓扑神经网络训练

使用方法：
    python tools/download_classics.py

数据来源：
1. 古诗文网 (gushiwen.cn) - 古诗词
2. 中华诗库 (shigeku.com) - 大量诗歌文档
3. 公开的经典文本资源
"""

import os
import sys
import time
import re
from pathlib import Path

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


# 经典文本下载配置
CLASSICS_CONFIG = {
    "tang_poems": {
        "name": "唐诗三百首",
        "source": "gushiwen",
        "url": "https://www.gushiwen.cn/gushi/tangshi.aspx",
        "output": "data/classics/tang_poems_300.txt"
    },
    "song_poems": {
        "name": "宋词三百首",
        "source": "gushiwen",
        "url": "https://www.gushiwen.cn/gushi/songci.aspx",
        "output": "data/classics/song_poems_300.txt"
    },
    "shijing": {
        "name": "诗经",
        "source": "gushiwen",
        "url": "https://www.gushiwen.cn/gushi/shijing.aspx",
        "output": "data/classics/shijing.txt"
    },
    "lunyu": {
        "name": "论语",
        "source": "gushiwen",
        "url": "https://www.gushiwen.cn/guwen/lunyu.aspx",
        "output": "data/classics/lunyu.txt"
    },
    "daodejing": {
        "name": "道德经",
        "source": "gushiwen",
        "url": "https://www.gushiwen.cn/guwen/daodejing.aspx",
        "output": "data/classics/daodejing.txt"
    }
}

# 下载配置
DOWNLOAD_CONFIG = {
    "timeout": 30,
    "retry_times": 3,
    "retry_delay": 2,
    "headers": {
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
        "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        "Accept-Language": "zh-CN,zh;q=0.9,en;q=0.8",
    }
}


class ClassicsDownloader:
    """经典文本下载器"""
    
    def __init__(self, output_dir: Path):
        self.output_dir = output_dir
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.session = requests.Session()
        self.session.headers.update(DOWNLOAD_CONFIG["headers"])
    
    def clean_text(self, text: str) -> str:
        """清理文本"""
        # 移除HTML标签
        text = re.sub(r'<[^>]+>', '', text)
        # 移除多余空白
        text = re.sub(r'\s+', '\n', text)
        # 移除特殊字符
        text = re.sub(r'[\x00-\x08\x0b\x0c\x0e-\x1f\x7f-\x9f]', '', text)
        return text.strip()
    
    def download_from_gushiwen(self, url: str, name: str) -> str | None:
        """从古诗文网下载文本"""
        try:
            print(f"\n正在下载: {name}")
            print(f"  URL: {url}")
            
            response = self.session.get(url, timeout=DOWNLOAD_CONFIG["timeout"])
            response.raise_for_status()
            response.encoding = 'utf-8'
            
            soup = BeautifulSoup(response.text, 'html.parser')
            
            # 查找诗文内容
            poems = []
            
            # 方法1: 提取<div class="sons">中的诗文
            for div in soup.find_all('div', class_='sons'):
                title_tag = div.find('b')
                if title_tag:
                    title = title_tag.get_text(strip=True)
                    
                    # 作者
                    source_tag = div.find('p', class_='source')
                    author = source_tag.get_text(strip=True) if source_tag else ""
                    
                    # 内容
                    content_tag = div.find('div', class_='contson')
                    if content_tag:
                        content = content_tag.get_text(strip=True)
                        content = re.sub(r'\s+', '', content)  # 移除多余空白
                        
                        if content and len(content) > 10:  # 过滤太短的内容
                            poem_text = f"【{title}】\n{author}\n{content}\n"
                            poems.append(poem_text)
            
            # 方法2: 如果没找到，尝试查找链接形式的诗文列表
            if not poems:
                # 查找所有诗文的链接
                links = soup.find_all('a', href=True)
                for link in links:
                    href = link.get('href', '')
                    text = link.get_text(strip=True)
                    
                    # 检查是否是诗文链接
                    if '/view_' in href and text and len(text) > 2 and len(text) < 30:
                        poems.append(f"【{text}】\n")
            
            if poems:
                result = "\n\n".join(poems)
                print(f"  ✅ 成功获取 {len(poems)} 篇作品")
                return result
            else:
                print("  ⚠️  未找到诗文内容")
                return None
                
        except Exception as e:
            print(f"  ❌ 下载失败: {e}")
            return None
    
    def save_text(self, text: str, output_path: Path, name: str):
        """保存文本到文件"""
        try:
            with open(output_path, 'w', encoding='utf-8') as f:
                f.write(f"# {name}\n")
                f.write(f"# 来源: 古诗文网\n")
                f.write(f"# 下载时间: {time.strftime('%Y-%m-%d %H:%M:%S')}\n\n")
                f.write(text)
            
            file_size = output_path.stat().st_size
            print(f"  ✅ 已保存: {output_path.name} ({file_size:,} 字节)")
            return True
            
        except Exception as e:
            print(f"  ❌ 保存失败: {e}")
            return False
    
    def download_classic(self, classic_key: str) -> bool:
        """下载单个经典文本"""
        if classic_key not in CLASSICS_CONFIG:
            print(f"❌ 未知的经典文本: {classic_key}")
            return False
        
        config = CLASSICS_CONFIG[classic_key]
        name = config["name"]
        url = config["url"]
        output_path = self.output_dir / Path(config["output"]).name
        
        print(f"\n{'='*60}")
        print(f"📖 {name}")
        print(f"{'='*60}")
        
        # 检查是否已存在
        if output_path.exists():
            print(f"⏭️  文件已存在，跳过下载")
            return True
        
        # 下载文本
        if config["source"] == "gushiwen":
            text = self.download_from_gushiwen(url, name)
        else:
            print(f"  ❌ 不支持的来源: {config['source']}")
            return False
        
        if not text:
            return False
        
        # 保存文本
        return self.save_text(text, output_path, name)
    
    def download_all(self):
        """批量下载所有经典文本"""
        print("\n" + "="*60)
        print("📚 中国经典文本下载工具")
        print("="*60)
        print(f"保存目录: {self.output_dir}")
        print("="*60)
        
        success_count = 0
        failed_count = 0
        
        for classic_key in CLASSICS_CONFIG.keys():
            if self.download_classic(classic_key):
                success_count += 1
            else:
                failed_count += 1
            
            # 延迟，避免请求过快
            time.sleep(1)
        
        # 打印统计
        print("\n" + "="*60)
        print("📊 下载统计")
        print("="*60)
        print(f"✅ 成功: {success_count}")
        print(f"❌ 失败: {failed_count}")
        print(f"📁 保存位置: {self.output_dir}")
        print("="*60)
        
        # 列出下载的文件
        print("\n已下载的文件:")
        for txt_file in sorted(self.output_dir.glob("*.txt")):
            size = txt_file.stat().st_size
            print(f"  ✅ {txt_file.name} ({size:,} 字节)")


def create_sample_texts(output_dir: Path):
    """创建示例文本（如果网络下载失败）"""
    print("\n创建示例文本...")
    
    samples = {
        "tang_poems_sample.txt": """# 唐诗示例
静夜思
李白
床前明月光，疑是地上霜。
举头望明月，低头思故乡。

春晓
孟浩然
春眠不觉晓，处处闻啼鸟。
夜来风雨声，花落知多少。

登鹳雀楼
王之涣
白日依山尽，黄河入海流。
欲穷千里目，更上一层楼。
""",
        "lunyu_sample.txt": """# 论语示例
学而篇第一

子曰："学而时习之，不亦说乎？有朋自远方来，不亦乐乎？人不知而不愠，不亦君子乎？"

有子曰："其为人也孝弟，而好犯上者，鲜矣；不好犯上，而好作乱者，未之有也。君子务本，本立而道生。孝弟也者，其为仁之本与！"

子曰："巧言令色，鲜矣仁！"
""",
        "daodejing_sample.txt": """# 道德经示例

第一章
道可道，非常道。名可名，非常名。
无名天地之始，有名万物之母。
故常无欲以观其妙，常有欲以观其徼。
此两者同出而异名，同谓之玄。
玄之又玄，众妙之门。

第二章
天下皆知美之为美，斯恶已。
皆知善之为善，斯不善已。
故有无相生，难易相成，长短相形，高下相倾，音声相和，前后相随。
是以圣人处无为之事，行不言之教。
万物作焉而不辞，生而不有，为而不恃，功成而弗居。
夫唯弗居，是以不去。
"""
    }
    
    for filename, content in samples.items():
        output_path = output_dir / filename
        if not output_path.exists():
            with open(output_path, 'w', encoding='utf-8') as f:
                f.write(content)
            print(f"  ✅ 创建示例: {filename}")


def main():
    """主函数"""
    # 设置目录
    base_dir = Path(__file__).parent.parent
    output_dir = base_dir / "data" / "classics"
    
    # 创建下载器
    downloader = ClassicsDownloader(output_dir)
    
    try:
        # 尝试在线下载
        downloader.download_all()
        
        # 如果下载失败，创建示例文本
        if not list(output_dir.glob("*.txt")):
            print("\n网络下载失败，创建示例文本...")
            create_sample_texts(output_dir)
    
    except KeyboardInterrupt:
        print("\n\n⏸️  下载被用户中断")
    except Exception as e:
        print(f"\n❌ 发生错误: {e}")
        # 创建示例文本
        create_sample_texts(output_dir)
    
    # 显示最终结果
    print("\n" + "="*60)
    print("📁 训练数据准备完成")
    print("="*60)
    
    txt_files = list(output_dir.glob("*.txt"))
    if txt_files:
        print(f"可用文本: {len(txt_files)} 个文件")
        for txt_file in sorted(txt_files):
            size = txt_file.stat().st_size
            print(f"  - {txt_file.name} ({size:,} 字节)")
    else:
        print("未找到文本文件")


if __name__ == "__main__":
    main()
