# SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference = 'Stop'
$compiler = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$output = Join-Path ([System.IO.Path]::GetTempPath()) ('privateCloud-presentation-test-' + [guid]::NewGuid().ToString('N') + '.exe')
$sources = @('SetupPresentation.cs', 'SetupPresentationTests.cs') | ForEach-Object { Join-Path $PSScriptRoot $_ }
& $compiler /nologo /target:exe /platform:x64 /warnaserror+ "/out:$output" @sources
if ($LASTEXITCODE -ne 0) { throw 'Setup presentation test compilation failed.' }
& $output
if ($LASTEXITCODE -ne 0) { throw 'Setup presentation tests failed.' }
