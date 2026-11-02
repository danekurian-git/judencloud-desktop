# SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the single-file Windows installer using the .NET compiler included in Windows."""
import argparse
import base64
import binascii
import hashlib
import json
from pathlib import Path
import re
import subprocess
import zipfile


PRIVATE_KEY_EXTENSIONS = frozenset({'.key', '.pfx', '.p12', '.pem', '.ppk'})
TEST_DIRECTORIES = frozenset({'test', 'tests', 'autotests', 'unittests', 'testdata',
                              'test_data', 'fixtures', 'mocks', 'qttest', 'e2e'})
PRIVATE_KEY_BLOCK = re.compile(
    rb'-----BEGIN (?P<kind>(?:[A-Z0-9][A-Z0-9 -]* )?PRIVATE KEY)-----[ \t]*\r?\n'
    rb'(?P<body>.*?)\r?\n-----END (?P=kind)-----', re.DOTALL)
REQUIRED_RUNTIME = ('privateCloud.exe', 'nextclouddevsync.dll', 'nextclouddev_csync.dll',
                    'nextclouddevsync_vfs_cfapi.dll', 'CfApiShellExtensions.dll',
                    'platforms/qwindows.dll', 'vcruntime140.dll', 'msvcp140.dll',
                    'tls/qopensslbackend.dll', 'libssl-3-x64.dll', 'libcrypto-3-x64.dll')
RUNTIME_REQUIREMENTS = 'privatecloud-runtime-requirements.json'


def validate_runtime_requirements(data: bytes, *, machine: bool = False) -> None:
    """Reject builds that the legacy per-user installer cannot provision."""
    try:
        requirements = json.loads(data.decode('utf-8'))
    except (UnicodeDecodeError, ValueError) as error:
        raise ValueError('Invalid privateCloud runtime requirements: ' + str(error)) from error
    if (not isinstance(requirements, dict) or
            type(requirements.get('schema')) is not int or requirements['schema'] != 1 or
            type(requirements.get('directConnectionHelperRequired')) is not bool or
            requirements.get('service') != 'privateCloudDirectBroker' or
            requirements.get('installScope') != 'machine'):
        raise ValueError('Invalid privateCloud runtime requirements: expected schema 1, a boolean '
                         'directConnectionHelperRequired, service privateCloudDirectBroker and installScope machine.')
    if machine and not requirements['directConnectionHelperRequired']:
        raise ValueError('The machine installer requires a helper-enabled privateCloud build.')
    if requirements['directConnectionHelperRequired'] and not machine:
        raise ValueError('This build requires the privateCloudDirectBroker Windows service. '
                         'The legacy per-user installer and portable App packaging do not install that service. '
                         'Keep the working App unchanged and use the machine-wide installer. '
                         'Do not remove or edit the runtime requirements manifest to bypass this check.')


def validate_build_runtime_requirements(build_bin: Path, *, machine: bool = False) -> Path:
    """Require the current build's declaration before touching an existing App."""
    manifest = build_bin / RUNTIME_REQUIREMENTS
    if not manifest.is_file():
        raise ValueError('Missing privateCloud runtime requirements in the current build: ' + str(manifest) +
                         '. Reconfigure the source build to generate this manifest before packaging. '
                         'Keep the working App unchanged.')
    validate_runtime_requirements(manifest.read_bytes(), machine=machine)
    return manifest


def validate_payload(relative: str, data: bytes, *, machine: bool = False) -> None:
    """Reject test material and private keys without matching DLL parser labels."""
    path = Path(relative)
    if relative.casefold() == RUNTIME_REQUIREMENTS.casefold():
        validate_runtime_requirements(data, machine=machine)
    if '\t' in relative or '\n' in relative or '\r' in relative:
        raise ValueError('Invalid package path: ' + relative)
    if path.suffix.casefold() in PRIVATE_KEY_EXTENSIONS:
        raise ValueError('Private-key or certificate-container file is not a runtime dependency: ' + relative)
    if any(part.casefold() in TEST_DIRECTORIES for part in path.parts[:-1]):
        raise ValueError('Test source/data must not be shipped: ' + relative)
    if path.suffix.casefold() == '.exe' and 'test' in path.stem.casefold():
        raise ValueError('Test executable must not be shipped: ' + relative)
    for match in PRIVATE_KEY_BLOCK.finditer(data):
        lines = match['body'].strip().splitlines()
        # Traditional encrypted PEM keys can include Proc-Type/DEK-Info headers.
        while lines and b':' in lines[0]:
            lines.pop(0)
        encoded = re.sub(rb'[ \t\r\n\f\v]', b'', b''.join(lines))
        try:
            decoded = base64.b64decode(encoded, validate=True)
        except (ValueError, binascii.Error):
            continue
        if decoded:
            raise ValueError('A complete private-key block must not be shipped: ' + relative)


