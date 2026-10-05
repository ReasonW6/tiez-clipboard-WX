param(
    [string]$ReportPath = 'artifacts/material-redesign/qa/glass-material-pipeline-regression.json',
    [string]$OpticsSource = 'src-tauri/native/glass_optics.hlsli'
)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$buildDirectory = Join-Path $repository 'src-tauri/target/glass-material-pipeline-regression'
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
$productionShader = Join-Path $repository 'src-tauri/native/glass_renderer.hlsl'
$productionOptics = if ([IO.Path]::IsPathRooted($OpticsSource)) { $OpticsSource } else { Join-Path $repository $OpticsSource }
$materialSourcePath = Join-Path $repository 'src-tauri/src/infrastructure/window_material.rs'
$materialSource = Get-Content -LiteralPath $materialSourcePath -Raw
$foregroundPaths = @(
    (Join-Path $repository 'src/styles/themes/materials.css'),
    (Join-Path $repository 'src/styles/themes/liquid-glass-settings.css')
    (Join-Path $repository 'src/styles/themes/liquid-glass.css')
)
$foreground = ($foregroundPaths | ForEach-Object { Get-Content -LiteralPath $_ -Raw }) -join "`n"
foreach ($declaration in @(
    '--text-primary: #17212e;', '--text-primary: #f6f8fc;',
    '--text-secondary: #1d2b3b;', '--text-secondary: #e1e7f0;',
    '--text-muted: #2d3d50;', '--text-muted: #d4deeb;',
    '--material-link-color: #173f7a;', '--material-link-color: #d4e2ff;',
    '--text-muted: #e1e7f0;', '--material-link-color: #e0eaff;',
    'color: #831f2b !important;', 'color: #ffdde5 !important;'
)) {
    if (-not $foreground.Contains($declaration)) { throw 'Foreground palette changed; update the material contrast fixture before claiming acceptance.' }
}
# The fixture values are the actual Rust material parameters, not an easier
# synthetic profile. Fail explicitly if that mapping changes without updating QA.
foreach ($pattern in @(
    'Material::Clear\s*=>\s*\(1\.5,\s*16\.0\)',
    '\(0\.74\s*\+\s*density\s*\*\s*0\.16\)\s*as\s*f32',
    '\[23\.0,\s*31\.0,\s*43\.0\]',
    '\[245\.0,\s*248\.0,\s*252\.0\]'
)) {
    if ($materialSource -notmatch $pattern) { throw 'Rust material parameters changed; update the full-pipeline fixture before claiming acceptance.' }
}
$optics = Get-Content -LiteralPath $productionOptics -Raw
$dispersionPattern = 'float dispersion = min\(0\.16, 0\.28 \* \(glassIor - 1\.0\)\);'
$offsetPattern = '(optics\.offset[RGB]) \+= GlassSnellOffset\([^;]+;'
$bodyOffsetPattern = '(optics\.offset[RGB]) = bodyWindow \* GlassBodyOffset\([^;]+;'
$pressureCapPattern = 'shoulder \* 0\.75 \* \(1\.0 \+ 0\.12 \* press\)'
$surfaceLodPattern = 'float lod = GlassSurfaceLod\(control\);'
if ([regex]::Matches($optics, $dispersionPattern).Count -ne 1 -or [regex]::Matches($optics, $offsetPattern).Count -ne 3 -or [regex]::Matches($optics, $bodyOffsetPattern).Count -ne 3) {
    throw 'Production optics changed; inspect the isolated negative controls before running.'
}
if ([regex]::Matches($optics, $pressureCapPattern).Count -ne 1) { throw 'Review the fixed pressure-cap negative control after the height formula changes.' }
if ([regex]::Matches((Get-Content -LiteralPath $productionShader -Raw), $surfaceLodPattern).Count -ne 1) {
    throw 'Review the constant-footprint oracle and depth-filter negative control after surface filtering changes.'
}
$pressureProbe = @'

