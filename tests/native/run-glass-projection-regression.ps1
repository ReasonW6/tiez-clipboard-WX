param(
    [string]$ReportPath = 'artifacts/material-redesign/qa/glass-projection-regression.json'
)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$buildDirectory = Join-Path $repository 'src-tauri/target/glass-projection-regression'
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
$shader = Join-Path $repository 'src-tauri/native/glass_renderer.hlsl'
$opticsPath = Join-Path $repository 'src-tauri/native/glass_optics.hlsli'
$optics = Get-Content -LiteralPath $opticsPath -Raw
$projectionPattern = 'float projectionHeight = height / \(1\.0 \+ 0\.5 \* max\(glassIor - 1\.52, 0\.0\)\);'
if ([regex]::Matches($optics, $projectionPattern).Count -ne 1) {
    throw 'Review the unbounded projection negative control after the production projection formula changes.'
}
$probe = @'

// Test-only observation; blurDirection carries IOR and thickness, while the
// otherwise-unused trackMeta.w chooses the wavelength. All optics are the
// production helper, executed at real raster sample centers in float32.
float4 ProjectionProbePS(Vertex input) : SV_Target
{
    GlassControl control = controls[0];
    GlassOptics optics = GlassEvaluate(input.position.xy, control.rectXYWH,
        control.shape.x, blurDirection.y, blurDirection.x, control.shape.y);
    float2 offset = trackMeta.w < 0.5 ? optics.offsetR
        : trackMeta.w < 1.5 ? optics.offsetG : optics.offsetB;
    return float4(input.position.xy + offset, optics.coverage, optics.thickness);
}
'@
$shaderPaths = @()
foreach ($variant in @('current', 'unbounded-projection')) {
    $directory = Join-Path $buildDirectory $variant
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $shaderPath = Join-Path $directory 'glass_renderer.hlsl'
    [IO.File]::WriteAllText($shaderPath, (Get-Content -LiteralPath $shader -Raw) + $probe)
    $variantOptics = if ($variant -eq 'current') { $optics } else {
        [regex]::Replace($optics, $projectionPattern, 'float projectionHeight = height;')
    }
    [IO.File]::WriteAllText((Join-Path $directory 'glass_optics.hlsli'), $variantOptics)
    Copy-Item -LiteralPath (Join-Path $repository 'src-tauri/native/glass_prefix.hlsli') -Destination (Join-Path $directory 'glass_prefix.hlsli')
    $shaderPaths += $shaderPath
}
$source = Join-Path $PSScriptRoot 'glass_projection_regression.cpp'
$executable = Join-Path $buildDirectory 'glass-projection-regression.exe'
& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /utf-8 $source "/Fo$buildDirectory/" "/Fe$executable" /link d3d11.lib d3dcompiler.lib
if ($LASTEXITCODE -ne 0) { throw 'Projection regression compilation failed.' }
$absoluteReport = if ([IO.Path]::IsPathRooted($ReportPath)) { $ReportPath } else { Join-Path $repository $ReportPath }
& $executable $shaderPaths[0] $shaderPaths[1] $absoluteReport
$testExitCode = $LASTEXITCODE
if (Test-Path -LiteralPath $absoluteReport) {
    $report = Get-Content -LiteralPath $absoluteReport -Raw | ConvertFrom-Json
    $report.surfaceFormat = 'R32G32B32A32_FLOAT coordinate probe (not final material color)'
    $report | Add-Member -NotePropertyName sourceHashes -NotePropertyValue @{
        shader = (Get-FileHash -LiteralPath $shader -Algorithm SHA256).Hash
        optics = (Get-FileHash -LiteralPath (Join-Path $buildDirectory 'current/glass_optics.hlsli') -Algorithm SHA256).Hash
        harness = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
        sharedHarness = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'glass_gpu_regression.cpp') -Algorithm SHA256).Hash
        runner = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    } -Force
    $report | Add-Member -NotePropertyName testedAtUtc -NotePropertyValue ([DateTime]::UtcNow.ToString('o')) -Force
    $report | Add-Member -NotePropertyName matrix -NotePropertyValue '4 geometries x 2 pressures x 4 paired XY phases x 5 base IORs x 3 channels; 480 float32 coordinate frames plus 1 unbounded negative control. Existing green/material thresholds are unchanged.' -Force
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $absoluteReport -Encoding UTF8
}
exit $testExitCode
