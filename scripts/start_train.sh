#!/usr/bin/bash
export PATH=/mingw64/bin:/usr/bin:/bin
cd /d/PivotMind_src || exit 1
nohup ./build/bin/batch_learn pivotmind_state.dat data/hermes_knowledge_base.json 500 > train_500_dell.log 2>&1 &
echo "PID=$!"
