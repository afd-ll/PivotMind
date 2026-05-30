# PivotMind batch_learn 构建脚本 (PowerShell)
$ErrorActionPreference = "Stop"
$CC = "gcc"
$CFLAGS = @("-Wall", "-Wextra", "-O2", "-Iinclude", "-I.", "-Ilibs", "-std=gnu99", "-fopenmp", "-pthread", "-MD", "-MP", "-D_USE_MATH_DEFINES")

cd "D:\work\玄枢-pivotmind"

# 创建输出目录
New-Item -ItemType Directory -Force -Path build\obj, build\dep, build\bin | Out-Null

Write-Host "=== 编译核心源文件 (src/*.c) ==="
$srcFiles = Get-ChildItem src\*.c
$total = $srcFiles.Count
$i = 0
foreach ($f in $srcFiles) {
    $i++
    $base = $f.BaseName
    $objFile = "build\obj\$base.o"
    $depFile = "build\dep\$base.d"
    Write-Host "[$i/$total] $($f.Name)"
    & $CC @CFLAGS "-MF$depFile" "-c" $f.FullName "-o" $objFile
    if ($LASTEXITCODE -ne 0) { Write-Host "FAILED: $($f.Name)"; exit 1 }
}

Write-Host "=== 编译 batch_learn.c ==="
& $CC @CFLAGS "-MFbuild\dep\batch_learn.d" "-c" "tools\batch_learn.c" "-o" "build\obj\batch_learn.o"
if ($LASTEXITCODE -ne 0) { Write-Host "FAILED: batch_learn.c"; exit 1 }

Write-Host "=== 创建静态库 libpivotmind.a ==="
$objFiles = (Get-ChildItem build\obj\*.o | Where-Object { $_.Name -ne "batch_learn.o" }) | ForEach-Object { $_.FullName }
& ar rcs libpivotmind.a @objFiles
if ($LASTEXITCODE -ne 0) { Write-Host "FAILED: ar"; exit 1 }

Write-Host "=== 链接 batch_learn.exe ==="
& $CC @CFLAGS "-o" "build\bin\batch_learn.exe" "build\obj\batch_learn.o" "-L." "-lpivotmind" "-lm"
if ($LASTEXITCODE -ne 0) { Write-Host "FAILED: link"; exit 1 }

$exe = Get-Item build\bin\batch_learn.exe
Write-Host "=== 完成! batch_learn.exe = $([math]::Round($exe.Length/1KB, 1))KB ==="
