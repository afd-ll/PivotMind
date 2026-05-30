@echo off
set PATH=D:\msys64\mingw64\bin;D:\msys64\usr\bin;%PATH%
set OMP_NUM_THREADS=4
cd /d D:\PivotMind_src
build\bin\batch_learn.exe D:\PivotMind_src\pivotmind_state.dat D:\PivotMind_src\data\hermes_knowledge_base.json 100 > D:\PivotMind_src\train_v5.log 2>&1
