# SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference = 'Stop'
$framework = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319'
$output = Join-Path ([System.IO.Path]::GetTempPath()) ('privateCloud-installer-test-' + [guid]::NewGuid().ToString('N') + '.exe')
$sources = @('InstallState.cs', 'InstallLock.cs', 'Package.cs', 'InstallerCore.cs', 'WindowsIntegration.cs', 'InstallerTests.cs') | ForEach-Object { Join-Path $PSScriptRoot $_ }
& (Join-Path $framework 'csc.exe') /nologo /target:exe /platform:x64 /reference:System.Windows.Forms.dll /reference:System.Drawing.dll "/reference:$framework\System.IO.Compression.dll" "/out:$output" @sources
if ($LASTEXITCODE -ne 0) { throw 'Installer test compilation failed.' }
$launcher = $output + '.launcher.exe'
$application = $output + '.application.exe'
$common = $sources | Where-Object { [System.IO.Path]::GetFileName($_) -ne 'InstallerTests.cs' }
& (Join-Path $framework 'csc.exe') /nologo /target:winexe /platform:x64 /reference:System.Windows.Forms.dll /reference:System.Drawing.dll "/reference:$framework\System.IO.Compression.dll" "/out:$launcher" @common (Join-Path $PSScriptRoot 'Launcher.cs')
if ($LASTEXITCODE -ne 0) { throw 'Launcher fixture compilation failed.' }
& (Join-Path $framework 'csc.exe') /nologo /target:winexe /platform:x64 "/out:$application" (Join-Path $PSScriptRoot 'LauncherTestApplication.cs')
if ($LASTEXITCODE -ne 0) { throw 'Child fixture compilation failed.' }
& $output $launcher $application
if ($LASTEXITCODE -ne 0) { throw 'Installer tests failed.' }
