$env:Path = "D:\msys64\mingw64\bin;" + [Environment]::GetEnvironmentVariable("Path", "Machine")
$proc = Start-Process -FilePath "D:\PivotMind_src\build\bin\batch_learn.exe" -ArgumentList "pivotmind_state.dat","data\hermes_knowledge_base.json","500" -RedirectStandardOutput "D:\PivotMind_src\train_500_dell.log" -RedirectStandardError "D:\PivotMind_src\train_500_dell_err.log" -WindowStyle Hidden -WorkingDirectory "D:\PivotMind_src" -PassThru
Write-Host ("PID=" + $proc.Id)
