param([string]$ReportPath='artifacts/material-redesign/qa/native-capture-reserve-20260926/gpu.json')
$ErrorActionPreference='Stop'
$repository=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$buildDirectory=Join-Path $repository 'src-tauri/target/glass-capture-reserve-regression'
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
$generated=Get-ChildItem -Path (Join-Path $repository 'src-tauri/target/debug/build/tiez-app-*/out/glass_renderer_blur_ps.h') -File | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if(-not $generated){throw 'Build the native renderer first to generate shader headers.'}
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudio=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(-not $visualStudio){throw 'MSVC x64 Build Tools are required.'}
$vcvars=Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
$environmentLines=& $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
if($LASTEXITCODE -ne 0){throw 'Could not load the MSVC environment.'}
$names=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach($line in $environmentLines){if($line -match '^([^=]+)=(.*)$' -and $names.Add($matches[1])){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
$geometry=Join-Path $buildDirectory 'geometry.exe'
& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /utf-8 (Join-Path $PSScriptRoot 'glass_capture_region_regression.cpp') "/Fo$buildDirectory/" "/Fe$geometry"
if($LASTEXITCODE -ne 0){throw 'Capture region test compilation failed.'}
& $geometry
if($LASTEXITCODE -ne 0){throw 'Capture region regression failed.'}
$gpu=Join-Path $buildDirectory 'gpu.exe'
& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /utf-8 /DUNICODE /D_UNICODE "/I$($generated.DirectoryName)" (Join-Path $PSScriptRoot 'glass_capture_surface_regression.cpp') "/Fo$buildDirectory/" "/Fe$gpu" /link windowsapp.lib d3d11.lib d3dcompiler.lib dxgi.lib dxguid.lib dcomp.lib dwmapi.lib user32.lib gdi32.lib comctl32.lib
if($LASTEXITCODE -ne 0){throw 'Production capture surface test compilation failed.'}
$absoluteReport=if([IO.Path]::IsPathRooted($ReportPath)){$ReportPath}else{Join-Path $repository $ReportPath}
New-Item -ItemType Directory -Path (Split-Path -Parent $absoluteReport) -Force | Out-Null
& $gpu (Join-Path $repository 'src-tauri/native/glass_renderer.hlsl') $absoluteReport
$result=$LASTEXITCODE
if(Test-Path -LiteralPath $absoluteReport){
    $report=Get-Content -LiteralPath $absoluteReport -Raw | ConvertFrom-Json
    $report | Add-Member -NotePropertyName sourceHashes -NotePropertyValue @{
        renderer=(Get-FileHash -LiteralPath (Join-Path $repository 'src-tauri/native/glass_renderer.cpp') -Algorithm SHA256).Hash
        geometry=(Get-FileHash -LiteralPath (Join-Path $repository 'src-tauri/native/glass_capture_region.h') -Algorithm SHA256).Hash
        shader=(Get-FileHash -LiteralPath (Join-Path $repository 'src-tauri/native/glass_renderer.hlsl') -Algorithm SHA256).Hash
        harness=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'glass_capture_surface_regression.cpp') -Algorithm SHA256).Hash
        runner=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    }
    $report | Add-Member -NotePropertyName scope -NotePropertyValue 'Production renderer GPU methods and geometry acknowledgement reporting. No initialization, HWND, WGC, desktop input or real presentation validation.'
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $absoluteReport -Encoding UTF8
}
exit $result
