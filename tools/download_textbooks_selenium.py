#!/usr/bin/env python3
"""
小学语文教材自动下载工具（浏览器自动化版）

使用Selenium模拟真实浏览器操作，绕过反爬虫保护
从国家中小学智慧教育平台下载教材PDF

使用方法：
1. 安装依赖: pip install selenium webdriver-manager
2. 运行脚本: python tools/download_textbooks_selenium.py

注意：
- 首次运行会自动下载浏览器驱动
- 下载过程会打开浏览器窗口
- 下载完成后浏览器会自动关闭
"""

import os
import sys
import time
import json
import re
from pathlib import Path
from urllib.parse import urlparse, parse_qs

try:
    from selenium import webdriver
    from selenium.webdriver.common.by import By
    from selenium.webdriver.support.ui import WebDriverWait
    from selenium.webdriver.support import expected_conditions as EC
    from selenium.webdriver.chrome.service import Service
    from selenium.webdriver.chrome.options import Options
    from webdriver_manager.chrome import ChromeDriverManager
except ImportError:
    print("=" * 60)
    print("错误: 需要安装依赖库")
    print("=" * 60)
    print("\n安装方法:")
    print("  pip install selenium webdriver-manager")
    print("=" * 60)
    sys.exit(1)


# 教材配置（国家中小学智慧教育平台）
TEXTBOOK_CONFIG = {
    "grade1_up": {
        "name": "一年级语文上册",
        "url": "https://basic.smartedu.cn/tchMaterial/detail?contentType=assets_document&contentId=1c73b348-d76d-48c7-b72f-3b9c8e5b7b4a",
        "output": "一年级语文上册.pdf"
    },
    # 注意：其他年级的contentId需要手动查找补充
    # 可以在平台页面URL中找到
}


