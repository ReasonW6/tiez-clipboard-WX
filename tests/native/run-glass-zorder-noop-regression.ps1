param([string]$ReportPath = 'artifacts/material-redesign/qa/presentation-continuity-20260923/policy.json')
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$buildDirectory = Join-Path $repository 'src-tauri/target/glass-zorder-noop-regression'
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
$source = Join-Path $PSScriptRoot 'glass_zorder_noop_regression.cpp'
$nativeDirectory = Join-Path $repository 'src-tauri/native'
$policy = Join-Path $nativeDirectory 'glass_zorder_noop.h'
$executable = Join-Path $buildDirectory 'glass_zorder_noop_regression.exe'
& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /utf-8 "/I$nativeDirectory" $source "/Fo$buildDirectory/" "/Fe$executable"
if ($LASTEXITCODE -ne 0) { throw 'Z-order no-op regression compilation failed.' }
$absoluteReport = if ([IO.Path]::IsPathRooted($ReportPath)) { $ReportPath } else { Join-Path $repository $ReportPath }
New-Item -ItemType Directory -Path (Split-Path -Parent $absoluteReport) -Force | Out-Null
& $executable $absoluteReport
$result = $LASTEXITCODE
$negativeControls = @()
if ($result -eq 0) {
    $policyText = Get-Content -LiteralPath $policy -Raw
    $functionPattern = '(?s)inline bool z_order_request_is_noop\(.*?\r?\n}\r?\n(?=\r?\ninline bool z_order_request_can_be_observed)'
    if ([regex]::Matches($policyText, $functionPattern).Count -ne 1) { throw 'Expected one production classifier.' }
    $earlyCheck = 'return current(token) && default_returned && within_deadline(now) && same_z_order(order, snapshot);'
    if (-not $policyText.Contains($earlyCheck)) { throw 'Expected the production default-procedure completion gate.' }
    $mutations = @(
        @{ Name = 'legacy-unconditional-stop'; Body = 'return false;'; ReplaceGate = $false },
        @{ Name = 'ignore-order-and-band'; Body = 'return pure_z_order_request(position, order.host, allow_no_z_order);'; ReplaceGate = $false },
        @{ Name = 'finish-inside-default-procedure'; ReplaceGate = $true }
    )
    foreach ($mutation in $mutations) {
        $mutationDirectory = Join-Path $buildDirectory $mutation.Name
        New-Item -ItemType Directory -Path $mutationDirectory -Force | Out-Null
        $mutationPolicy = Join-Path $mutationDirectory 'glass_zorder_noop.h'
        $mutationText = if ($mutation.ReplaceGate) {
            $policyText.Replace($earlyCheck, 'return current(token) && within_deadline(now) && same_z_order(order, snapshot);')
        } else {
            $replacement = "inline bool z_order_request_is_noop(const WINDOWPOS& position, const ZOrderSnapshot& order, bool allow_no_z_order = false) {`n    (void)position; (void)order; (void)allow_no_z_order; $($mutation.Body)`n}`n"
            [regex]::Replace($policyText, $functionPattern, $replacement)
        }
        $mutationText | Set-Content -LiteralPath $mutationPolicy -Encoding UTF8
        $mutationExecutable = Join-Path $mutationDirectory 'regression.exe'
        & cl.exe /nologo /std:c++17 /EHsc /W4 /WX /O2 /utf-8 "/I$mutationDirectory" $source "/Fo$mutationDirectory/" "/Fe$mutationExecutable"
        if ($LASTEXITCODE -ne 0) { throw "Negative-control compilation failed: $($mutation.Name)" }
        $mutationReport = Join-Path (Split-Path -Parent $absoluteReport) "$($mutation.Name).json"
        $negativeErrorPreference = $ErrorActionPreference
        try {
            # A rejected mutation writes its expected failing checks to stderr.
            # Windows PowerShell otherwise promotes that evidence to a pipeline
            # exception before the explicit exit/report checks below can run.
            $ErrorActionPreference = 'Continue'
            & $mutationExecutable $mutationReport 2>&1 | Set-Content -LiteralPath ([IO.Path]::ChangeExtension($mutationReport, '.txt')) -Encoding UTF8
            $mutationResult = $LASTEXITCODE
        } finally { $ErrorActionPreference = $negativeErrorPreference }
        $mutationEvidence = Get-Content -LiteralPath $mutationReport -Raw | ConvertFrom-Json
        if ($mutationResult -eq 0 -or $mutationEvidence.failed -lt 1) { throw "Negative control was not detected: $($mutation.Name)" }
        $negativeControls += @{
            name = $mutation.Name
            detected = $true
            failedChecks = $mutationEvidence.failed
            policyHash = (Get-FileHash -LiteralPath $mutationPolicy -Algorithm SHA256).Hash
        }
    }
}
if (Test-Path -LiteralPath $absoluteReport) {
    $report = Get-Content -LiteralPath $absoluteReport -Raw | ConvertFrom-Json
    $report | Add-Member -NotePropertyName sourceHashes -NotePropertyValue @{
        policy = (Get-FileHash -LiteralPath $policy -Algorithm SHA256).Hash
        renderer = (Get-FileHash -LiteralPath (Join-Path $nativeDirectory 'glass_renderer.cpp') -Algorithm SHA256).Hash
        harness = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
        runner = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
        executable = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    }
    $report | Add-Member -NotePropertyName testedAtUtc -NotePropertyValue ([DateTime]::UtcNow.ToString('o'))
    $report | Add-Member -NotePropertyName negativeControls -NotePropertyValue $negativeControls
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $absoluteReport -Encoding UTF8
}
exit $result
