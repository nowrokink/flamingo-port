$d = Join-Path $env:USERPROFILE "FlamingoWorker"
Get-Process flamingo-worker -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process dxc -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$d\*" } | Stop-Process -Force
Start-Sleep -Milliseconds 500
$cmd = "cmd /c `"`"$d\flamingo-worker.exe`" > `"$d\worker.log`" 2>&1`""
$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $cmd; CurrentDirectory = $d }
"started pid $($r.ProcessId) rc $($r.ReturnValue)"
