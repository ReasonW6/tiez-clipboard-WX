$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
$originalSigningKey = [Environment]::GetEnvironmentVariable('TAURI_SIGNING_PRIVATE_KEY')
Push-Location -LiteralPath $repoRoot
try {
    if (-not $env:TAURI_SIGNING_PRIVATE_KEY) {
        $localSigningKey = Join-Path $repoRoot '.tauri\updater.key'
        if (-not (Test-Path -LiteralPath $localSigningKey -PathType Leaf)) {
            throw 'Set TAURI_SIGNING_PRIVATE_KEY or restore .tauri\updater.key before building an update.'
        }
        $env:TAURI_SIGNING_PRIVATE_KEY = $localSigningKey
    }
    & npm.cmd run tauri -- build --ci --config src-tauri/tauri.release.conf.json
    if ($LASTEXITCODE -ne 0) { throw "Release build failed with exit code $LASTEXITCODE." }
}
finally {
    [Environment]::SetEnvironmentVariable('TAURI_SIGNING_PRIVATE_KEY', $originalSigningKey)
    Pop-Location
}
