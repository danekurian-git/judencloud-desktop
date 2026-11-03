# SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Machine package construction tests; compilers and deployment tools are mocked."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET
import zipfile


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


BUILDER = load_module('privatecloud_machine_builder', Path(__file__).with_name('build-machine-installer.py'))
GUARD = BUILDER.GUARD


def requirements(required=True):
    return {'schema': 1, 'directConnectionHelperRequired': required,
            'service': 'privateCloudDirectBroker', 'installScope': 'machine'}


def make_package(root):
    for name in (*GUARD.REQUIRED_RUNTIME, 'privateCloudDirectBroker.exe'):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'MZ isolated runtime fixture ' + name.encode())
    (root / GUARD.RUNTIME_REQUIREMENTS).write_text(json.dumps(requirements()), encoding='utf-8')


def snapshot(root):
    return {p.relative_to(root).as_posix(): (p.read_bytes(), p.stat().st_mtime_ns)
            for p in root.rglob('*') if p.is_file()}


def fake_compile(command, **kwargs):
    if kwargs != {'check': True}:
        raise AssertionError('Compiler failure must propagate')
    output = Path(next(arg[5:] for arg in command if arg.startswith('/out:')))
    output.write_bytes(b'MZ isolated compiler fixture ' + output.name.encode())


class MachineBuildTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='privatecloud-machine-build-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.package = self.root / 'payload'
        self.output = self.root / 'output'
        make_package(self.package)

    def invoke(self, compiler=fake_compile):
        arguments = ['build-machine-installer.py', '--package', str(self.package),
                     '--output', str(self.output), '--version', '2026.09.23-test']
        with mock.patch('sys.argv', arguments), \
                mock.patch.object(BUILDER.subprocess, 'run', side_effect=compiler) as run, \
                contextlib.redirect_stdout(io.StringIO()):
            BUILDER.main()
        return run

    def assert_rejected_before_compiler(self, exception=ValueError):
        with mock.patch.object(BUILDER.subprocess, 'run') as compiler:
            with self.assertRaises(exception):
                self.invoke(compiler)
            compiler.assert_not_called()
        self.assertFalse(self.output.exists())

    def test_hashes_and_archive_use_exact_verified_bytes_and_generated_maintenance(self):
        before = snapshot(self.package)
        run = self.invoke()
        self.assertEqual(run.call_count, 2)
        self.assertEqual(snapshot(self.package), before)
        expected = {name: value[0] for name, value in before.items()}
        expected['privateCloudMaintenance.exe'] = (self.output / 'privateCloudMaintenance.exe').read_bytes()
        with zipfile.ZipFile(self.output / 'payload.zip') as archive:
            self.assertEqual(set(archive.namelist()), set(expected))
            self.assertEqual(len(archive.namelist()), len(expected))
            self.assertEqual({name: archive.read(name) for name in archive.namelist()}, expected)
        hashes = dict(line.split('\t', 1)[::-1] for line in
                      (self.output / 'package.sha256').read_text(encoding='utf-8').splitlines())
        self.assertEqual(hashes, {name: hashlib.sha256(data).hexdigest() for name, data in expected.items()})
        self.assertEqual((self.output / 'version.txt').read_text(encoding='utf-8'), '2026.09.23-test')
        metadata = (self.output / 'AssemblyInfo.cs').read_text(encoding='utf-8')
        self.assertIn('AssemblyProduct("privateCloud")', metadata)
        self.assertIn('AssemblyInformationalVersion("2026.09.23-test")', metadata)
        self.assertNotIn('AssemblyInfo.cs', expected)
        first, second = [call.args[0] for call in run.call_args_list]
        self.assertFalse(any(arg.startswith('/resource:') for arg in first))
        self.assertEqual({arg.split(',')[-1] for arg in second if arg.startswith('/resource:')},
                         {'payload.zip', 'package.sha256', 'version.txt'})
        for command in (first, second):
            self.assertIn('/platform:x64', command)
            self.assertIn('/main:PrivateCloud.Setup.MachineSetupProgram', command)
            manifest = Path(next(arg.split(':', 1)[1] for arg in command if arg.startswith('/win32manifest:')))
            levels = ET.parse(manifest).getroot().findall('.//{urn:schemas-microsoft-com:asm.v3}requestedExecutionLevel')
            self.assertEqual([level.attrib['level'] for level in levels], ['asInvoker'])

    def test_machine_manifest_must_be_explicit_and_supported_before_output(self):
        manifest = self.package / GUARD.RUNTIME_REQUIREMENTS
        for data in (None, b'{', json.dumps(requirements(False)).encode(),
                     json.dumps(dict(requirements(), schema=True)).encode()):
            with self.subTest(data=data):
                if data is None:
                    manifest.unlink()
                else:
                    manifest.write_bytes(data)
                self.assert_rejected_before_compiler()

    def test_missing_service_binary_is_rejected_before_output(self):
        (self.package / 'privateCloudDirectBroker.exe').unlink()
        self.assert_rejected_before_compiler(FileNotFoundError)

    def test_private_key_and_test_material_still_fail_admission(self):
        for name, data in [('private.pem', b'container'), ('tests/data.txt', b'fixture'),
                           ('data.bin', b'-----BEGIN PRIVATE KEY-----\nYWJjZA==\n-----END PRIVATE KEY-----\n')]:
            path = self.package / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            with self.subTest(name=name):
                self.assert_rejected_before_compiler()
            path.unlink()

    def test_existing_output_is_never_overwritten(self):
        self.output.mkdir()
        marker = self.output / 'existing.bin'
        marker.write_bytes(b'existing output')
        before = snapshot(self.output)
        with mock.patch.object(BUILDER.subprocess, 'run') as compiler:
            with self.assertRaises(FileExistsError):
                self.invoke(compiler)
            compiler.assert_not_called()
        self.assertEqual(snapshot(self.output), before)

    def test_archived_manifest_is_revalidated_after_maintenance_compilation(self):
        calls = []

        def compile_then_change(command, **kwargs):
            calls.append(command)
            fake_compile(command, **kwargs)
            (self.package / GUARD.RUNTIME_REQUIREMENTS).write_text(json.dumps(requirements(False)), encoding='utf-8')

        with self.assertRaisesRegex(ValueError, 'helper-enabled'):
            self.invoke(compile_then_change)
        self.assertEqual(len(calls), 1)
        self.assertFalse((self.output / 'privateCloud-2026.09.23-test-Machine-Setup.exe').exists())
        self.assertFalse((self.output / 'package.sha256').exists())

    def test_payload_cannot_replace_generated_maintenance(self):
        (self.package / 'privateCloudMaintenance.exe').write_bytes(b'MZ unexpected payload maintenance')
        with self.assertRaisesRegex(ValueError, 'must not supply'):
            self.invoke()
        self.assertFalse((self.output / 'payload.zip').exists())
        self.assertFalse((self.output / 'privateCloud-2026.09.23-test-Machine-Setup.exe').exists())


