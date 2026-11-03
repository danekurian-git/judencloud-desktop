# SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read-only package admission checks and isolated blocked-payload tests."""
import base64
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location('privatecloud_packager', Path(__file__).with_name('build-installer.py'))
PACKAGER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGER)


def key_block(kind='PRIVATE KEY'):
    # Synthetic bytes, not a usable key. Exercise complete PEM block detection.
    encoded = base64.b64encode(b'privateCloud package admission test fixture')
    return b'-----BEGIN ' + kind.encode() + b'-----\n' + encoded + b'\n-----END ' + kind.encode() + b'-----\n'


def runtime_requirements(required=False):
    return {'schema': 1, 'directConnectionHelperRequired': required,
            'service': 'privateCloudDirectBroker', 'installScope': 'machine'}


def make_runtime_package(package):
    for relative in PACKAGER.REQUIRED_RUNTIME:
        path = package / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'MZ runtime fixture')


class PayloadAdmissionTest(unittest.TestCase):
    def test_complete_openssl_runtime_is_required(self):
        tls_files = ('tls/qopensslbackend.dll', 'libssl-3-x64.dll', 'libcrypto-3-x64.dll')
        for missing in tls_files:
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as directory:
                package = Path(directory)
                make_runtime_package(package)
                for name in tls_files:
                    path = package / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(b'MZ TLS runtime fixture')
                (package / missing).unlink()
                with self.assertRaisesRegex(FileNotFoundError, 'Required runtime missing'):
                    PACKAGER.validate_package(package)

    def test_machine_package_requires_explicit_manifest_and_helper(self):
        with tempfile.TemporaryDirectory() as directory:
            package = Path(directory)
            make_runtime_package(package)
            with self.assertRaisesRegex(ValueError, 'Missing privateCloud runtime requirements'):
                PACKAGER.validate_package(package, machine=True)
            manifest = package / PACKAGER.RUNTIME_REQUIREMENTS
            manifest.write_text(json.dumps(runtime_requirements(True)), encoding='utf-8')
            with self.assertRaisesRegex(FileNotFoundError, 'privateCloudDirectBroker'):
                PACKAGER.validate_package(package, machine=True)
            (package / 'privateCloudDirectBroker.exe').write_bytes(b'MZ isolated broker fixture')
            self.assertTrue(PACKAGER.validate_package(package, machine=True))
            with self.assertRaisesRegex(ValueError, 'legacy per-user'):
                PACKAGER.validate_package(package)
            manifest.write_text(json.dumps(runtime_requirements(False)), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'helper-enabled'):
                PACKAGER.validate_package(package, machine=True)

    def test_machine_admission_preserves_key_and_test_rejections(self):
        for name, contents in [('data.bin', key_block()), ('tests/data.txt', b'data'), ('TestBroker.exe', b'MZ')]:
            with self.subTest(name=name), self.assertRaises(ValueError):
                PACKAGER.validate_payload(name, contents, machine=True)

    def test_qt_parser_markers_without_key_block_are_allowed(self):
        for data in (b'MZ\0Qt TLS parser: -----BEGIN PRIVATE KEY-----\0-----END PRIVATE KEY-----\0',
                     b'MZ\0-----BEGIN RSA PRIVATE KEY-----\n',
                     b'MZ\0-----BEGIN PRIVATE KEY-----\nnot base64 parser text\n-----END PRIVATE KEY-----\0'):
            with self.subTest(data=data):
                PACKAGER.validate_payload('Qt6Network.dll', data)

    def test_complete_private_key_blocks_are_rejected_under_any_extension(self):
        for kind in ('PRIVATE KEY', 'RSA PRIVATE KEY', 'EC PRIVATE KEY', 'DSA PRIVATE KEY',
                     'OPENSSH PRIVATE KEY', 'ENCRYPTED PRIVATE KEY'):
            with self.subTest(kind=kind), self.assertRaisesRegex(ValueError, 'complete private-key block'):
                PACKAGER.validate_payload('unnoticed-data.txt', key_block(kind))

    def test_traditional_encrypted_private_key_is_rejected(self):
        data = key_block('RSA PRIVATE KEY').replace(b'-----\n', b'-----\nProc-Type: 4,ENCRYPTED\nDEK-Info: AES-256-CBC,00\n\n', 1)
        with self.assertRaisesRegex(ValueError, 'complete private-key block'):
            PACKAGER.validate_payload('embedded-resource.bin', data)

    def test_private_key_with_crlf_and_base64_whitespace_is_rejected(self):
        lines = key_block().splitlines()
        data = lines[0] + b'\r\n' + b' \t'.join(lines[1][i:i + 8] for i in range(0, len(lines[1]), 8)) + b'\r\n' + lines[2]
        with self.assertRaisesRegex(ValueError, 'complete private-key block'):
            PACKAGER.validate_payload('embedded-resource.bin', data)

    def test_conservative_private_key_extensions_are_rejected(self):
        for suffix in ('.pem', '.PEM', '.key', '.pfx', '.p12', '.ppk'):
            with self.subTest(suffix=suffix), self.assertRaisesRegex(ValueError, 'certificate-container'):
                PACKAGER.validate_payload('certificate' + suffix, b'Even a public-only container is not required in this payload.')

    def test_test_executables_are_rejected(self):
        for name in ('test.exe', 'PrivateCloudDriveTest.exe', 'SyncTests.EXE', 'TestFixture.exe'):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'Test executable'):
                PACKAGER.validate_payload(name, b'MZ fixture')
        PACKAGER.validate_payload('privateCloud.exe', b'MZ runtime')

    def test_test_source_and_fixture_paths_are_rejected(self):
        for name in ('test/sample.txt', 'tests/certificate.txt', 'src/autotests/example.cpp',
                     'e2e/fixture.dat', 'qml/QtTest/qmldir', 'fixtures/data.bin'):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'Test source/data'):
                PACKAGER.validate_payload(name, b'fixture')

    def test_public_certificate_block_in_text_is_not_a_private_key(self):
        PACKAGER.validate_payload('certificate-description.txt', key_block('CERTIFICATE'))

    def test_legacy_package_accepts_absent_or_false_runtime_requirements(self):
        with tempfile.TemporaryDirectory(prefix='privatecloud-package-test-') as temp:
            package = Path(temp)
            make_runtime_package(package)
            self.assertEqual(len(PACKAGER.validate_package(package)), len(PACKAGER.REQUIRED_RUNTIME))
            manifest = package / 'privatecloud-runtime-requirements.json'
            manifest.write_text(json.dumps(runtime_requirements()), encoding='utf-8')
            self.assertIn(manifest.resolve(), PACKAGER.validate_package(package))

    def test_helper_dependent_package_stops_before_compiler_or_output_creation(self):
        with tempfile.TemporaryDirectory(prefix='privatecloud-package-test-') as temp:
            root = Path(temp)
            package = root / 'package'
            make_runtime_package(package)
            (package / 'privatecloud-runtime-requirements.json').write_text(
                json.dumps(runtime_requirements(True)), encoding='utf-8')
            output = root / 'output'
            with mock.patch('sys.argv', ['build-installer.py', '--package', str(package), '--output', str(output), '--version', '1']), \
                    mock.patch.object(PACKAGER.subprocess, 'run') as compiler:
                with self.assertRaisesRegex(ValueError, 'machine-wide installer'):
                    PACKAGER.main()
                compiler.assert_not_called()
            self.assertFalse(output.exists())

    def test_invalid_runtime_requirements_fail_closed(self):
        invalid = [b'{', b'null', b'[]', b'{}', b'\xff']
        for field, values in {'schema': [True, '1', 0, 2, None],
                              'directConnectionHelperRequired': [0, 1, 'false', None],
                              'service': [True, None, 'anotherService'],
                              'installScope': [True, None, 'user']}.items():
            for value in values:
                changed = runtime_requirements()
                changed[field] = value
                invalid.append(json.dumps(changed).encode())
            missing = runtime_requirements()
            del missing[field]
            invalid.append(json.dumps(missing).encode())
        with tempfile.TemporaryDirectory(prefix='privatecloud-package-test-') as temp:
            package = Path(temp)
            make_runtime_package(package)
            manifest = package / 'privatecloud-runtime-requirements.json'
            for data in invalid:
                with self.subTest(data=data):
                    manifest.write_bytes(data)
                    with self.assertRaisesRegex(ValueError, 'runtime requirements'):
                        PACKAGER.validate_package(package)

    def test_manifest_is_validated_again_when_payload_is_archived(self):
        with self.assertRaisesRegex(ValueError, 'machine-wide installer'):
            PACKAGER.validate_payload('privatecloud-runtime-requirements.json',
                                      json.dumps(runtime_requirements(True)).encode())

    def test_current_build_requires_an_explicit_supported_manifest(self):
        with tempfile.TemporaryDirectory(prefix='privatecloud-package-test-') as temp:
            build_bin = Path(temp)
            with self.assertRaisesRegex(ValueError, 'Missing privateCloud runtime requirements'):
                PACKAGER.validate_build_runtime_requirements(build_bin)
            manifest = build_bin / 'privatecloud-runtime-requirements.json'
            manifest.write_text(json.dumps(runtime_requirements()), encoding='utf-8')
            self.assertEqual(PACKAGER.validate_build_runtime_requirements(build_bin), manifest)

    def test_rejected_package_stops_before_compiler_or_output_creation(self):
        with tempfile.TemporaryDirectory(prefix='privatecloud-package-test-') as temp:
            root = Path(temp).resolve()
            package = root / 'package'
            for relative in PACKAGER.REQUIRED_RUNTIME:
                path = package / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'MZ runtime fixture')
            self.assertEqual(len(PACKAGER.validate_package(package)), len(PACKAGER.REQUIRED_RUNTIME))
            (package / 'test-key.pem').write_bytes(key_block())
            output = root / 'output'
            with mock.patch('sys.argv', ['build-installer.py', '--package', str(package), '--output', str(output), '--version', '1']), \
                    mock.patch.object(PACKAGER.subprocess, 'run') as compiler:
                with self.assertRaises(ValueError):
                    PACKAGER.main()
                compiler.assert_not_called()
            self.assertFalse(output.exists())

    def test_current_application_package_passes_read_only_preflight(self):
        configured = os.environ.get('PRIVATECLOUD_PACKAGE_UNDER_TEST')
        if not configured:
            self.skipTest('Set PRIVATECLOUD_PACKAGE_UNDER_TEST for the read-only runtime package audit.')
        files = PACKAGER.validate_package(Path(configured), machine=True)
        self.assertGreater(len(files), len(PACKAGER.REQUIRED_RUNTIME))
        self.assertTrue(any(path.name == 'privateCloud.exe' for path in files))


class SourcePackagingAdmissionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        configured = os.environ.get('PRIVATECLOUD_PACKAGE_SCRIPT_UNDER_TEST')
        if not configured:
            raise unittest.SkipTest('Set PRIVATECLOUD_PACKAGE_SCRIPT_UNDER_TEST to test the external packaging entry point.')
        spec = importlib.util.spec_from_file_location('privatecloud_source_packager', configured)
        cls.packager = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.packager)

    def test_blocked_build_does_not_create_or_change_app(self):
        payloads = (None, b'{', json.dumps(runtime_requirements(True)).encode(),
                    json.dumps(dict(runtime_requirements(), directConnectionHelperRequired='false')).encode())
        for existing_app in (False, True):
            for payload in payloads:
                with self.subTest(existing_app=existing_app, payload=payload), \
                        tempfile.TemporaryDirectory(prefix='privatecloud-package-test-') as temp:
                    root = Path(temp)
                    build_bin = root / 'build/bin'
                    build_bin.mkdir(parents=True)
                    if payload is not None:
                        (build_bin / 'privatecloud-runtime-requirements.json').write_bytes(payload)
                    package = root / 'App'
                    if existing_app:
                        package.mkdir()
                        (package / 'privateCloud.exe').write_bytes(b'working old app')
                        (package / 'settings.txt').write_bytes(b'preserve this fixture')
                    before = {p.relative_to(package): (p.read_bytes(), p.stat().st_mtime_ns)
                              for p in package.rglob('*') if p.is_file()}
                    with mock.patch.object(self.packager.subprocess, 'run') as process, \
                            mock.patch.object(self.packager.shutil, 'copy2') as copy, \
                            mock.patch.object(self.packager.os, 'replace') as replace:
                        with self.assertRaises(ValueError):
                            self.packager.main(source=Path(__file__).resolve().parents[3],
                                               build_bin=build_bin, deps=root / 'deps', package=package)
                        process.assert_not_called()
                        copy.assert_not_called()
                        replace.assert_not_called()
                    self.assertEqual(package.exists(), existing_app)
                    after = {p.relative_to(package): (p.read_bytes(), p.stat().st_mtime_ns)
                             for p in package.rglob('*') if p.is_file()}
                    self.assertEqual(after, before)


if __name__ == '__main__':
    unittest.main()
