@echo off
echo ====================================
echo 解码策略对比测试
echo ====================================
echo.

echo [1] 测试贪婪解码...
echo 修改 main.c 中 use_beam_search = 0
echo.

echo [2] 测试束搜索 (K=3)...
echo 修改 main.c 中 use_beam_search = 1
echo.

echo 当前配置: 束搜索 (K=3, 长度惩罚=0.7)
echo 模型: 嵌入128维, 隐藏层256维
echo.

echo 开始运行...
main.exe
