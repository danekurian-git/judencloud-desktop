# SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Isolated checks for source snapshot admission; never snapshots the real tree."""
import importlib.util
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest


SCRIPT = Path(__file__).with_name("snapshot-source.py")
SPEC = importlib.util.spec_from_file_location("snapshot_source", SCRIPT)
SNAPSHOT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SNAPSHOT)


class SnapshotSourceTest(unittest.TestCase):
    def test_build_cache_and_credentials_are_excluded(self):
        for name in ("build/output.dll", ".env", "config/tokens.json", "config/local.key", "__pycache__/module.pyc"):
            with self.subTest(name=name):
                self.assertIsNotNone(SNAPSHOT.exclusion(name, b"local data"))
        self.assertIsNone(SNAPSHOT.exclusion("admin/win/privatecloud/build-installer.py", b"print(1)"))
        self.assertIsNone(SNAPSHOT.exclusion(".env.example", b"HOST=example.test"))

    def test_disposable_key_requires_exact_path_and_fingerprint(self):
        key_name = "test/privatecloudtransportkey.pem"
        key = (SCRIPT.parents[3] / key_name).read_bytes()
        self.assertIsNone(SNAPSHOT.exclusion(key_name, key))
        self.assertIsNotNone(SNAPSHOT.exclusion("unexpected.pem", key))
        self.assertIsNotNone(SNAPSHOT.exclusion(key_name, key + b"\n"))

    def test_support_build_flags_are_preserved_and_checksummed(self):
        with tempfile.TemporaryDirectory(prefix="privatecloud-snapshot-test-") as temporary:
            cache = Path(temporary) / "CMakeCache.txt"
            cache.write_text("TOKEN_AUTH_ONLY:BOOL=OFF\nENCRYPTION_HARDWARE_TOKEN_DRIVER_PATH:PATH=\nCMAKE_BUILD_TYPE:STRING=RelWithDebInfo\n")
            args = SimpleNamespace(tooling_dir=None, cmake_cache=cache, handoff=[], audit=[])
            files = SNAPSHOT.collect_support(args)
            entry = files["build-config/CMakeCache.txt"]
            self.assertEqual(entry["sha256"], SNAPSHOT.digest(cache.read_bytes()))
            self.assertEqual(entry["bytes"], len(cache.read_bytes()))

    def test_cmake_secret_is_rejected_without_echoing_value(self):
        with tempfile.TemporaryDirectory(prefix="privatecloud-snapshot-test-") as temporary:
            cache = Path(temporary) / "CMakeCache.txt"
            cache.write_text("API_TOKEN:STRING=do-not-export\n")
            args = SimpleNamespace(tooling_dir=None, cmake_cache=cache, handoff=[], audit=[])
            with self.assertRaises(ValueError) as failure:
                SNAPSHOT.collect_support(args)
            self.assertIn("API_TOKEN", str(failure.exception))
            self.assertNotIn("do-not-export", str(failure.exception))


if __name__ == "__main__":
    unittest.main()
