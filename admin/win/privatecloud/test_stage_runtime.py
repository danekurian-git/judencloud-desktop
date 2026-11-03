# SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Runtime staging dependency closure and package admission regression tests."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location('privatecloud_stage', Path(__file__).with_name('stage-runtime.py'))
STAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(STAGE)


class RuntimeStageTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='privatecloud-stage-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.package, self.deps, self.system = [self.root / p for p in ('package', 'deps', 'system')]
        for path in (self.package, self.deps, self.system):
            path.mkdir()

    def test_transitive_and_plugin_dependencies_are_copied_without_system_files(self):
        (self.package / 'privateCloud.exe').write_bytes(b'app')
        (self.package / 'tls').mkdir()
        (self.package / 'tls' / 'backend.dll').write_bytes(b'plugin')
        (self.deps / 'first.dll').write_bytes(b'first')
        (self.deps / 'second.dll').write_bytes(b'second')
        (self.system / 'kernel32.dll').write_bytes(b'system')
        imports = {'privateCloud.exe': {'first.dll', 'kernel32.dll'},
                   'first.dll': {'second.dll'}, 'second.dll': {'first.dll'},
                   'backend.dll': {'second.dll', 'api-ms-win-core-file-l1-1-0.dll'}}
        with mock.patch.object(STAGE, 'imported_libraries', side_effect=lambda path: imports[path.name]):
            STAGE.collect_dependencies(self.package, [self.deps], self.system)
        self.assertEqual((self.package / 'first.dll').read_bytes(), b'first')
        self.assertEqual((self.package / 'second.dll').read_bytes(), b'second')
        self.assertFalse((self.package / 'kernel32.dll').exists())
        self.assertEqual((self.deps / 'first.dll').read_bytes(), b'first')

    def test_missing_library_fails_instead_of_producing_broken_installer(self):
        (self.package / 'privateCloud.exe').write_bytes(b'app')
        with mock.patch.object(STAGE, 'imported_libraries', return_value={'missing.dll'}):
            with self.assertRaisesRegex(FileNotFoundError, 'missing.dll.*privateCloud.exe'):
                STAGE.collect_dependencies(self.package, [self.deps], self.system)

    def test_import_cannot_escape_package_directory(self):
        (self.package / 'privateCloud.exe').write_bytes(b'app')
        for name in ('../outside.dll', r'..\outside.dll', 'C:/outside.dll'):
            with self.subTest(name=name), mock.patch.object(STAGE, 'imported_libraries', return_value={name}):
                with self.assertRaisesRegex(ValueError, 'Invalid imported library'):
                    STAGE.collect_dependencies(self.package, [self.deps], self.system)

    def make_runtime(self, family):
        names = set(STAGE.GUARD.REQUIRED_RUNTIME) - set(STAGE.GUARD.CORE_RUNTIME_VARIANTS[0])
        names.update(family)
        for name in names:
            path = self.package / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'fixture')

    def test_release_build_family_is_admitted(self):
        self.make_runtime(STAGE.GUARD.CORE_RUNTIME_VARIANTS[1])
        self.assertEqual(len(STAGE.GUARD.validate_package(self.package)), len(STAGE.GUARD.REQUIRED_RUNTIME))

    def test_mixed_build_families_are_rejected(self):
        self.make_runtime(STAGE.GUARD.CORE_RUNTIME_VARIANTS[1])
        (self.package / STAGE.GUARD.CORE_RUNTIME_VARIANTS[0][0]).write_bytes(b'stale dev DLL')
        with self.assertRaisesRegex(ValueError, 'mix development and release'):
            STAGE.GUARD.validate_package(self.package)

    def test_incomplete_build_family_is_rejected(self):
        self.make_runtime(STAGE.GUARD.CORE_RUNTIME_VARIANTS[1])
        (self.package / STAGE.GUARD.CORE_RUNTIME_VARIANTS[1][2]).unlink()
        with self.assertRaisesRegex(FileNotFoundError, 'one complete client DLL family'):
            STAGE.GUARD.validate_package(self.package)

    def test_staging_deploys_current_build_with_explicit_openssl_location(self):
        source = self.root / 'source'
        build_bin = source / 'build' / 'bin'
        build_bin.mkdir(parents=True)
        (source / 'src' / 'gui').mkdir(parents=True)
        (source / 'LICENSES').mkdir()
        (source / 'sync-exclude.lst').write_text('exclude fixture\n')
        (source / 'unrelated-source.txt').write_text('Do not package the source tree')
        family = STAGE.GUARD.CORE_RUNTIME_VARIANTS[0]
        for name in ('nextclouddev.exe', 'CfApiShellExtensions.dll', *family):
            (build_bin / name).write_bytes(b'current build ' + name.encode())
        (build_bin / STAGE.GUARD.RUNTIME_REQUIREMENTS).write_text(json.dumps({
            'schema': 1, 'directConnectionHelperRequired': True,
            'service': 'privateCloudDirectBroker', 'installScope': 'machine'}))
        (self.deps / 'bin').mkdir()
        for name in ('windeployqt.exe', 'libssl-3-x64.dll', 'libcrypto-3-x64.dll'):
            (self.deps / 'bin' / name).write_bytes(b'deployment input')
        broker = self.root / 'broker.exe'
        broker.write_bytes(b'current broker')
        redist = self.root / 'redist'
        redist.mkdir()
        package = self.root / 'staged'

        def deploy(command, *, check):
            self.assertTrue(check)
            # windeployqt rejects these two options when supplied together.
            if '--force-openssl' in command and '--openssl-root' in command:
                raise STAGE.subprocess.CalledProcessError(1, command)
            self.assertEqual(command[command.index('--openssl-root') + 1], str(self.deps / 'bin'))
            for name in STAGE.GUARD.REQUIRED_RUNTIME:
                target = package / name
                if not target.exists():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(b'Qt deployment fixture')

        with mock.patch.object(STAGE.subprocess, 'run', side_effect=deploy), \
                mock.patch.object(STAGE, 'collect_dependencies'):
            STAGE.main(source=source, build_bin=build_bin, deps=self.deps, package=package,
                       machine=True, broker=broker, redist=redist)
        self.assertEqual((package / 'privateCloud.exe').read_bytes(), b'current build nextclouddev.exe')
        self.assertEqual((package / 'privateCloudDirectBroker.exe').read_bytes(), b'current broker')
        self.assertFalse((package / 'unrelated-source.txt').exists())
        self.assertFalse((package / 'windeployqt.exe').exists())


if __name__ == '__main__':
    unittest.main()
