@echo off
cd /d c:\Users\wen\Desktop\ai_C
echo 你好 > temp_input.txt
echo 退出 >> temp_input.txt
main.exe < temp_input.txt > test_output3.txt 2>&1
type test_output3.txt
del temp_input.txt