// Test-only float32 observation of the actual production helper.
float4 PressureProbePS(Vertex input) : SV_Target
{
    GlassControl control = controls[0];
    GlassOptics optics = GlassEvaluate(input.position.xy, control.rectXYWH,
        control.shape.x, 26.0 * max(options.w, 0.01), 1.52, control.shape.y);
    return float4(input.position.xy + optics.offsetG, optics.coverage, optics.thickness);
}
float4 SurfaceLodProbePS(Vertex input) : SV_Target
{
    return float4(GlassSurfaceLod(controls[0]), 0.0, 0.0, 1.0);
}
'@
$shaderPaths = @()
foreach ($variant in @('current', 'zero-dispersion', 'zero-offsets', 'rejected-rim-tint', 'fixed-pressure-cap', 'depth-filter', 'constant-lod')) {
    $directory = Join-Path $buildDirectory $variant
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $shaderPath = Join-Path $directory 'glass_renderer.hlsl'
    Copy-Item -LiteralPath $productionShader -Destination $shaderPath
    $prefixText = Get-Content -LiteralPath (Join-Path $repository 'src-tauri/native/glass_prefix.hlsli') -Raw
    if ($variant -eq 'rejected-rim-tint') {
        $shaderText = Get-Content -LiteralPath $shaderPath -Raw
        $uniformTintPattern = '(SampleGlass\(pixel \+ optics\.offset[RGB], lod, )tintRgba\.a(\)\.[rgb])'
        if ([regex]::Matches($shaderText, $uniformTintPattern).Count -ne 3) { throw 'Review the rejected rim-tint negative control after shader sampling changes.' }
        $rejectedTint = '$1lerp(max(0.32, tintRgba.a - 0.38), tintRgba.a, smoothstep(0.0, 0.75, smoothstep(0.35 * radius, max(radius, dpi), max(-boundary.distance, 0.0))))$2'
        [IO.File]::WriteAllText($shaderPath, [regex]::Replace($shaderText, $uniformTintPattern, $rejectedTint))
        $prefixTintMarker = '    float width = min(dpi, radius);'
        if (-not $prefixText.Contains($prefixTintMarker)) { throw 'Review the prefix rim-tint negative control.' }
        $prefixTintNegative = @'
    // Counterfactual: undo the single-surface global coat and reapply a
    // depth-dependent coat. This deliberately restores the rejected dark rim.
    float rejectedTint = lerp(max(0.32, tintRgba.a - 0.38), tintRgba.a,
        smoothstep(0.0, 0.75, smoothstep(0.35 * radius, max(radius, dpi), max(-boundary.distance, 0.0))));
    refracted = lerp(saturate((refracted - tintRgba.rgb * tintRgba.a) / max(1.0 - tintRgba.a, 0.00001)), tintRgba.rgb, rejectedTint);
'@
        $prefixText = $prefixText.Replace($prefixTintMarker, $prefixTintNegative + "`n" + $prefixTintMarker)
    }
    if ($variant -eq 'depth-filter') {
        $shaderText = Get-Content -LiteralPath $shaderPath -Raw
        # Preserve the rejected production profile as a negative: clear
        # shoulders transitioning into geometry-dependent frosted interiors.
        $depthFilter = @'
float body = smoothstep(0.35 * radius, max(radius, dpi), max(-boundary.distance, 0.0));
        float bodySpread = (clamp(min(control.rectXYWH.z, control.rectXYWH.w) * 0.075,
            1.25 * dpi, 7.0 * dpi) + 0.20 * max(options.x, 0.0)) * sqrt(12.0);
        float lod = log2(max(1.0, lerp(0.65 * dpi, 1.0 + bodySpread, body)));
'@
        [IO.File]::WriteAllText($shaderPath, [regex]::Replace($shaderText, $surfaceLodPattern, $depthFilter))
        $prefixText = [regex]::Replace($prefixText, $surfaceLodPattern, $depthFilter)
    }
    if ($variant -eq 'constant-lod') {
        $shaderText = Get-Content -LiteralPath $shaderPath -Raw
        [IO.File]::WriteAllText($shaderPath, [regex]::Replace($shaderText, $surfaceLodPattern, 'float lod = control.shape.w;'))
        $prefixText = [regex]::Replace($prefixText, $surfaceLodPattern, 'float lod = control.shape.w;')
    }
    [IO.File]::AppendAllText($shaderPath, $pressureProbe)
    $variantOptics = switch ($variant) {
        'zero-dispersion' { [regex]::Replace($optics, $dispersionPattern, 'float dispersion = 0.0;') }
        'zero-offsets' { [regex]::Replace([regex]::Replace($optics, $bodyOffsetPattern, '$1 = 0.0;'), $offsetPattern, '$1 += 0.0;') }
        'fixed-pressure-cap' { [regex]::Replace($optics, $pressureCapPattern, 'shoulder * 0.75') }
        default { $optics }
    }
    [IO.File]::WriteAllText((Join-Path $directory 'glass_optics.hlsli'), $variantOptics)
    [IO.File]::WriteAllText((Join-Path $directory 'glass_prefix.hlsli'), $prefixText)
    $shaderPaths += $shaderPath
}
$source = Join-Path $PSScriptRoot 'glass_material_pipeline_regression.cpp'
$executable = Join-Path $buildDirectory 'glass-material-pipeline-regression.exe'
& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /utf-8 $source "/Fo$buildDirectory/" "/Fe$executable" /link d3d11.lib d3dcompiler.lib
if ($LASTEXITCODE -ne 0) { throw 'Full material regression compilation failed.' }
$absoluteReport = if ([IO.Path]::IsPathRooted($ReportPath)) { $ReportPath } else { Join-Path $repository $ReportPath }
New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($absoluteReport)) -Force | Out-Null
& $executable $shaderPaths[0] $shaderPaths[1] $shaderPaths[2] $shaderPaths[3] $shaderPaths[4] $shaderPaths[5] $shaderPaths[6] $absoluteReport
$testExitCode = $LASTEXITCODE
if (Test-Path -LiteralPath $absoluteReport) {
    $report = Get-Content -LiteralPath $absoluteReport -Raw | ConvertFrom-Json
    $report | Add-Member -NotePropertyName sourceHashes -NotePropertyValue @{
        shader = (Get-FileHash -LiteralPath $productionShader -Algorithm SHA256).Hash
        shaderWithTestProbe = (Get-FileHash -LiteralPath $shaderPaths[0] -Algorithm SHA256).Hash
        optics = (Get-FileHash -LiteralPath (Join-Path $buildDirectory 'current/glass_optics.hlsli') -Algorithm SHA256).Hash
        prefixShader = (Get-FileHash -LiteralPath (Join-Path $buildDirectory 'current/glass_prefix.hlsli') -Algorithm SHA256).Hash
        materialParameters = (Get-FileHash -LiteralPath $materialSourcePath -Algorithm SHA256).Hash
        materialForeground = (Get-FileHash -LiteralPath $foregroundPaths[0] -Algorithm SHA256).Hash
        settingsForeground = (Get-FileHash -LiteralPath $foregroundPaths[1] -Algorithm SHA256).Hash
        liquidForeground = (Get-FileHash -LiteralPath $foregroundPaths[2] -Algorithm SHA256).Hash
        prefixHarness = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'glass_prefix_gpu.h') -Algorithm SHA256).Hash
        sharedHarness = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'glass_gpu_regression.cpp') -Algorithm SHA256).Hash
        harness = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
        surfaceFilterHarness = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'glass_surface_filter_regression.h') -Algorithm SHA256).Hash
        depthFilterNegative = (Get-FileHash -LiteralPath $shaderPaths[5] -Algorithm SHA256).Hash
        constantLodOracle = (Get-FileHash -LiteralPath $shaderPaths[6] -Algorithm SHA256).Hash
        runner = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    } -Force
    $report | Add-Member -NotePropertyName testedAtUtc -NotePropertyValue ([DateTime]::UtcNow.ToString('o')) -Force
    $report | Add-Member -NotePropertyName materialPipeline -NotePropertyValue 'BGRA8 raw input and window blur base; one premultiplied tint; ordered RGBA16F optical prefix with R16F union coverage; each track precedes its thumb; final BGRA8 output.' -Force
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $absoluteReport -Encoding UTF8
}
exit $testExitCode
