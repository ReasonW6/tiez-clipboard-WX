param(
    [string]$ReportPath = 'artifacts/material-redesign/qa/glass-track-warp-regression.json'
)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$buildDirectory = Join-Path $repository 'src-tauri/target/glass-gpu-regression'
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $visualStudio) { throw 'MSVC x64 Build Tools are required.' }
$vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
# Import the compiler environment only. Compile arguments are passed as separate
# PowerShell arguments, never interpolated into a cmd command string.
$environmentLines = & $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Could not load the MSVC environment.' }
$importedNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($line in $environmentLines) {
    # Some hosts inherit both PATH and Path. vcvars updates PATH, while the
    # later stale Path would otherwise overwrite it in PowerShell's environment.
    if ($line -match '^([^=]+)=(.*)$' -and $importedNames.Add($matches[1])) {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
$executable = Join-Path $buildDirectory 'glass_gpu_regression.exe'
$source = Join-Path $PSScriptRoot 'glass_gpu_regression.cpp'
& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /utf-8 $source "/Fo$buildDirectory/" "/Fe$executable" /link d3d11.lib d3dcompiler.lib
if ($LASTEXITCODE -ne 0) { throw 'GPU regression compilation failed.' }
$shader = Join-Path $repository 'src-tauri/native/glass_renderer.hlsl'
$absoluteReport = if ([IO.Path]::IsPathRooted($ReportPath)) { $ReportPath } else { Join-Path $repository $ReportPath }
New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($absoluteReport)) -Force | Out-Null
& $executable $shader $absoluteReport
$testExitCode = $LASTEXITCODE
if (Test-Path -LiteralPath $absoluteReport) {
    $report = Get-Content -LiteralPath $absoluteReport -Raw | ConvertFrom-Json
    $report | Add-Member -NotePropertyName sourceHashes -NotePropertyValue @{
        shader = (Get-FileHash -LiteralPath $shader -Algorithm SHA256).Hash
        optics = (Get-FileHash -LiteralPath (Join-Path $repository 'src-tauri/native/glass_optics.hlsli') -Algorithm SHA256).Hash
        harness = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    } -Force
    $report | Add-Member -NotePropertyName testedAtUtc -NotePropertyValue ([DateTime]::UtcNow.ToString('o')) -Force
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $absoluteReport -Encoding UTF8
}
exit $testExitCode
