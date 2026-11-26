# Installs flamingo-worker on this PC as a task that starts with Windows (before anyone logs in, as SYSTEM), restarts
# if it stops, and has no time limit. Run it from the helper's folder (with flamingo-worker.exe, dxc and worker.txt),
# from an administrator account. To remove: Unregister-ScheduledTask -TaskName "Flamingo Worker" -Confirm:$false
$ErrorActionPreference = "Stop"
$d = $PSScriptRoot
$name = "Flamingo Worker"
if (-not (Test-Path (Join-Path $d "flamingo-worker.exe"))) { throw "flamingo-worker.exe is not in $d" }

# what started by hand goes first
Get-Process flamingo-worker -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process dxc -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$d\*" } | Stop-Process -Force
Start-Sleep -Milliseconds 500

$action = New-ScheduledTaskAction -Execute "cmd.exe" -WorkingDirectory $d `
    -Argument "/c `"`"$d\flamingo-worker.exe`" > `"$d\worker.log`" 2>&1`""
$trigger = New-ScheduledTaskTrigger -AtStartup
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 999 `
    -RestartInterval (New-TimeSpan -Minutes 1) -StartWhenAvailable -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId "SYSTEM" -LogonType ServiceAccount -RunLevel Highest
Register-ScheduledTask -TaskName $name -Action $action -Trigger $trigger -Settings $settings -Principal $principal `
    -Description "Flamingo Port: compiles shaders for the main PC on the local network" -Force | Out-Null
Start-ScheduledTask -TaskName $name
Start-Sleep -Seconds 3
$t = Get-ScheduledTask -TaskName $name
"$name installed: $($t.State)"
