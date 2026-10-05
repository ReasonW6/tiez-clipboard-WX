param([string]$ReportPath='artifacts/material-redesign/qa/glass-body-regression.json')
$ErrorActionPreference='Stop'
$repository=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$buildDirectory=Join-Path $repository 'src-tauri/target/glass-body-regression'
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
$opticsPath=Join-Path $repository 'src-tauri/native/glass_optics.hlsli'
$shaderPath=Join-Path $repository 'src-tauri/native/glass_renderer.hlsl'
$optics=Get-Content -LiteralPath $opticsPath -Raw
$camera='float focalDistance = max(6.0 * restingHeight, 0.3 * max(rectXYWH.z, rectXYWH.w));'
$direction='float2 local = pixel - rectXYWH.xy - 0.5 * rectXYWH.zw;'
$projection='float projectionHeight = height / (1.0 + 0.5 * max(glassIor - 1.52, 0.0));'
$bodyPattern='(optics\.offset[RGB]) = bodyWindow \* GlassBodyOffset\([^;]+;'
foreach($marker in @($camera,$direction,$projection)) {
  if([regex]::Matches($optics,[regex]::Escape($marker)).Count -ne 1) { throw 'Review body negative controls after optics changes' }
}
if([regex]::Matches($optics,$bodyPattern).Count -ne 3) { throw 'Review flat-core negative control' }
$probe=@'

float4 ProjectionProbePS(Vertex input) : SV_Target
{
    GlassControl control = controls[0];
    float dpi = options.w;
    float2 probePixel = tintRgba.xy + input.position.xy * tintRgba.zw;
    GlassOptics optics = GlassEvaluate(probePixel * dpi, control.rectXYWH * dpi,
        control.shape.x * dpi, blurDirection.y * dpi, blurDirection.x, control.shape.y);
    float2 offset = trackMeta.w < 0.5 ? optics.offsetR
        : trackMeta.w < 1.5 ? optics.offsetG : optics.offsetB;
    return float4(probePixel + offset / dpi, optics.coverage, optics.thickness / dpi);
}
'@
$paths=@()
foreach($variant in @('current','fixed-pixel-camera','moving-pressure-camera','medial-direction','flat-core','unbounded-projection')) {
  $folder=Join-Path $buildDirectory $variant
  New-Item -ItemType Directory -Path $folder -Force | Out-Null
  $changed=switch($variant) {
    'fixed-pixel-camera' { $optics.Replace($camera,'float focalDistance = max(64.0, 0.3 * max(rectXYWH.z, rectXYWH.w));') }
    'moving-pressure-camera' { $optics.Replace('6.0 * restingHeight','6.0 * height') }
    'medial-direction' { $optics.Replace($direction,'float2 local = boundary.outward * length(pixel - rectXYWH.xy - 0.5 * rectXYWH.zw);') }
    'flat-core' { [regex]::Replace($optics,$bodyPattern,'$1 = 0.0;') }
    'unbounded-projection' { $optics.Replace($projection,'float projectionHeight = height;') }
    default { $optics }
  }
  [IO.File]::WriteAllText((Join-Path $folder 'glass_optics.hlsli'),$changed)
  $path=Join-Path $folder 'glass_renderer.hlsl'
  [IO.File]::WriteAllText($path,(Get-Content -LiteralPath $shaderPath -Raw)+$probe)
  Copy-Item -LiteralPath (Join-Path $repository 'src-tauri/native/glass_prefix.hlsli') -Destination (Join-Path $folder 'glass_prefix.hlsli')
  $paths+=$path
}
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudio=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(-not $visualStudio) { throw 'MSVC x64 tools required' }
$vcvars=Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
$environmentLines=& $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
if($LASTEXITCODE -ne 0) { throw 'Could not load MSVC environment' }
$importedNames=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach($line in $environmentLines) { if($line -match '^([^=]+)=(.*)$' -and $importedNames.Add($matches[1])) { [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process') } }
$executable=Join-Path $buildDirectory 'glass-body-regression.exe'
$source=Join-Path $PSScriptRoot 'glass_body_regression.cpp'
& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /utf-8 $source "/Fo$buildDirectory/" "/Fe$executable" /link d3d11.lib d3dcompiler.lib
if($LASTEXITCODE -ne 0) { throw 'Body regression did not compile' }
$absoluteReport=if([IO.Path]::IsPathRooted($ReportPath)){$ReportPath}else{Join-Path $repository $ReportPath}
New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($absoluteReport)) -Force | Out-Null
& $executable @paths $absoluteReport
$testExitCode=$LASTEXITCODE
if(Test-Path -LiteralPath $absoluteReport) {
  $report=Get-Content -LiteralPath $absoluteReport -Raw | ConvertFrom-Json
  $report.surfaceFormat='R32G32B32A32_FLOAT logical coordinates / coverage / thickness'
  $report | Add-Member -NotePropertyName scope -NotePropertyValue 'Production helper: 6 geometries (thickness/width/height camera branches) x 3 pressures x 4 phases x 4 IORs x 3 wavelengths, DPI 1/1.25/1.5/2/3; independent slab oracle, symmetry, dense continuity and negative controls. No color/presentation/iOS proof.' -Force
  $report | Add-Member -NotePropertyName sourceHashes -NotePropertyValue @{
    optics=(Get-FileHash -LiteralPath $opticsPath).Hash
    shader=(Get-FileHash -LiteralPath $shaderPath).Hash
    harness=(Get-FileHash -LiteralPath $source).Hash
    shared=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'glass_projection_regression.cpp')).Hash
    runner=(Get-FileHash -LiteralPath $PSCommandPath).Hash
  } -Force
  $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $absoluteReport -Encoding utf8
}
exit $testExitCode
