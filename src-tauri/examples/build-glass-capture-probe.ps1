param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../../artifacts/material-redesign/probe'),
    [ValidateSet('single', 'multi')][string]$ProbeKind = 'single'
)
$ErrorActionPreference = 'Stop'
$glassOutput = [IO.Path]::GetFullPath($OutputDirectory)
$glassProbeName = if ($ProbeKind -eq 'multi') { 'glass_multi_capture_probe' } else { 'glass_capture_probe' }
$glassSource = Join-Path $PSScriptRoot ($glassProbeName + '.cpp')
$glassVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$glassVs = & $glassVswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $glassVs) { throw 'Visual Studio C++ Build Tools are required.' }
$glassVcvars = Join-Path $glassVs 'VC/Auxiliary/Build/vcvars64.bat'
New-Item -ItemType Directory -Path $glassOutput -Force | Out-Null
# Import the compiler environment into this build process; no global settings change.
$glassEnvironment = & $env:ComSpec /d /s /c "`"$glassVcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the C++ build environment.' }
foreach ($glassLine in $glassEnvironment) {
    if ($glassLine -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}
$glassCompilerPath = $glassEnvironment | Where-Object { $_ -clike 'PATH=*' } | Select-Object -First 1
if ($glassCompilerPath) { $env:Path = $glassCompilerPath.Substring(5) }
$glassFxc = Get-Command fxc.exe -ErrorAction SilentlyContinue
if ($glassFxc) {
    $glassFxcPath = $glassFxc.Source
} else {
    $glassSdkBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin'
    $glassFxcPath = Get-ChildItem -LiteralPath $glassSdkBin -Directory |
        Where-Object { $_.Name -match '^10\.\d+\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64/fxc.exe' } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if (-not $glassFxcPath) { throw 'Windows SDK x64 fxc.exe is required.' }
}
$glassExecutable = Join-Path $glassOutput ($glassProbeName + '.exe')
$glassObject = Join-Path $glassOutput ($glassProbeName + '.obj')
$glassShader = Join-Path $glassOutput ($glassProbeName + '.hlsl')
$glassVertex = Join-Path $glassOutput ($glassProbeName + '.vs.cso')
$glassPixel = Join-Path $glassOutput ($glassProbeName + '.ps.cso')
$glassVertexHeader = Join-Path $glassOutput ($glassProbeName + '_vs.generated.h')
$glassPixelHeader = Join-Path $glassOutput ($glassProbeName + '_ps.generated.h')
$glassNativeInclude = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../native'))
$glassOptics = Join-Path $glassNativeInclude 'glass_optics.hlsli'
$glassLayout = Join-Path $PSScriptRoot 'glass_probe_layout.hlsli'
$glassCode = Get-Content -Raw -Encoding UTF8 -LiteralPath $glassSource
$glassMatch = [regex]::Match($glassCode, '(?s)R"hlsl\((.*?)\)hlsl"')
if (-not $glassMatch.Success) { throw 'Embedded probe shader not found.' }
[IO.File]::WriteAllText($glassShader, $glassMatch.Groups[1].Value, [Text.UTF8Encoding]::new($false))
& $glassFxcPath /nologo /Ges /WX /O3 /T vs_5_0 /E VS /I $PSScriptRoot /I $glassNativeInclude `
    /Fo $glassVertex /Fh $glassVertexHeader /Vn kGlassProbeVS $glassShader
if ($LASTEXITCODE -ne 0) { throw 'Probe vertex shader compilation failed.' }
& $glassFxcPath /nologo /Ges /WX /O3 /T ps_5_0 /E PS /I $PSScriptRoot /I $glassNativeInclude `
    /Fo $glassPixel /Fh $glassPixelHeader /Vn kGlassProbePS $glassShader
if ($LASTEXITCODE -ne 0) { throw 'Probe pixel shader compilation failed.' }
& cl.exe /nologo /EHsc /std:c++17 /O2 /MD /utf-8 /DUNICODE /D_UNICODE /I $glassOutput `
    $glassSource "/Fo$glassObject" /link /SUBSYSTEM:WINDOWS "/OUT:$glassExecutable" `
    windowsapp.lib d3d11.lib dxgi.lib dcomp.lib dwmapi.lib user32.lib gdi32.lib
if ($LASTEXITCODE -ne 0) { throw "Probe compilation failed with exit code $LASTEXITCODE." }
$glassBuild = [ordered]@{
    builtAt = [DateTimeOffset]::Now.ToString('o')
    executable = $glassExecutable
    executableSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $glassExecutable).Hash
    sourceSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $glassSource).Hash
    opticsModuleSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $glassOptics).Hash
    layoutModuleSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $glassLayout).Hash
    vertexBytecodeSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $glassVertex).Hash
    pixelBytecodeSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $glassPixel).Hash
    cxx = 'MSVC x64 C++17'
    shaders = @('VS vs_5_0', 'PS ps_5_0')
    compilePassed = $true
    runtimeStartedByBuild = $false
    probeKind = $ProbeKind
    shaderBytecodeEmbedded = $true
    runtimeShaderFileAccess = $false
}
$glassBuildName = if ($ProbeKind -eq 'multi') { 'multi-build-evidence.json' } else { 'build-evidence.json' }
$glassBuild | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $glassOutput $glassBuildName)
foreach ($glassTemporary in @($glassObject, $glassShader, $glassVertex, $glassPixel, $glassVertexHeader, $glassPixelHeader)) {
    $glassTemporaryItem = Get-Item -LiteralPath $glassTemporary
    if ($glassTemporaryItem.DirectoryName -ne $glassOutput -or
        ($glassTemporaryItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Unexpected generated intermediate path: $glassTemporary"
    }
    Remove-Item -LiteralPath $glassTemporary
}
Get-FileHash -Algorithm SHA256 -LiteralPath $glassExecutable | Format-List
Write-Output "Compiled only. Launch the project-owned probe explicitly: $glassExecutable"
