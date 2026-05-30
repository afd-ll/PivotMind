@echo off
cd /d D:\PivotMind_src
set PATH=D:\msys64\mingw64\bin;D:\msys64\usr\bin;%PATH%

echo ===== BUILD START %date% %time% ===== > build_train.log 2>&1

REM find make
where make >> build_train.log 2>&1
where mingw32-make >> build_train.log 2>&1

REM try mingw32-make first, then make
set MAKE_CMD=
if exist D:\msys64\mingw64\bin\mingw32-make.exe set MAKE_CMD=D:\msys64\mingw64\bin\mingw32-make.exe
if "%MAKE_CMD%"=="" if exist D:\msys64\usr\bin\make.exe set MAKE_CMD=D:\msys64\usr\bin\make.exe
if "%MAKE_CMD%"=="" (
    echo ERROR: make not found >> build_train.log 2>&1
    exit /b 1
)

echo Using: %MAKE_CMD% >> build_train.log 2>&1

REM clean build
%MAKE_CMD% clean >> build_train.log 2>&1
%MAKE_CMD% batch-learn >> build_train.log 2>&1

if errorlevel 1 (
    echo ===== BUILD FAILED %date% %time% ===== >> build_train.log 2>&1
    exit /b 1
)

echo ===== BUILD OK %date% %time% ===== >> build_train.log 2>&1

REM run 30 rounds standard
echo ===== TRAIN START (30 rounds, standard) %date% %time% ===== >> build_train.log 2>&1
batch_learn.exe pivotmind_state.dat data/training_data.json 30 >> build_train.log 2>&1

echo ===== TRAIN DONE %date% %time% ===== >> build_train.log 2>&1
