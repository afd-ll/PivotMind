$env:Path = "D:\PivotMind_src;" + [Environment]::GetEnvironmentVariable("Path", "Machine")
Start-Process -FilePath D:\PivotMind_src\batch_learn_v2.exe -ArgumentList "pivotmind_state.dat","data\hermes_knowledge_base.json","500" -RedirectStandardOutput D:\PivotMind_src\train_500_v2.log -RedirectStandardError D:\PivotMind_src\train_500_v2_err.log -WindowStyle Hidden -WorkingDirectory D:\PivotMind_src
Write-Host "Launched"
