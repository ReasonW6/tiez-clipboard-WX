param([switch]$SkipBuild)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
$outputRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'artifacts\portable'))
$stagePath = Join-Path $outputRoot ('stage-' + [guid]::NewGuid().ToString('N'))
$zipPath = Join-Path $outputRoot 'tiez-portable.zip'
Push-Location -LiteralPath $repoRoot
try {
    if (-not $SkipBuild) {
        & npm.cmd run tauri -- build --no-bundle
        if ($LASTEXITCODE -ne 0) { throw "Portable build failed with exit code $LASTEXITCODE." }
    }
    $executable = Join-Path $repoRoot 'src-tauri\target\release\tiez-app.exe'
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "Release executable not found: $executable"
    }
    if (Test-Path -LiteralPath $outputRoot) {
        if (((Get-Item -Force -LiteralPath $outputRoot).Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw 'Portable output directory cannot be a reparse point.'
        }
    }
    New-Item -ItemType Directory -Path (Join-Path $stagePath 'data') -Force | Out-Null
    Copy-Item -LiteralPath $executable -Destination (Join-Path $stagePath 'TieZ.exe')
    Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination (Join-Path $stagePath 'LICENSE')
    [System.IO.File]::WriteAllText((Join-Path $stagePath 'README.txt'),
        "Extract this folder before running TieZ.exe. Data is stored in the adjacent data folder.`r`nSensitive clipboard data remains protected by the current Windows account.`r`nAutomatic updates install the setup edition; to keep using this folder, replace TieZ.exe from a newer portable archive while preserving data.`r`n")
    if (Test-Path -LiteralPath $zipPath) {
        if (((Get-Item -Force -LiteralPath $zipPath).Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw 'Portable archive cannot be a reparse point.'
        }
        Remove-Item -LiteralPath $zipPath
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory($stagePath, $zipPath)
    Write-Output $zipPath
}
finally {
    if (Test-Path -LiteralPath $stagePath) {
        $resolvedStage = (Resolve-Path -LiteralPath $stagePath).Path
        if (-not $resolvedStage.StartsWith($outputRoot + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw 'Refusing to clean a staging directory outside the portable output directory.'
        }
        if (((Get-Item -Force -LiteralPath $resolvedStage).Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw 'Refusing to clean a staging directory that is a reparse point.'
        }
        Remove-Item -LiteralPath $resolvedStage -Recurse -Force
    }
    Pop-Location
}