class MachineRuntimeStageTest(unittest.TestCase):
    def test_portable_app_and_descendants_are_preserved_before_staging(self):
        script = os.environ.get('PRIVATECLOUD_PACKAGE_SCRIPT_UNDER_TEST')
        if not script:
            self.skipTest('Set PRIVATECLOUD_PACKAGE_SCRIPT_UNDER_TEST to the external package-privatecloud.py')
        packager = load_module('privatecloud_machine_runtime_packager', Path(script))
        source = Path(__file__).resolve().parents[3]
        with tempfile.TemporaryDirectory(prefix='privatecloud-runtime-stage-test-') as temp:
            root = Path(temp)
            portable = root / 'portable-app'
            portable.mkdir()
            (portable / 'keep.bin').write_bytes(b'working application sentinel')
            before = snapshot(portable)
            build_bin = root / 'build' / 'bin'
            build_bin.mkdir(parents=True)
            (build_bin / GUARD.RUNTIME_REQUIREMENTS).write_text(json.dumps(requirements()), encoding='utf-8')
            broker = root / 'privateCloudDirectBroker.exe'
            broker.write_bytes(b'MZ isolated broker fixture')
            with mock.patch.object(packager, 'PORTABLE_APP', portable), \
                    mock.patch.object(packager.subprocess, 'run') as subprocess_run, \
                    mock.patch.object(packager.shutil, 'copy2') as copy:
                for destination in (portable, portable / 'new-stage', portable / 'nested' / 'new-stage'):
                    with self.subTest(destination=destination), self.assertRaises(ValueError):
                        packager.main(source=source, build_bin=build_bin, deps=root / 'deps',
                                      package=destination, machine=True, broker=broker)
                subprocess_run.assert_not_called()
                copy.assert_not_called()
            self.assertEqual(snapshot(portable), before)
            self.assertEqual(list(portable.iterdir()), [portable / 'keep.bin'])


if __name__ == '__main__':
    unittest.main()
