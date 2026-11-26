# Builds runtime\build\FlamingoPort.exe (the launcher, the preparation and the runtime in one program) and the
# runtime's tests, with VS's CMake + Ninja, at BelowNormal priority, on half the cores.
param([switch]$Debug)
$ErrorActionPreference = "Stop"

$root = Split-Path $PSScriptRoot -Parent
$installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vs = & "$installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ x64 tools wasn't found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$jobs = [Math]::Max(1, [int]([Environment]::ProcessorCount / 2))

$src = Join-Path $root "runtime"
$out = Join-Path $src "build"
New-Item -ItemType Directory -Force $out | Out-Null

$cmakeDir = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake"
$cmake = Join-Path $cmakeDir "CMake\bin\cmake.exe"
$ninja = Join-Path $cmakeDir "Ninja\ninja.exe"
if (-not ((Test-Path $cmake) -and (Test-Path $ninja))) { throw "VS's 'C++ CMake tools for Windows' component is needed." }
$cfg = if ($Debug) { "Debug" } else { "RelWithDebInfo" }

$body = @"
"$cmake" -S "$src" -B "$out" -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=$cfg || exit /b 1
"$cmake" --build "$out" -j $jobs || exit /b 1
"@

# A .cmd file avoids cmd.exe's quote stripping on paths with spaces.
$bat = Join-Path $out "_build.cmd"
"@echo off`r`nset `"PATH=$installer;%PATH%`"`r`ncall `"$vcvars`" >nul || exit /b 1`r`n$body" | Set-Content -Encoding ascii $bat

$p = Start-Process cmd.exe -ArgumentList "/c", "`"$bat`"" -NoNewWindow -PassThru
$null = $p.Handle  # keeps the exit code readable after WaitForExit
try { $p.PriorityClass = "BelowNormal" } catch {}
$p.WaitForExit()
if ($p.ExitCode -ne 0) { throw "build failed ($($p.ExitCode))" }
Write-Host "built $out\FlamingoPort.exe"
