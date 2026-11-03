# SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the protected machine installer; the legacy per-user guard stays enabled."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import re
import subprocess
import zipfile

SOURCE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location('privatecloud_payload_guard', SOURCE / 'build-installer.py')
GUARD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GUARD)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--icon', type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9][A-Za-z0-9._-]{0,79}', args.version):
        raise ValueError('Invalid version identifier')
    package = args.package.resolve(strict=True)
    files = GUARD.validate_package(package, machine=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    compiler = Path(r'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe')
    names = ('InstallState.cs', 'Package.cs', 'WindowsIntegration.cs', 'MachineInstallPaths.cs',
             'MachineInstallState.cs', 'MachineInstallerCore.cs', 'MachineWindowsService.cs',
             'MachineWindowsIntegration.cs', 'SetupLog.cs', 'SetupPresentation.cs', 'SetupProgressWindow.cs', 'MachineSetupProgram.cs')
    flags = [str(compiler), '/nologo', '/optimize+', '/target:winexe', '/platform:x64',
             '/main:PrivateCloud.Setup.MachineSetupProgram',
             '/reference:System.Windows.Forms.dll', '/reference:System.Drawing.dll',
             '/reference:' + str(compiler.parent / 'System.IO.Compression.dll'),
             '/win32manifest:' + str(SOURCE / 'privatecloud.manifest')]
    if args.icon:
        flags.append('/win32icon:' + str(args.icon.resolve(strict=True)))
    numeric_version = args.version if re.fullmatch(r'\d+\.\d+\.\d+\.\d+', args.version) else '0.0.0.0'
    metadata = output / 'AssemblyInfo.cs'
    metadata.write_text('using System.Reflection;\n'
                        '[assembly: AssemblyTitle("privateCloud Setup")]\n'
                        '[assembly: AssemblyProduct("privateCloud")]\n'
                        '[assembly: AssemblyDescription("privateCloud Windows installer")]\n'
                        '[assembly: AssemblyVersion("' + numeric_version + '")]\n'
                        '[assembly: AssemblyFileVersion("' + numeric_version + '")]\n'
                        '[assembly: AssemblyInformationalVersion("' + args.version + '")]\n', encoding='utf-8')
    common = [str(SOURCE / name) for name in names] + [str(metadata)]
    maintenance = output / 'privateCloudMaintenance.exe'
    subprocess.run(flags + ['/out:' + str(maintenance)] + common, check=True)
    items = [(p.relative_to(package).as_posix(), p) for p in files]
    if any(name.casefold() == maintenance.name.casefold() for name, _ in items):
        raise ValueError('Input payload must not supply the generated maintenance executable')
    items.append((maintenance.name, maintenance))
    manifest = []
    with zipfile.ZipFile(output / 'payload.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name, path in items:
            if path.is_symlink() or (path != maintenance and not path.resolve().is_relative_to(package)):
                raise ValueError('A payload file redirects outside the package')
            data = path.read_bytes()
            GUARD.validate_payload(name, data, machine=True)
            manifest.append(hashlib.sha256(data).hexdigest() + '\t' + name)
            archive.writestr(name, data)
    (output / 'package.sha256').write_text('\n'.join(manifest) + '\n', encoding='utf-8')
    (output / 'version.txt').write_text(args.version, encoding='utf-8')
    resources = ['/resource:' + str(output / name) + ',' + name for name in ('payload.zip', 'package.sha256', 'version.txt')]
    installer = output / ('privateCloud-' + args.version + '-Machine-Setup.exe')
    subprocess.run(flags + ['/out:' + str(installer)] + resources + common, check=True)
    print(str(installer))
    print('Unsigned machine installer; ' + str(len(items)) + ' files; SHA256 ' + hashlib.sha256(installer.read_bytes()).hexdigest())


if __name__ == '__main__':
    main()
