$env:Path = "D:\PivotMind_src\build\bin;" + [Environment]::GetEnvironmentVariable("Path", "Machine")
$pinfo = New-Object System.Diagnostics.ProcessStartInfo
$pinfo.FileName = "D:\PivotMind_src\build\bin\batch_learn.exe"
$pinfo.Arguments = "../../pivotmind_state.dat ../../data/hermes_knowledge_base.json 500"
$pinfo.RedirectStandardOutput = $true
$pinfo.RedirectStandardError = $true
$pinfo.UseShellExecute = $false
$pinfo.WorkingDirectory = "D:\PivotMind_src\build\bin"
$p = New-Object System.Diagnostics.Process
$p.StartInfo = $pinfo
$p.Start() | Out-Null
$stdout = $p.StandardOutput.ReadToEnd()
$stderr = $p.StandardError.ReadToEnd()
[System.IO.File]::WriteAllText("D:\PivotMind_src\train_500_dell_ps.log", $stdout)
[System.IO.File]::WriteAllText("D:\PivotMind_src\train_500_dell_ps_err.log", $stderr)
Write-Host ("PID=" + $p.Id)
