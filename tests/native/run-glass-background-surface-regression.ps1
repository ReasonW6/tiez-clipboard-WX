param([string]$ReportPath = 'artifacts/material-redesign/qa/background-surface-20260922/regression.json')
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$buildDirectory = Join-Path $repository 'src-tauri/target/glass-background-surface-regression'
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
$executable = Join-Path $buildDirectory 'glass_background_surface_regression.exe'
$source = Join-Path $PSScriptRoot 'glass_background_surface_regression.cpp'
$helper = Join-Path $repository 'src-tauri/native/glass_background_surface.h'
if (-not (Test-Path -LiteralPath $helper)) { throw 'The production background surface helper is required.' }
& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /utf-8 $source "/Fo$buildDirectory/" "/Fe$executable" /link d3d11.lib dxgi.lib dcomp.lib windowsapp.lib
if ($LASTEXITCODE -ne 0) { throw 'Background surface regression compilation failed.' }
$absoluteReport = if ([IO.Path]::IsPathRooted($ReportPath)) { $ReportPath } else { Join-Path $repository $ReportPath }
New-Item -ItemType Directory -Path (Split-Path -Parent $absoluteReport) -Force | Out-Null
$logPath = [IO.Path]::ChangeExtension($absoluteReport, '.txt')
& $executable $absoluteReport | Tee-Object -FilePath $logPath
$result = $LASTEXITCODE
if (Test-Path -LiteralPath $absoluteReport) {
    $report = Get-Content -LiteralPath $absoluteReport -Raw | ConvertFrom-Json
    $report | Add-Member -NotePropertyName sourceHashes -NotePropertyValue @{
        helper = (Get-FileHash -LiteralPath $helper -Algorithm SHA256).Hash
        epochLogic = (Get-FileHash -LiteralPath (Join-Path $repository 'src-tauri/native/glass_renderer.h') -Algorithm SHA256).Hash
        harness = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
        runner = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
        executable = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    }
    $report | Add-Member -NotePropertyName testedAtUtc -NotePropertyValue ([DateTime]::UtcNow.ToString('o'))
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $absoluteReport -Encoding UTF8
}
exit $result
