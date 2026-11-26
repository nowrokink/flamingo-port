# Builds tools\worker\build\flamingo-worker.exe (MSVC, static CRT) and puts next to it the dxc.exe and dxcompiler.dll
# of this PC's Vulkan SDK, so a helper PC gets the same compiler as the main one: that folder is all it needs.
$ErrorActionPreference = "Stop"

$root = Split-Path $PSScriptRoot -Parent
$installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vs = & "$installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ x64 tools wasn't found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

$src = Join-Path $root "tools\worker"
$out = Join-Path $src "build"
New-Item -ItemType Directory -Force $out | Out-Null

$bat = Join-Path $out "_build.cmd"
@"
@echo off
set "PATH=$installer;%PATH%"
call "$vcvars" >nul || exit /b 1
cd /d "$out" || exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX "$src\flamingo_worker.cpp" /Fe:flamingo-worker.exe /link winhttp.lib || exit /b 1
"@ | Set-Content -Encoding ascii $bat
& cmd /c "`"$bat`""
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$sdk = [Environment]::GetEnvironmentVariable("VULKAN_SDK", "Machine")
if (-not $sdk) { $sdk = $env:VULKAN_SDK }
if (-not $sdk) { throw "VULKAN_SDK is not set: install the Vulkan SDK (it has dxc)." }
foreach ($f in "dxc.exe", "dxcompiler.dll") { Copy-Item (Join-Path $sdk "Bin\$f") $out -Force }
# dxc needs the Visual C++ runtime; a helper PC may not have it (or an old one), so it goes along, app-local
foreach ($f in "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll") {
    Copy-Item (Join-Path $env:WINDIR "System32\$f") $out -Force
}
"built $out\flamingo-worker.exe (with dxc and the Visual C++ runtime it needs)"
