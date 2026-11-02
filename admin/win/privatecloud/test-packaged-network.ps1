# SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
# SPDX-License-Identifier: GPL-2.0-or-later
param(
    [Parameter(Mandatory=$true)][string]$Package,
    [Parameter(Mandatory=$true)][string]$TestBin,
    [Parameter(Mandatory=$true)][string]$QtTestDll,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference = 'Stop'
$packageRoot = (Resolve-Path -LiteralPath $Package).Path
$testRoot = (Resolve-Path -LiteralPath $TestBin).Path
$qtTest = (Resolve-Path -LiteralPath $QtTestDll).Path
$outputRoot = [IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $outputRoot) { throw 'The isolated test output must not exist.' }
$tests = @('PrivateCloudAuthenticationTest', 'PrivateCloudTransportTest', 'PrivateCloudConnectRelayTest')
foreach ($name in $tests) {
    if (-not (Test-Path -LiteralPath (Join-Path $testRoot ($name + '.exe')) -PathType Leaf)) {
        throw ('Missing test executable: ' + $name)
    }
}
New-Item -ItemType Directory -Path $outputRoot | Out-Null
Get-ChildItem -LiteralPath $packageRoot -File -Filter '*.dll' | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $outputRoot
}
Copy-Item -LiteralPath (Join-Path $packageRoot 'tls') -Destination $outputRoot -Recurse
Copy-Item -LiteralPath $qtTest -Destination (Join-Path $outputRoot 'Qt6Test.dll')
# Prevent the development installation's plugin path from masking missing files.
Set-Content -LiteralPath (Join-Path $outputRoot 'qt.conf') -Encoding ASCII -Value "[Paths]`nPrefix=.`nPlugins=."
$saved = @{}
$isolated = @{
    PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    QT_PLUGIN_PATH = $outputRoot
    QT_QPA_PLATFORM = 'offscreen'
    PRIVATECLOUD_DIRECT_HTTPS_PROBE = $null
}
try {
    foreach ($name in $isolated.Keys) {
        $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $isolated[$name], 'Process')
    }
    foreach ($name in $tests) {
        $binary = Join-Path $outputRoot ($name + '.exe')
        Copy-Item -LiteralPath (Join-Path $testRoot ($name + '.exe')) -Destination $binary
        $report = Join-Path $outputRoot ($name + '.txt')
        $process = Start-Process -FilePath $binary -WorkingDirectory $outputRoot -WindowStyle Hidden -PassThru `
            -ArgumentList ('-o "' + $report + ',txt"')
        if (-not $process.WaitForExit(90000)) {
            $process.Kill()
            throw ('Isolated network test timed out: ' + $name)
        }
        $process.Refresh()
        if ($process.ExitCode -ne 0) { throw ('Isolated network test failed: ' + $report) }
        Select-String -LiteralPath $report -Pattern '^Totals:|^SKIP' | ForEach-Object { Write-Output ($name + ': ' + $_.Line) }
    }
} finally {
    foreach ($name in $saved.Keys) {
        [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process')
    }
}
