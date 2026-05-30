Set WshShell = CreateObject("WScript.Shell")
WshShell.CurrentDirectory = "D:\PivotMind_src"
WshShell.Run "cmd /c batch_learn_v2.exe pivotmind_state.dat data\hermes_knowledge_base.json 500 > train_500_vbs.log 2>&1", 0, False
