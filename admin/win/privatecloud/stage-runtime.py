# SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stage the current Windows build without modifying installed applications."""
import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess

SOURCE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location('privatecloud_stage_guard', SOURCE / 'build-installer.py')
GUARD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GUARD)
PORTABLE_APP = Path(os.environ.get('LOCALAPPDATA', str(Path.home()))) / 'privateCloud' / 'App'


def imported_libraries(path):
    # Build-time dependency only; pefile and Python are never shipped.
    import pefile
    with pefile.PE(str(path), fast_load=True) as binary:
        if binary.FILE_HEADER.Machine != 0x8664:
            raise ValueError('Expected an x64 runtime binary: ' + str(path))
        binary.parse_data_directories(directories=[
            pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT'],
            pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT'],
        ])
        return {item.dll.decode('ascii') for table in
                ('DIRECTORY_ENTRY_IMPORT', 'DIRECTORY_ENTRY_DELAY_IMPORT')
                for item in getattr(binary, table, ())}


def copy_file(source, destination):
    if source.is_symlink() or not source.is_file():
        raise ValueError('Expected a regular runtime file: ' + str(source))
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def collect_dependencies(package, search_directories, system_directory):
    """Resolve normal and delay imports, never copying DLLs from System32."""
    pending = list(package.rglob('*.dll')) + list(package.rglob('*.exe'))
    processed = set()
    while pending:
        binary = pending.pop()
        if binary in processed:
            continue
        processed.add(binary)
        for name in sorted(imported_libraries(binary)):
            if Path(name).name != name or '/' in name or '\\' in name:
                raise ValueError('Invalid imported library name: ' + name)
            if name.lower().startswith(('api-ms-win-', 'ext-ms-win-')):
                continue
            available = next((root / name for root in (binary.parent, package)
                              if (root / name).is_file()), None)
            if available:
                pending.append(available)
                continue
            dependency = next((root / name for root in search_directories
                               if (root / name).is_file()), None)
            if dependency:
                target = package / name
                copy_file(dependency, target)
                pending.append(target)
            elif not (system_directory / name).is_file():
                raise FileNotFoundError(f'Unresolved runtime dependency {name} required by {binary.name}')


def main(*, source, build_bin, deps, package, machine=False, broker=None, redist=None):
    source, build_bin, deps, package = (Path(p).resolve() for p in (source, build_bin, deps, package))
    manifest = GUARD.validate_build_runtime_requirements(build_bin, machine=machine)
    if not machine:
        raise ValueError('This staging entry point requires the machine-wide installer.')
    protected = [source, build_bin, deps, PORTABLE_APP.resolve()]
    if os.environ.get('ProgramFiles'):
        protected.append(Path(os.environ['ProgramFiles']).resolve())
    if any(package == root or package.is_relative_to(root) or root.is_relative_to(package)
           for root in protected):
        raise ValueError('Stage outside source, toolchain and installed application directories.')
    if package.exists():
        raise ValueError('Runtime staging requires a new output directory.')
    if broker is None or not Path(broker).is_file():
        raise ValueError('Supply the compiled privateCloudDirectBroker.exe.')
    if redist is None or not Path(redist).is_dir():
        raise ValueError('Supply the Visual Studio x64 redistributable directory.')
    variants = [names for names in GUARD.CORE_RUNTIME_VARIANTS
                if all((build_bin / name).is_file() for name in names)]
    if len(variants) != 1:
        raise ValueError('Expected one complete client DLL family in the current build.')
    executable = 'nextclouddev.exe' if variants[0] == GUARD.CORE_RUNTIME_VARIANTS[0] else 'nextcloud.exe'
    files = [(build_bin / executable, 'privateCloud.exe'),
             (Path(broker).resolve(), 'privateCloudDirectBroker.exe'),
             (manifest, manifest.name),
             (build_bin / 'CfApiShellExtensions.dll', 'CfApiShellExtensions.dll'),
             (source / 'sync-exclude.lst', 'sync-exclude.lst')]
    files.extend((build_bin / name, name) for name in variants[0])
    # OpenSSL is loaded by Qt at runtime rather than through the app's PE imports.
    files.extend((deps / 'bin' / name, name) for name in ('libssl-3-x64.dll', 'libcrypto-3-x64.dll'))
    deploy = deps / 'bin' / 'windeployqt.exe'
    for path in [deploy, *(path for path, _ in files)]:
        if not path.is_file() or path.is_symlink():
            raise FileNotFoundError('Required staging input missing: ' + str(path))
    package.mkdir(parents=True, exist_ok=False)
    for path, relative in files:
        copy_file(path, package / relative)
    subprocess.run([str(deploy), '--release', '--no-compiler-runtime', '--no-translations',
                    '--openssl-root', str(deps / 'bin'),
                    '--qmldir', str(source / 'src' / 'gui'), '--dir', str(package),
                    str(package / 'privateCloud.exe')], check=True)
    redist = Path(redist).resolve()
    search = [redist, *sorted(path for path in redist.iterdir() if path.is_dir()), deps / 'bin']
    collect_dependencies(package, search, Path(os.environ['SystemRoot']) / 'System32')
    (package / 'qt.conf').write_text('[Paths]\nPrefix=.\nPlugins=.\nQmlImports=qml\n', encoding='ascii')
    for path in sorted((source / 'LICENSES').glob('*.txt')):
        copy_file(path, package / 'licenses' / path.name)
    for name in ('COPYING', 'COPYING.documentation'):
        if (source / name).is_file():
            copy_file(source / name, package / 'licenses' / name)
    GUARD.validate_package(package, machine=True)
    # A runtime must not accidentally contain Qt command-line tools or test programs.
    allowed = {'privatecloud.exe', 'privateclouddirectbroker.exe'}
    unexpected = [p.name for p in package.rglob('*.exe') if p.name.casefold() not in allowed]
    if unexpected:
        raise ValueError('Unexpected runtime executables: ' + ', '.join(unexpected))
    print('Staged and checked privateCloud runtime: ' + str(package))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('source', 'build-bin', 'deps', 'package', 'broker', 'redist'):
        parser.add_argument('--' + option, type=Path, required=True)
    main(**vars(parser.parse_args()), machine=True)
