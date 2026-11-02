# SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference = 'Stop'
$framework = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319'
$output = Join-Path ([System.IO.Path]::GetTempPath()) ('privateCloud-machine-security-test-' + [guid]::NewGuid().ToString('N') + '.exe')
$sources = @('InstallState.cs', 'Package.cs', 'MachineInstallPaths.cs', 'MachineInstallState.cs', 'MachineInstallerCore.cs', 'WindowsIntegration.cs', 'MachineWindowsService.cs', 'MachineWindowsIntegration.cs', 'SetupLog.cs', 'SetupPresentation.cs', 'SetupProgressWindow.cs', 'MachineSetupProgram.cs', 'MachineWindowsIntegrationTests.cs') | ForEach-Object { Join-Path $PSScriptRoot $_ }
& (Join-Path $framework 'csc.exe') /nologo /target:exe /platform:x64 /main:PrivateCloud.Setup.MachineWindowsIntegrationTests /reference:System.Windows.Forms.dll /reference:System.Drawing.dll "/reference:$framework\System.IO.Compression.dll" "/out:$output" @sources
if ($LASTEXITCODE -ne 0) { throw 'Machine security test compilation failed.' }
& $output
if ($LASTEXITCODE -ne 0) { throw 'Machine security tests failed.' }
