# SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
# SPDX-License-Identifier: GPL-2.0-or-later
param(
    [Parameter(Mandatory=$true)][string]$Source,
    [Parameter(Mandatory=$true)][string]$Build,
    [Parameter(Mandatory=$true)][string]$Dependencies,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference = 'Stop'
$sourceRoot = (Resolve-Path -LiteralPath $Source).Path
$buildRoot = (Resolve-Path -LiteralPath $Build).Path
$depsRoot = (Resolve-Path -LiteralPath $Dependencies).Path
$outputRoot = [IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $outputRoot) { throw 'Packaging output must be a new directory.' }
if ($outputRoot.StartsWith($sourceRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Use a packaging output outside the source tree.'
}
$packaging = Join-Path $sourceRoot 'admin\win\privatecloud'
$bin = Join-Path $buildRoot 'bin'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ redistributable files were not found.' }
$redist = Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Redist\MSVC') -Directory |
    Where-Object Name -Match '^14\.' | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$redist) { throw 'Visual Studio C++ redistributable version was not found.' }
New-Item -ItemType Directory -Path $outputRoot | Out-Null
$brokerBuild = Join-Path $outputRoot 'broker-build'
& cmake -S (Join-Path $sourceRoot 'admin\win\privatecloud-direct-broker') -B $brokerBuild -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'Helper configuration failed.' }
& cmake --build $brokerBuild --config Release --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Helper compilation failed.' }
& ctest --test-dir $brokerBuild -C Release --output-on-failure --timeout 60 -E BrokerPipeLifecycleNativeTests
if ($LASTEXITCODE -ne 0) { throw 'Helper tests failed.' }
$runtime = Join-Path $outputRoot 'runtime'
& python -B (Join-Path $packaging 'stage-runtime.py') --source $sourceRoot --build-bin $bin --deps $depsRoot `
    --package $runtime --broker (Join-Path $brokerBuild 'Release\privateCloudDirectBroker.exe') `
    --redist (Join-Path $redist.FullName 'x64')
if ($LASTEXITCODE -ne 0) { throw 'Runtime staging failed.' }
$env:PRIVATECLOUD_PACKAGE_UNDER_TEST = $runtime
$env:PRIVATECLOUD_PACKAGE_SCRIPT_UNDER_TEST = Join-Path $packaging 'stage-runtime.py'
& python -B -m unittest discover -s $packaging -p 'test_*.py' -v
if ($LASTEXITCODE -ne 0) { throw 'Installer admission tests failed.' }
foreach ($test in @('test-installer.ps1', 'test-machine-installer.ps1', 'test-machine-windows-integration.ps1', 'test-setup-bootstrap.ps1', 'test-setup-presentation.ps1', 'test-setup-progress.ps1')) {
    & (Join-Path $packaging $test)
    if ($LASTEXITCODE -ne 0) { throw "$test failed" }
}
& (Join-Path $packaging 'test-packaged-network.ps1') -Package $runtime -TestBin $bin `
    -QtTestDll (Join-Path $depsRoot 'bin\Qt6Test.dll') -Output (Join-Path $outputRoot 'network-tests')
$versionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $runtime 'privateCloud.exe'))
$version = '{0}.{1}.{2}.{3}' -f $versionInfo.FileMajorPart, $versionInfo.FileMinorPart, $versionInfo.FileBuildPart, $versionInfo.FilePrivatePart
$installerOutput = Join-Path $outputRoot 'installer-build'
& python -B (Join-Path $packaging 'build-machine-installer.py') --package $runtime --output $installerOutput `
    --version $version --icon (Join-Path $buildRoot 'src\gui\privateCloud.ico')
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }
$installer = Join-Path $installerOutput ('privateCloud-' + $version + '-Machine-Setup.exe')
$verified = Join-Path $outputRoot 'verified-payload'
$process = Start-Process -FilePath $installer -ArgumentList ('--verify-package "' + $verified + '"') -WindowStyle Hidden -PassThru
if (!$process.WaitForExit(120000)) { $process.Kill(); throw 'Installer payload verification timed out.' }
$process.Refresh()
if ($process.ExitCode -ne 0) { throw 'Installer payload verification failed.' }
$release = Join-Path $outputRoot 'release'
New-Item -ItemType Directory -Path $release | Out-Null
$filename = [IO.Path]::GetFileName($installer)
Copy-Item -LiteralPath $installer -Destination $release
$sha = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath (Join-Path $release ($filename + '.sha256')) -Value ($sha + '  ' + $filename) -Encoding ascii
Copy-Item -LiteralPath (Join-Path $packaging 'INSTALL.md') -Destination (Join-Path $release 'INSTALL.md')
Write-Output ('Verified installer: ' + $filename + '; SHA256 ' + $sha)
