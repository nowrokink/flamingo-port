# Builds the runtime and runs its tests: the CPU unit test, then the GPU layer test on translated hand-written shaders.
#   .\scripts\test_runtime.ps1               # build + test
#   .\scripts\test_runtime.ps1 -Validation   # with the Khronos validation layer (Vulkan SDK)
param([switch]$Validation, [switch]$NoBuild)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent

if (-not $env:VULKAN_SDK) { $env:VULKAN_SDK = [Environment]::GetEnvironmentVariable("VULKAN_SDK", "Machine") }
if (-not $env:VULKAN_SDK) { throw "Vulkan SDK not found: install it from https://vulkan.lunarg.com/" }
$spirvAs = Join-Path $env:VULKAN_SDK "Bin\spirv-as.exe"
$spirvVal = Join-Path $env:VULKAN_SDK "Bin\spirv-val.exe"

if (-not $NoBuild) { & (Join-Path $PSScriptRoot "build_runtime.ps1") }
$build = Join-Path $root "runtime\build"
# The tests log to stderr; judge them by exit code, not by PowerShell's stderr-as-error rule.
$ErrorActionPreference = "Continue"

Write-Host "`n== xmem_test"
& (Join-Path $build "xmem_test.exe")
if ($LASTEXITCODE -ne 0) { throw "xmem_test failed" }

Write-Host "`n== files_test"
& (Join-Path $build "files_test.exe")
if ($LASTEXITCODE -ne 0) { throw "files_test failed" }

Write-Host "`n== net_test"
& (Join-Path $build "net_test.exe")
if ($LASTEXITCODE -ne 0) { throw "net_test failed" }

Write-Host "`n== storage_test"
& (Join-Path $build "storage_test.exe")
if ($LASTEXITCODE -ne 0) { throw "storage_test failed" }

Write-Host "`n== gamesave_test"
& (Join-Path $build "gamesave_test.exe")
if ($LASTEXITCODE -ne 0) { throw "gamesave_test failed" }

Write-Host "`n== frame_test"
& (Join-Path $build "frame_test.exe")
if ($LASTEXITCODE -ne 0) { throw "frame_test failed" }

Write-Host "`n== xtaskqueue_test"
& (Join-Path $build "xtaskqueue_test.exe")
if ($LASTEXITCODE -ne 0) { throw "xtaskqueue_test failed" }

Write-Host "`n== pad_test"
& (Join-Path $build "pad_test.exe")
if ($LASTEXITCODE -ne 0) { throw "pad_test failed" }

Write-Host "`n== cpu_emul_test"
& (Join-Path $build "cpu_emul_test.exe")
if ($LASTEXITCODE -ne 0) { throw "cpu_emul_test failed" }

Write-Host "`n== inflate_test"
& (Join-Path $build "inflate_test.exe")
if ($LASTEXITCODE -ne 0) { throw "inflate_test failed" }

# With samples in temp\bcpack (tools\bcpack_samples.py, each with its .expected decode), also byte for byte.
Write-Host "`n== bcpack_test"
$bcpackSamples = Join-Path $root "temp\bcpack"
if (Test-Path $bcpackSamples) { & (Join-Path $build "bcpack_test.exe") $bcpackSamples } else { & (Join-Path $build "bcpack_test.exe") }
if ($LASTEXITCODE -ne 0) { throw "bcpack_test failed" }

Write-Host "`n== gfx10_layout_test"
& (Join-Path $build "gfx10_layout_test.exe")
if ($LASTEXITCODE -ne 0) { throw "gfx10_layout_test failed" }

Write-Host "`n== stage_meta_test"
& (Join-Path $build "stage_meta_test.exe")
if ($LASTEXITCODE -ne 0) { throw "stage_meta_test failed" }

Write-Host "`n== gpu_memory_test"
$dxc = Join-Path $env:VULKAN_SDK "Bin\dxc.exe"
& $dxc -T cs_6_6 -E main (Join-Path $root "runtime\test\gpu_memory_test.hlsl") -Fo (Join-Path $build "gpu_memory_test.cso")
if ($LASTEXITCODE -ne 0) { throw "dxc failed on gpu_memory_test.hlsl" }
Push-Location $build
& (Join-Path $build "gpu_memory_test.exe")
$rc = $LASTEXITCODE
Pop-Location
if ($rc -ne 0) { throw "gpu_memory_test failed" }

Write-Host "`n== uma_test"
& (Join-Path $build "uma_test.exe")
if ($LASTEXITCODE -ne 0) { throw "uma_test failed" }

Write-Host "`n== gpu_unit_test"
& (Join-Path $build "gpu_unit_test.exe")
if ($LASTEXITCODE -ne 0) { throw "gpu_unit_test failed" }

# Assemble the translator's reference outputs that the GPU test draws.
$spv = Join-Path $build "spv"
New-Item -ItemType Directory -Force $spv | Out-Null
foreach ($n in "VS_FullscreenUV", "PS_SampleHeapDescriptors", "PS_gpu_ps_sdwa", "PS_gpu_ps_ops") {
    $src = Join-Path $root "tests\translator\expected\$n.spvasm"
    $dst = Join-Path $spv "$n.spv"
    & $spirvAs $src -o $dst --target-env vulkan1.2
    if ($LASTEXITCODE -ne 0) { throw "spirv-as failed on $n" }
    & $spirvVal --target-env vulkan1.2 $dst
    if ($LASTEXITCODE -ne 0) { throw "spirv-val failed on $n" }
}

Write-Host "`n== gpu_layer_test"
$testArgs = @($spv)
if ($Validation) { $testArgs += "--validation" }
& (Join-Path $build "gpu_layer_test.exe") @testArgs
if ($LASTEXITCODE -ne 0) { throw "gpu_layer_test failed" }
