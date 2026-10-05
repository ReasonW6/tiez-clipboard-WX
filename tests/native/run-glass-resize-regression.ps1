param([string]$ReportPath = 'artifacts/material-redesign/qa/glass-resize-gpu-regression.json')
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$buildDirectory = Join-Path $repository 'src-tauri/target/glass-resize-regression'
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $visualStudio) { throw 'MSVC x64 Build Tools are required.' }
$vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
$environmentLines = & $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Could not load the MSVC environment.' }
$importedNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($line in $environmentLines) {
    if ($line -match '^([^=]+)=(.*)$' -and $importedNames.Add($matches[1])) {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
$executable = Join-Path $buildDirectory 'glass_resize_regression.exe'
$source = Join-Path $PSScriptRoot 'glass_resize_regression.cpp'
& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /utf-8 $source "/Fo$buildDirectory/" "/Fe$executable" /link gdi32.lib
if ($LASTEXITCODE -ne 0) { throw 'Native resize regression compilation failed.' }
& $executable
if ($LASTEXITCODE -ne 0) { throw 'Native resize state regression failed.' }
$lifetimeExecutable = Join-Path $buildDirectory 'glass_swapchain_lifetime_regression.exe'
$lifetimeSource = Join-Path $PSScriptRoot 'glass_swapchain_lifetime_regression.cpp'
& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /utf-8 $lifetimeSource "/Fo$buildDirectory/" "/Fe$lifetimeExecutable" /link d3d11.lib dxgi.lib dcomp.lib
if ($LASTEXITCODE -ne 0) { throw 'Swap-chain lifetime regression compilation failed.' }
& $lifetimeExecutable
if ($LASTEXITCODE -ne 0) { throw 'Swap-chain lifetime regression failed.' }
$gpuExecutable = Join-Path $buildDirectory 'glass_resize_gpu_regression.exe'
$gpuSource = Join-Path $PSScriptRoot 'glass_resize_gpu_regression.cpp'
& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /utf-8 $gpuSource "/Fo$buildDirectory/" "/Fe$gpuExecutable" /link d3d11.lib d3dcompiler.lib
if ($LASTEXITCODE -ne 0) { throw 'Native resize GPU regression compilation failed.' }
$absoluteReport = if ([IO.Path]::IsPathRooted($ReportPath)) { $ReportPath } else { Join-Path $repository $ReportPath }
New-Item -ItemType Directory -Path (Split-Path -Parent $absoluteReport) -Force | Out-Null
$shader = Join-Path $repository 'src-tauri/native/glass_renderer.hlsl'
& $gpuExecutable $shader $absoluteReport
$result = $LASTEXITCODE
if (Test-Path -LiteralPath $absoluteReport) {
    $report = Get-Content -LiteralPath $absoluteReport -Raw | ConvertFrom-Json
    $report | Add-Member -NotePropertyName sourceHashes -NotePropertyValue @{
        shader = (Get-FileHash -LiteralPath $shader -Algorithm SHA256).Hash
        renderer = (Get-FileHash -LiteralPath (Join-Path $repository 'src-tauri/native/glass_renderer.cpp') -Algorithm SHA256).Hash
        lifetimeLogic = (Get-FileHash -LiteralPath (Join-Path $repository 'src-tauri/native/glass_renderer.h') -Algorithm SHA256).Hash
        harness = (Get-FileHash -LiteralPath $gpuSource -Algorithm SHA256).Hash
        sharedHarness = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'glass_gpu_regression.cpp') -Algorithm SHA256).Hash
        runner = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
        swapchainLifetimeHarness = (Get-FileHash -LiteralPath $lifetimeSource -Algorithm SHA256).Hash
    }
    $report | Add-Member -NotePropertyName testedAtUtc -NotePropertyValue ([DateTime]::UtcNow.ToString('o')) -Force
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $absoluteReport -Encoding UTF8
}
exit $result