class TextbookDownloaderSelenium:
    """使用Selenium的教材下载器"""
    
    def __init__(self, output_dir: Path, headless: bool = False):
        self.output_dir = output_dir
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.headless = headless
        self.driver = None
        
    def init_driver(self):
        """初始化浏览器驱动"""
        print("\n正在初始化浏览器...")
        
        chrome_options = Options()
        
        # 无头模式（可选）
        if self.headless:
            chrome_options.add_argument('--headless')
        
        # 常用配置
        chrome_options.add_argument('--disable-gpu')
        chrome_options.add_argument('--no-sandbox')
        chrome_options.add_argument('--disable-dev-shm-usage')
        chrome_options.add_argument('--disable-blink-features=AutomationControlled')
        chrome_options.add_experimental_option('excludeSwitches', ['enable-automation'])
        chrome_options.add_experimental_option('useAutomationExtension', False)
        
        # 自动下载和配置ChromeDriver
        service = Service(ChromeDriverManager().install())
        self.driver = webdriver.Chrome(service=service, options=chrome_options)
        
        # 设置隐式等待
        self.driver.implicitly_wait(10)
        
        print("✅ 浏览器初始化完成")
    
    def get_pdf_url_from_page(self, textbook_url: str) -> str | None:
        """从教材页面获取PDF下载链接"""
        try:
            print(f"\n正在访问: {textbook_url}")
            self.driver.get(textbook_url)
            
            # 等待页面加载
            time.sleep(3)
            
            # 方法1: 从Network请求中获取PDF链接
            # 启用Network监控
            self.driver.execute_script("""
                window.pdfUrls = [];
                window.performance.getEntriesByType('resource').forEach(entry => {
                    if (entry.name.includes('.pdf')) {
                        window.pdfUrls.push(entry.name);
                    }
                });
            """)
            
            pdf_urls = self.driver.execute_script("return window.pdfUrls;")
            
            if pdf_urls:
                print(f"✅ 找到PDF链接: {pdf_urls[0]}")
                return pdf_urls[0]
            
            # 方法2: 查找页面中的PDF链接
            links = self.driver.find_elements(By.TAG_NAME, "a")
            for link in links:
                href = link.get_attribute("href")
                if href and ".pdf" in href:
                    print(f"✅ 找到PDF链接: {href}")
                    return href
            
            # 方法3: 查找iframe中的PDF
            iframes = self.driver.find_elements(By.TAG_NAME, "iframe")
            for iframe in iframes:
                src = iframe.get_attribute("src")
                if src and ".pdf" in src:
                    print(f"✅ 找到PDF链接: {src}")
                    return src
            
            # 方法4: 从页面源码中提取
            page_source = self.driver.page_source
            pdf_pattern = r'https?://[^\s\'"<>]+\.pdf[^\s\'"<>]*'
            matches = re.findall(pdf_pattern, page_source)
            
            if matches:
                print(f"✅ 找到PDF链接: {matches[0]}")
                return matches[0]
            
            print("❌ 未找到PDF下载链接")
            return None
            
        except Exception as e:
            print(f"❌ 获取PDF链接失败: {e}")
            return None
    
    def download_pdf(self, pdf_url: str, output_path: Path) -> bool:
        """下载PDF文件"""
        try:
            import requests
            
            print(f"\n正在下载: {output_path.name}")
            
            # 使用requests下载（更快）
            headers = {
                "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
            }
            
            response = requests.get(pdf_url, headers=headers, stream=True, timeout=60)
            response.raise_for_status()
            
            total_size = int(response.headers.get('content-length', 0))
            downloaded = 0
            
            with open(output_path, 'wb') as f:
                for chunk in response.iter_content(chunk_size=8192):
                    if chunk:
                        f.write(chunk)
                        downloaded += len(chunk)
                        
                        if total_size:
                            percent = (downloaded / total_size) * 100
                            print(f"\r  进度: {percent:.1f}%", end='', flush=True)
            
            print(f"\n✅ 下载完成: {output_path.name} ({downloaded:,} 字节)")
            return True
            
        except Exception as e:
            print(f"\n❌ 下载失败: {e}")
            return False
    
    def download_textbook(self, grade_key: str) -> bool:
        """下载单本教材"""
        if grade_key not in TEXTBOOK_CONFIG:
            print(f"❌ 未知的教材: {grade_key}")
            return False
        
        config = TEXTBOOK_CONFIG[grade_key]
        name = config["name"]
        url = config["url"]
        output_path = self.output_dir / config["output"]
        
        print(f"\n{'='*60}")
        print(f"📖 {name}")
        print(f"{'='*60}")
        
        # 检查是否已存在
        if output_path.exists():
            print(f"⏭️  文件已存在，跳过下载")
            return True
        
        # 获取PDF链接
        pdf_url = self.get_pdf_url_from_page(url)
        if not pdf_url:
            return False
        
        # 下载PDF
        return self.download_pdf(pdf_url, output_path)
    
    def download_all(self):
        """批量下载所有教材"""
        print("\n" + "="*60)
        print("📚 小学语文教材下载工具 (Selenium版)")
        print("="*60)
        print(f"保存目录: {self.output_dir}")
        print("="*60)
        
        # 初始化浏览器
        self.init_driver()
        
        success_count = 0
        failed_count = 0
        
        try:
            for grade_key in TEXTBOOK_CONFIG.keys():
                if self.download_textbook(grade_key):
                    success_count += 1
                else:
                    failed_count += 1
                
                # 延迟，避免请求过快
                time.sleep(2)
        
        finally:
            # 关闭浏览器
            if self.driver:
                self.driver.quit()
                print("\n✅ 浏览器已关闭")
        
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
            print(f"  ✅ {pdf_file.name} ({size:,} 字节)")
    
    def close(self):
        """清理资源"""
        if self.driver:
            self.driver.quit()


def main():
    """主函数"""
    # 设置输出目录
    base_dir = Path(__file__).parent.parent
    output_dir = base_dir / "data" / "textbooks"
    
    # 创建下载器
    downloader = TextbookDownloaderSelenium(output_dir, headless=False)
    
    try:
        # 开始下载
        downloader.download_all()
    except KeyboardInterrupt:
        print("\n\n⏸️  下载被用户中断")
    except Exception as e:
        print(f"\n❌ 发生错误: {e}")
        import traceback
        traceback.print_exc()
    finally:
        downloader.close()


if __name__ == "__main__":
    main()
