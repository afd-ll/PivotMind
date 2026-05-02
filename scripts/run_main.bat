@echo off
chcp 65001 > nul 2>&1
set LC_ALL=zh_CN.UTF-8
set LANG=zh_CN.UTF-8
set PYTHONIOENCODING=utf-8
echo ========================================
echo 运行 AI 聊天系统 (UTF-8 编码)
echo ========================================
echo.
echo 使用文本文件作为训练数据源
echo.
main.exe training_data.txt 0
pause
