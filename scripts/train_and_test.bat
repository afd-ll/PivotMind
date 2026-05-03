@echo off
REM 一键训练和测试脚本

echo ========================================
echo 🎯 一键训练+测试流程
echo ========================================
echo.

REM 步骤1：训练
echo [步骤1] 训练经典文本...
echo ========================================
build\bin\train_classics.exe --save
if errorlevel 1 (
    echo ❌ 训练失败
    pause
    exit /b 1
)

echo.
echo ========================================
echo ✅ 训练完成
echo ========================================
echo.

REM 步骤2：测试
echo [步骤2] 测试生成效果...
echo ========================================

echo.
echo 测试1: 输入 "你好"
build\bin\test_trained_model.exe "你好"
echo.

echo 测试2: 输入 "春天"
build\bin\test_trained_model.exe "春天"
echo.

echo 测试3: 输入 "月亮"
build\bin\test_trained_model.exe "月亮"
echo.

echo ========================================
echo ✅ 全部完成
echo ========================================
echo.
echo 查看训练统计: data\training_stats.txt
echo.
pause
