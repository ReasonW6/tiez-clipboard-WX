param([string]$ReportPath='artifacts/liquid-glass-performance-20261005/gpu-frames.json', [switch]$Runtime,
    [ValidateSet('light','dark')][string]$ColorMode='light')
$ErrorActionPreference='Stop'
$repository=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$buildDirectory=Join-Path $repository 'src-tauri/target/glass-frame-regression'
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
$generated=Get-ChildItem -Path (Join-Path $repository 'src-tauri/target/debug/build/tiez-app-*/out/glass_renderer_blur_ps.h') -File | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if(-not $generated){throw 'Build the native renderer first to generate shader headers.'}
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudio=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
$environmentLines=& $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
if($LASTEXITCODE -ne 0){throw 'Could not load MSVC.'}
$names=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach($line in $environmentLines){if($line -match '^([^=]+)=(.*)$' -and $names.Add($matches[1])){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
$binary=Join-Path $buildDirectory 'frames.exe'
& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /utf-8 /DUNICODE /D_UNICODE "/I$($generated.DirectoryName)" (Join-Path $PSScriptRoot 'glass_frame_regression.cpp') "/Fo$buildDirectory/" "/Fe$binary" /link windowsapp.lib d3d11.lib d3dcompiler.lib dxgi.lib dxguid.lib dcomp.lib dwmapi.lib user32.lib gdi32.lib comctl32.lib
if($LASTEXITCODE -ne 0){throw 'Frame regression compilation failed.'}
$absoluteReport=if([IO.Path]::IsPathRooted($ReportPath)){$ReportPath}else{Join-Path $repository $ReportPath}
New-Item -ItemType Directory -Path (Split-Path -Parent $absoluteReport) -Force | Out-Null
if ($Runtime) {
    $runtimeMode=if($ColorMode -eq 'dark'){'--runtime-dark'}else{'--runtime'}
    & $binary (Join-Path $repository 'src-tauri/native/glass_renderer.hlsl') $absoluteReport $runtimeMode
} else {
    & $binary (Join-Path $repository 'src-tauri/native/glass_renderer.hlsl') $absoluteReport
}
$resultCode=$LASTEXITCODE
if($resultCode -eq 0){
    $result=Get-Content -LiteralPath $absoluteReport -Raw | ConvertFrom-Json
    $result | Add-Member -NotePropertyName sourceHashes -NotePropertyValue ([ordered]@{
        renderer=(Get-FileHash -LiteralPath (Join-Path $repository 'src-tauri/native/glass_renderer.cpp') -Algorithm SHA256).Hash
        shader=(Get-FileHash -LiteralPath (Join-Path $repository 'src-tauri/native/glass_renderer.hlsl') -Algorithm SHA256).Hash
        harness=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'glass_frame_regression.cpp') -Algorithm SHA256).Hash
        runtimeHarness=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'glass_runtime_regression.h') -Algorithm SHA256).Hash
        zOrderPolicy=(Get-FileHash -LiteralPath (Join-Path $repository 'src-tauri/native/glass_zorder_noop.h') -Algorithm SHA256).Hash
        executable=(Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
        runner=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    })
    [IO.File]::WriteAllText($absoluteReport,($result | ConvertTo-Json -Depth 10),[Text.UTF8Encoding]::new($false))
}
exit $resultCode