def validate_package(package: Path, *, machine: bool = False) -> list[Path]:
    """Read-only preflight; run before creating output or invoking the compiler."""
    package = package.resolve(strict=True)
    if machine:
        validate_build_runtime_requirements(package, machine=True)
        if not (package / 'privateCloudDirectBroker.exe').is_file():
            raise FileNotFoundError('The machine package is missing privateCloudDirectBroker.exe.')
    for name in REQUIRED_RUNTIME:
        if not (package / name).is_file():
            raise FileNotFoundError('Required runtime missing: ' + name)
    files = sorted(p for p in package.rglob('*') if p.is_file() and
                   not re.search(r'\.previous-|\.incoming-|\.pending-|\.pdb$|\.ilk$', p.name, re.I) and
                   p.name != 'migration-package.json')
    for path in files:
        if path.is_symlink() or not path.resolve().is_relative_to(package):
            raise ValueError('Do not package redirected files: ' + str(path))
        validate_payload(path.relative_to(package).as_posix(), path.read_bytes(), machine=machine)
    return files


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--icon', type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9][A-Za-z0-9._-]{0,79}', args.version):
        raise ValueError('Invalid version identifier')
    package = args.package.resolve()
    files = validate_package(package)
    source = Path(__file__).resolve().parent
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler = Path(r'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe')
    framework = compiler.parent
    common = [str(source / name) for name in ('InstallState.cs', 'InstallLock.cs', 'Package.cs', 'InstallerCore.cs', 'WindowsIntegration.cs')]
    flags = [str(compiler), '/nologo', '/optimize+', '/target:winexe', '/platform:x64',
             '/reference:System.Windows.Forms.dll', '/reference:System.Drawing.dll',
             '/reference:' + str(framework / 'System.IO.Compression.dll'),
             '/win32manifest:' + str(source / 'privatecloud.manifest')]
    if args.icon:
        flags.append('/win32icon:' + str(args.icon.resolve(strict=True)))
    launcher = output / 'launcher.exe'
    subprocess.run(flags + ['/out:' + str(launcher)] + common + [str(source / 'Launcher.cs')], check=True)
    manifest = []
    with zipfile.ZipFile(output / 'payload.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for path in files:
            if path.is_symlink():
                raise ValueError('Do not package redirected files: ' + str(path))
            relative = path.relative_to(package).as_posix()
            data = path.read_bytes()
            # Check the exact bytes that are hashed and archived, even if a file
            # changed since preflight. Parser marker strings alone are allowed.
            validate_payload(relative, data)
            manifest.append(hashlib.sha256(data).hexdigest() + '\t' + relative)
            archive.writestr(relative, data)
    (output / 'package.sha256').write_text('\n'.join(manifest) + '\n', encoding='utf-8')
    (output / 'version.txt').write_text(args.version, encoding='utf-8')
    installer = output / ('privateCloud-' + args.version + '-Setup.exe')
    resources = ['/resource:' + str(output / name) + ',' + name for name in ('payload.zip', 'package.sha256', 'version.txt')]
    resources.append('/resource:' + str(launcher) + ',launcher.exe')
    subprocess.run(flags + ['/out:' + str(installer)] + resources + common + [str(source / 'SetupProgram.cs')], check=True)
    print(str(installer))
    print('Unsigned x64 installer; ' + str(len(files)) + ' verified payload files; SHA256 ' + hashlib.sha256(installer.read_bytes()).hexdigest())


if __name__ == '__main__':
    main()
