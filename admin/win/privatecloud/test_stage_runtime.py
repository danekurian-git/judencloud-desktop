# SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Runtime staging dependency closure and package admission regression tests."""
import importlib.util
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


if __name__ == '__main__':
    unittest.main()
