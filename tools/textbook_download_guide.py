#!/usr/bin/env python3
"""
小学语文教材下载辅助工具

由于国家中小学智慧教育平台有反爬虫保护，本工具采用半自动化方式：
1. 提供教材链接列表
2. 引导用户手动获取PDF直链
3. 辅助批量下载

使用方法：
    python tools/textbook_download_guide.py
"""

import os
import webbrowser
from pathlib import Path


# 教材链接配置（国家中小学智慧教育平台）
TEXTBOOK_LINKS = {
    "一年级上册": "https://basic.smartedu.cn/tchMaterial/detail?contentType=assets_document&contentId=1c73b348-d76d-48c7-b72f-3b9c8e5b7b4a",
    # 更多教材链接需要手动查找补充
}


def print_manual_guide():
    """打印手动下载指南"""
    guide = """
╔══════════════════════════════════════════════════════════════╗
║          小学语文教材下载指南 - 手动方法                      ║
╚══════════════════════════════════════════════════════════════╝

由于电子课本网站链接已失效，建议使用官方平台下载：

【方法一】国家中小学智慧教育平台（官方推荐）
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

步骤：
1. 访问平台：https://basic.smartedu.cn/elecEdu
2. 选择：小学 → 语文 → 选择年级 → 选择册次
3. 打开教材预览页面
4. 按F12打开开发者工具
5. 在Network标签页找到PDF文件请求
6. 右键 → Copy → Copy link address
7. 将链接粘贴到浏览器下载

【方法二】使用开源工具
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

推荐工具：
1. smartedu-dl-py (Python)
   GitHub: https://github.com/changsongyang/smartedu-dl-py
   
2. SmartEduDownloader (C#)
   Gitee: https://gitee.com/loongba/SmartEduDownloader

安装和使用：
```bash
# 克隆项目
git clone https://github.com/changsongyang/smartedu-dl-py

# 安装依赖
cd smartedu-dl-py
pip install -r requirements.txt

# 运行工具
python main.py
```

【方法三】手动查找教材ID
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

已知教材ID格式：
https://basic.smartedu.cn/tchMaterial/detail?contentType=assets_document&contentId=<ID>

一年级语文上册ID: 1c73b348-d76d-48c7-b72f-3b9c8e5b7b4a

其他教材ID需要：
1. 访问平台手动查找
2. 从URL中提取contentId参数
3. 使用工具批量下载

╔══════════════════════════════════════════════════════════════╗
║                    快速开始                                  ║
╚══════════════════════════════════════════════════════════════╝

选项1: 打开官方平台 (推荐)
选项2: 下载开源工具
选项3: 使用已有教材链接

"""
    print(guide)


def open_smartedu_platform():
    """打开国家中小学智慧教育平台"""
    url = "https://basic.smartedu.cn/elecEdu"
    print(f"\n正在打开: {url}")
    print("请在浏览器中查找需要的教材...")
    webbrowser.open(url)


def list_known_textbooks():
    """列出已知的教材链接"""
    print("\n已知教材链接：")
    print("=" * 60)
    
    for name, url in TEXTBOOK_LINKS.items():
        print(f"\n{name}:")
        print(f"  {url}")
    
    print("\n" + "=" * 60)
    print("\n提示：点击链接打开教材页面，然后按F12获取PDF下载地址")


def download_smartedu_dl():
    """提供smartedu-dl-py的下载指导"""
    info = """
╔══════════════════════════════════════════════════════════════╗
║          smartedu-dl-py 工具安装指南                         ║
╚══════════════════════════════════════════════════════════════╝

这是一个Python编写的国家中小学智慧教育平台教材下载工具

安装步骤：

1. 克隆项目：
   git clone https://github.com/changsongyang/smartedu-dl-py

2. 进入目录：
   cd smartedu-dl-py

3. 安装依赖：
   pip install -r requirements.txt

4. 运行工具：
   python main.py

功能特点：
- ✅ 支持批量下载
- ✅ 自动获取教材列表
- ✅ 支持多线程下载
- ✅ 自动命名文件

注意事项：
- 需要Python 3.7+
- 需要网络连接
- 部分资源可能需要登录

"""
    print(info)


def main():
    """主函数"""
    print_manual_guide()
    
    while True:
        print("\n请选择操作：")
        print("1. 打开国家中小学智慧教育平台")
        print("2. 查看已知教材链接")
        print("3. 下载smartedu-dl-py工具")
        print("4. 退出")
        
        try:
            choice = input("\n请输入选项 (1-4): ").strip()
            
            if choice == "1":
                open_smartedu_platform()
            elif choice == "2":
                list_known_textbooks()
            elif choice == "3":
                download_smartedu_dl()
            elif choice == "4":
                print("\n再见！")
                break
            else:
                print("无效选项，请重新输入")
        
        except KeyboardInterrupt:
            print("\n\n操作已取消")
            break
        except Exception as e:
            print(f"错误: {e}")


if __name__ == "__main__":
    main()
