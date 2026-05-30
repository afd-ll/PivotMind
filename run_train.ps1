$ErrorActionPreference = "SilentlyContinue"
Set-Location "D:\work\玄枢-pivotmind"

$logFile = "D:\work\玄枢-pivotmind\train_log_100.txt"
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = "D:\work\玄枢-pivotmind\build\bin\batch_learn.exe"
$psi.Arguments = "pivotmind_state.dat data\hermes_knowledge_base.json 100"
$psi.WorkingDirectory = "D:\work\玄枢-pivotmind"
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true

$p = [System.Diagnostics.Process]::Start($psi)
$p.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::Normal

$writer = New-Object System.IO.StreamWriter($logFile, $false, [System.Text.Encoding]::UTF8)
$writer.AutoFlush = $true

while (!$p.StandardOutput.EndOfStream) {
    $line = $p.StandardOutput.ReadLine()
    $writer.WriteLine($line)
}

$writer.Close()
$p.WaitForExit()
