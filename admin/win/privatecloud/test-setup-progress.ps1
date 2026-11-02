# SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
# SPDX-License-Identifier: GPL-2.0-or-later
param(
    [string]$OutputDirectory = (Join-Path ([IO.Path]::GetTempPath()) ('privateCloud-setup-progress-' + [guid]::NewGuid().ToString('N'))),
    [string]$LogPath = ''
)
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($LogPath)) { $LogPath = Join-Path $OutputDirectory 'setup-progress-tests.log' }
$compiler = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$sourceDirectory = $PSScriptRoot
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
New-Item -ItemType Directory -Path (Split-Path -Parent $LogPath) -Force | Out-Null
$executable = Join-Path $OutputDirectory 'SetupProgressWindowTests.exe'
$arguments = @('/nologo', '/target:exe', '/platform:x64', '/warnaserror+',
    '/reference:System.Windows.Forms.dll', '/reference:System.Drawing.dll',
    '/main:PrivateCloud.Setup.SetupProgressWindowTests', "/out:$executable",
    (Join-Path $sourceDirectory 'SetupProgressWindow.cs'),
    (Join-Path $sourceDirectory 'SetupProgressWindowTests.cs'))
$buildOutput = & $compiler @arguments 2>&1
$buildResult = $LASTEXITCODE
$buildOutput | Set-Content -LiteralPath $LogPath
if ($buildResult -ne 0) { $buildOutput | Write-Output; throw "Setup progress test compilation failed ($buildResult)." }
$testOutput = & $executable 2>&1
$testResult = $LASTEXITCODE
$testOutput | Tee-Object -FilePath $LogPath -Append
if ($testResult -ne 0) { throw "Setup progress tests failed ($testResult). See $LogPath" }
Write-Output "Preview: $executable --preview success (or failure)"
