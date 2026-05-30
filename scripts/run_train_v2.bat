@echo off
set PATH=D:\msys64\mingw64\bin;D:\msys64\usr\bin;%PATH%
cd /d D:\PivotMind_src
build\bin\batch_learn.exe pivotmind_state.dat data/hermes_knowledge_base.json 100 > train_v2.log 2>&1
