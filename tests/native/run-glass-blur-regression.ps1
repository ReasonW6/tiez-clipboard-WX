param(
    [string]$ReportPath = 'artifacts/material-redesign/qa/glass-blur-regression.json',
    [switch]$BuildOnly
)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$buildDirectory = Join-Path $repository 'src-tauri/target/glass-blur-regression'
New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null
$productionShader = Join-Path $repository 'src-tauri/native/glass_renderer.hlsl'
$productionOptics = Join-Path $repository 'src-tauri/native/glass_optics.hlsli'
$shader = Get-Content -LiteralPath $productionShader -Raw
$start = $shader.IndexOf('float4 BlurPS(Vertex input)')
$end = $shader.IndexOf('float3 CompositeTracks(', $start + 1)
if ($start -lt 0 -or $end -lt $start) { throw 'Review the independent blur references after the shader structure changes.' }
$prefix = @'
float4 BlurPS(Vertex input) : SV_Target
{
    float sigma = clamp(options.x, 0.0, 128.0);
    float4 blurred;
    if (sigma <= 0.001)
        blurred = sourceImage.SampleLevel(linearClamp, input.uv, 0.0);
    else
    {
'@
$suffix = @'
        blurred = accumulated / weightSum;
    }
    if (options.z <= 0.5) return blurred;
    float3 color = blurred.a > 0.00001 ? blurred.rgb / blurred.a : tintRgba.rgb;
    color = lerp(color, tintRgba.rgb, saturate(tintRgba.a));
    return float4(color, 1.0);
}

'@
$denseBody = @'
        // Independent integer-lattice Gaussian oracle. No paired offsets/mips.
        int radius = int(ceil(3.0 * sigma));
        float4 accumulated = 0.0;
        float weightSum = 0.0;
        [loop]
        for (int tap = -radius; tap <= radius; ++tap)
        {
            float distance = float(tap) / sigma;
            float weight = exp(-0.5 * distance * distance);
            accumulated += sourceImage.SampleLevel(linearClamp,
                input.uv + blurDirection * float(tap) / sceneSize, 0.0) * weight;
            weightSum += weight;
        }
'@
$sparseBody = @'
        // Frozen pre-fix sampling rule, also the low-sigma compatibility oracle.
        float2 stepUv = blurDirection * (sigma * 0.25) / sceneSize;
        float4 accumulated = 0.0;
        float weightSum = 0.0;
        [unroll]
        for (int tap = -12; tap <= 12; ++tap)
        {
            float normalizedDistance = float(tap) * 0.25;
            float weight = exp(-0.5 * normalizedDistance * normalizedDistance);
            accumulated += sourceImage.SampleLevel(linearClamp,
                input.uv + float(tap) * stepUv, 0.0) * weight;
            weightSum += weight;
        }
'@
$paths = @()
foreach ($variant in @('current', 'dense', 'legacy-sparse')) {
    $directory = Join-Path $buildDirectory $variant
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $variantShader = $shader
    if ($variant -ne 'current') {
        $body = if ($variant -eq 'dense') { $denseBody } else { $sparseBody }
        $function = $prefix + "`n" + $body + "`n" + $suffix
        $variantShader = $shader.Substring(0, $start) + $function + $shader.Substring($end)
    }
    $path = Join-Path $directory 'glass_renderer.hlsl'
    [IO.File]::WriteAllText($path, $variantShader)
    Copy-Item -LiteralPath $productionOptics -Destination (Join-Path $directory 'glass_optics.hlsli')
    Copy-Item -LiteralPath (Join-Path $repository 'src-tauri/native/glass_prefix.hlsli') -Destination (Join-Path $directory 'glass_prefix.hlsli')
    $paths += $path
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $visualStudio) { throw 'MSVC x64 Build Tools are required.' }
$vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
$lines = & $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Could not load the MSVC environment.' }
$names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($line in $lines) {
    if ($line -match '^([^=]+)=(.*)$' -and $names.Add($matches[1])) {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
$source = Join-Path $PSScriptRoot 'glass_blur_regression.cpp'
$executable = Join-Path $buildDirectory 'glass-blur-regression.exe'
& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /utf-8 $source "/Fo$buildDirectory/" "/Fe$executable" /link d3d11.lib d3dcompiler.lib
if ($LASTEXITCODE -ne 0) { throw 'Blur regression compilation failed.' }
if ($BuildOnly) { exit 0 }
$absoluteReport = if ([IO.Path]::IsPathRooted($ReportPath)) { $ReportPath } else { Join-Path $repository $ReportPath }
New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($absoluteReport)) | Out-Null
& $executable $paths[0] $paths[1] $paths[2] $absoluteReport
$testExitCode = $LASTEXITCODE
if (Test-Path -LiteralPath $absoluteReport) {
    $report = Get-Content -LiteralPath $absoluteReport -Raw | ConvertFrom-Json
    $report.PSObject.Properties.Remove('warpTiming')
    $report | Add-Member -NotePropertyName sourceHashes -NotePropertyValue @{
        productionShaderSnapshot = (Get-FileHash -LiteralPath $paths[0] -Algorithm SHA256).Hash
        opticsSnapshot = (Get-FileHash -LiteralPath (Join-Path $buildDirectory 'current/glass_optics.hlsli') -Algorithm SHA256).Hash
        denseReference = (Get-FileHash -LiteralPath $paths[1] -Algorithm SHA256).Hash
        legacySparseReference = (Get-FileHash -LiteralPath $paths[2] -Algorithm SHA256).Hash
        sharedHarness = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'glass_gpu_regression.cpp') -Algorithm SHA256).Hash
        harness = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
        runner = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    }
    $report | Add-Member -NotePropertyName testedAtUtc -NotePropertyValue ([DateTime]::UtcNow.ToString('o'))
    $report | Add-Member -NotePropertyName scope -NotePropertyValue 'Actual production BlurPS with RGBA16F intermediate and BGRA8 output. Independent dense Gaussian and legacy sparse negative control. No OpticsPS draws, UI, capture or timing.'
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $absoluteReport -Encoding UTF8
}
exit $testExitCode
