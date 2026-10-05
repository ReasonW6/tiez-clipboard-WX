param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../../artifacts/material-redesign/dwm-scene-probe'),
    [switch]$WithWebView,
    [switch]$RoundedRegion,
    [string]$WebViewSdkRoot = ''
)
$ErrorActionPreference = 'Stop'
$sceneOutput = [IO.Path]::GetFullPath($OutputDirectory)
$sceneSource = Join-Path $PSScriptRoot 'dwm_scene_probe.cpp'
$sceneExtraCompile = @()
$sceneExtraLink = @()
$sceneWebViewHeader = Join-Path $PSScriptRoot 'probe_webview.h'
if ($WithWebView) {
    if (-not $WebViewSdkRoot) {
        $sceneWebViewPackages = Join-Path $env:USERPROFILE '.nuget/packages/microsoft.web.webview2'
        $WebViewSdkRoot = Get-ChildItem -LiteralPath $sceneWebViewPackages -Directory |
            Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
            Sort-Object { [version]$_.Name } -Descending |
            ForEach-Object { Join-Path $_.FullName 'build/native' } |
            Where-Object { Test-Path -LiteralPath (Join-Path $_ 'include/WebView2.h') -PathType Leaf } |
            Select-Object -First 1
    }
    if (-not $WebViewSdkRoot) { throw 'An installed WebView2 native SDK is required for -WithWebView.' }
    $sceneWebViewInclude = Join-Path $WebViewSdkRoot 'include'
    $sceneWebViewLoader = Join-Path $WebViewSdkRoot 'x64/WebView2LoaderStatic.lib'
    if (-not (Test-Path -LiteralPath $sceneWebViewLoader -PathType Leaf)) { throw 'WebView2 x64 static loader is missing.' }
    $sceneExtraCompile = @('/DTIEZ_PROBE_WEBVIEW=1', "/I$sceneWebViewInclude")
    $sceneExtraLink = @($sceneWebViewLoader, 'ole32.lib', 'version.lib', 'shlwapi.lib', 'advapi32.lib')
}
if ($RoundedRegion) { $sceneExtraCompile += '/DTIEZ_PROBE_ROUNDED=1' }
$sceneVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$sceneVs = & $sceneVswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $sceneVs) { throw 'Visual Studio C++ Build Tools are required.' }
$sceneVcvars = Join-Path $sceneVs 'VC/Auxiliary/Build/vcvars64.bat'
New-Item -ItemType Directory -Path $sceneOutput -Force | Out-Null
$sceneEnvironment = & $env:ComSpec /d /s /c "`"$sceneVcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the C++ build environment.' }
foreach ($sceneLine in $sceneEnvironment) {
    if ($sceneLine -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}
$sceneCompilerPath = $sceneEnvironment | Where-Object { $_ -clike 'PATH=*' } | Select-Object -First 1
if ($sceneCompilerPath) { $env:Path = $sceneCompilerPath.Substring(5) }
$sceneExecutable = Join-Path $sceneOutput 'dwm_scene_probe.exe'
$sceneObject = Join-Path $sceneOutput 'dwm_scene_probe.obj'
$sceneFxc = Get-Command fxc.exe -ErrorAction SilentlyContinue
if ($sceneFxc) {
    $sceneFxcPath = $sceneFxc.Source
} else {
    $sceneSdkBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin'
    $sceneFxcPath = Get-ChildItem -LiteralPath $sceneSdkBin -Directory |
        Where-Object { $_.Name -match '^10\.\d+\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64/fxc.exe' } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if (-not $sceneFxcPath) { throw 'Windows SDK x64 fxc.exe is required.' }
}
$sceneShader = Join-Path $sceneOutput 'dwm_scene_probe.hlsl'
$sceneVertex = Join-Path $sceneOutput 'dwm_scene_probe.vs.cso'
$scenePixel = Join-Path $sceneOutput 'dwm_scene_probe.ps.cso'
$sceneVertexHeader = Join-Path $sceneOutput 'dwm_scene_probe_vs.generated.h'
$scenePixelHeader = Join-Path $sceneOutput 'dwm_scene_probe_ps.generated.h'
$sceneNativeInclude = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../native'))
$sceneOptics = Join-Path $sceneNativeInclude 'glass_optics.hlsli'
$sceneLayout = Join-Path $PSScriptRoot 'glass_probe_layout.hlsli'
$sceneCode = Get-Content -Raw -Encoding UTF8 -LiteralPath $sceneSource
$sceneMatch = [regex]::Match($sceneCode, '(?s)R"hlsl\((.*?)\)hlsl"')
if (-not $sceneMatch.Success) { throw 'Embedded scene shader not found.' }
[IO.File]::WriteAllText($sceneShader, $sceneMatch.Groups[1].Value, [Text.UTF8Encoding]::new($false))
& $sceneFxcPath /nologo /Ges /WX /O3 /T vs_5_0 /E VS /I $PSScriptRoot /I $sceneNativeInclude `
    /Fo $sceneVertex /Fh $sceneVertexHeader /Vn kDwmSceneProbeVS $sceneShader
if ($LASTEXITCODE -ne 0) { throw 'Scene vertex shader compilation failed.' }
& $sceneFxcPath /nologo /Ges /WX /O3 /T ps_5_0 /E PS /I $PSScriptRoot /I $sceneNativeInclude `
    /Fo $scenePixel /Fh $scenePixelHeader /Vn kDwmSceneProbePS $sceneShader
if ($LASTEXITCODE -ne 0) { throw 'Scene pixel shader compilation failed.' }
& cl.exe /nologo /EHsc /std:c++17 /O2 /MD /utf-8 /DUNICODE /D_UNICODE /I $sceneOutput @sceneExtraCompile `
    $sceneSource "/Fo$sceneObject" /link /SUBSYSTEM:WINDOWS "/OUT:$sceneExecutable" `
    windowsapp.lib CoreMessaging.lib d3d11.lib d2d1.lib dwrite.lib dxguid.lib dxgi.lib dcomp.lib dwmapi.lib user32.lib gdi32.lib @sceneExtraLink
if ($LASTEXITCODE -ne 0) { throw "Scene probe compilation failed with exit code $LASTEXITCODE." }
[ordered]@{
    builtAt = [DateTimeOffset]::Now.ToString('o')
    executable = $sceneExecutable
    executableSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $sceneExecutable).Hash
    sourceSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $sceneSource).Hash
    roundedRegion = [bool]$RoundedRegion
    roundedRadiusPx = $(if ($RoundedRegion) { 24 } else { 0 })
    webViewForeground = [bool]$WithWebView
    webViewHeaderSha256 = $(if ($WithWebView) { (Get-FileHash -Algorithm SHA256 -LiteralPath $sceneWebViewHeader).Hash } else { $null })
    webViewLoaderSha256 = $(if ($WithWebView) { (Get-FileHash -Algorithm SHA256 -LiteralPath $sceneWebViewLoader).Hash } else { $null })
    sourceEffectSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $PSScriptRoot 'probe_gaussian_blur.h')).Hash
    sourceMaterial = 'Windows.UI.Composition CreateBackdropBrush + GaussianBlur; sharp sibling text surface'
    opticsModuleSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $sceneOptics).Hash
    layoutModuleSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $sceneLayout).Hash
    vertexBytecodeSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $sceneVertex).Hash
    pixelBytecodeSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $scenePixel).Hash
    shaderProfiles = @('VS vs_5_0', 'PS ps_5_0')
    shaderCompilerFlags = '/Ges /WX /O3'
    shaderBytecodeEmbedded = $true
    runtimeShaderFileAccess = $false
    compilePassed = $true
    runtimeStartedByBuild = $false
    privateApi = 'dwmapi ordinals 163 / 164; Windows 11 ABI; experimental'
} | ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $sceneOutput 'build-evidence.json')
foreach ($sceneTemporary in @($sceneObject, $sceneShader, $sceneVertex, $scenePixel, $sceneVertexHeader, $scenePixelHeader)) {
    $sceneTemporaryItem = Get-Item -LiteralPath $sceneTemporary
    if ($sceneTemporaryItem.DirectoryName -ne $sceneOutput -or ($sceneTemporaryItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'Unexpected generated intermediate path.'
    }
    Remove-Item -LiteralPath $sceneTemporary
}
Get-FileHash -Algorithm SHA256 -LiteralPath $sceneExecutable | Format-List
Write-Output 'Compiled only; this script does not launch or capture.'
