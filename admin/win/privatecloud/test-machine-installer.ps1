# SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference = 'Stop'
$framework = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319'
$output = Join-Path ([System.IO.Path]::GetTempPath()) ('privateCloud-machine-installer-test-' + [guid]::NewGuid().ToString('N') + '.exe')
$sources = @('InstallState.cs', 'Package.cs', 'MachineInstallPaths.cs', 'MachineInstallState.cs', 'MachineInstallerCore.cs', 'MachineInstallerTests.cs') | ForEach-Object { Join-Path $PSScriptRoot $_ }
& (Join-Path $framework 'csc.exe') /nologo /target:exe /platform:x64 "/reference:$framework\System.IO.Compression.dll" "/out:$output" @sources
if ($LASTEXITCODE -ne 0) { throw 'Machine installer test compilation failed.' }
& $output
if ($LASTEXITCODE -ne 0) { throw 'Machine installer tests failed.' }
