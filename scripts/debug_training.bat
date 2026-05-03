@echo off
cd /d "c:\Users\wen\Desktop\ai_C"

echo === 编译程序 ===
gcc -o debug_main.exe main.c src/*.c -Iinclude -lm

echo === 运行程序进行诊断 ===
echo 注意：观察训练轮数显示
pause
debug_main.exe

echo === 程序运行结束 ===
pause