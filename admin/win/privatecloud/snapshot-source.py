# SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare a recoverable source snapshot without committing or changing Git state.

The default is a read-only dry run. Pass --write and a new --output directory
only after source edits have finished. Build outputs and local credentials are
excluded; two public, disposable test keys are retained by exact fingerprint.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import zipfile


TEST_KEYS = {
    "test/e2etestsfakecertprivatekey.pem": "84e7fe1958544bf16dbf6d330c2bfc935004238d47f6c4df44e3dc647d428821",
    "test/privatecloudtransportkey.pem": "7ee74a60d46748d2958f25696e39f4102322f2a7357d9c1070b5efed05e14e23",
}
CACHE_DIRECTORIES = {".git", "__pycache__", ".pytest_cache", ".mypy_cache", "node_modules", "cmakefiles", ".vs", ".vscode", "build", "bin", "obj"}
OUTPUT_SUFFIXES = {".pyc", ".pyo", ".obj", ".o", ".pdb", ".ilk", ".exe", ".dll", ".lib", ".a", ".so", ".dylib"}
SECRET_NAMES = {".env", "credentials.json", "tokens.json", "nextcloud.cfg", "nextclouddev.cfg", "privatecloud.cfg", "id_rsa", "id_ed25519"}
PRIVATE_KEY = re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH |ENCRYPTED )?PRIVATE KEY-----\r?\n[A-Za-z0-9+/=\r\n]+-----END (?:RSA |EC |OPENSSH |ENCRYPTED )?PRIVATE KEY-----")
ACCESS_TOKEN = re.compile(rb"(?:gh[pousr]_[A-Za-z0-9]{36,}|github_pat_[A-Za-z0-9_]{70,}|AKIA[A-Z0-9]{16})")
TOOLING_FILES = ("package-privatecloud.py", "smoke-privatecloud.py", "test-privatecloud-folder-picker.py", "test-privatecloud-refinements.py", "tidy-policy.py")
SECRET_SETTING = re.compile(r"(?:^|_)(?:PASSWORD|PASSWD|TOKEN|SECRET|PRIVATE_KEY|ACCESS_KEY)(?:_|$)", re.IGNORECASE)


def git(source, *arguments):
    return subprocess.check_output(["git", "-C", str(source), *arguments])


def digest(data):
    return hashlib.sha256(data).hexdigest()


def exclusion(relative, data):
    path = Path(relative)
    if any(part.lower() in CACHE_DIRECTORIES for part in path.parts[:-1]) or path.suffix.lower() in OUTPUT_SUFFIXES:
        return "build or cache output"
    if path.name.lower() in SECRET_NAMES or path.suffix.lower() in {".pfx", ".p12", ".key"}:
        return "credential filename"
    if path.name.lower().startswith(".env.") and path.name.lower() not in {".env.example", ".env.sample", ".env.template"}:
        return "local environment settings"
    if PRIVATE_KEY.search(data) and TEST_KEYS.get(relative) != digest(data):
        return "private key outside the fingerprinted disposable test fixtures"
    if ACCESS_TOKEN.search(data):
        return "access-token signature"
    return None


def collect(source):
    listed = git(source, "ls-files", "--cached", "--others", "--exclude-standard", "-z").decode("utf-8").split("\0")
    index_modes = {}
    for entry in filter(None, git(source, "ls-files", "--stage", "-z").decode("utf-8").split("\0")):
        attributes, name = entry.split("\t", 1)
        index_modes[name] = attributes.split()[0]
    # Keep this explicit even if the repository's ignore exception is later lost.
    listed.append("admin/win/privatecloud/build-installer.py")
    files, excluded, missing = {}, [], []
    for relative in sorted(set(filter(None, listed))):
        path = source / relative
        if not path.exists() and not path.is_symlink():
            missing.append(relative)
            continue
        if path.is_symlink():
            data = os.readlink(path).encode("utf-8")
            mode = stat.S_IFLNK | 0o777
        else:
            if not path.resolve().is_relative_to(source):
                raise ValueError("Source path redirects outside the repository: " + relative)
            data = path.read_bytes()
            # Git can check symlinks out as text files on Windows. Preserve the
            # source mode so recovery on another platform can recreate the link.
            recorded_mode = index_modes.get(relative)
            mode = stat.S_IFLNK | 0o777 if recorded_mode == "120000" else stat.S_IFREG | (0o755 if recorded_mode == "100755" else 0o644)
        reason = exclusion(relative, data)
        if reason:
            excluded.append({"path": relative, "reason": reason})
        else:
            files[relative] = {"sha256": digest(data), "bytes": len(data), "mode": mode}
    return files, excluded, missing


def collect_support(args):
    requested = {}
    if args.tooling_dir:
        requested.update({"tooling/" + name: args.tooling_dir / name for name in TOOLING_FILES})
    if args.cmake_cache:
        requested["build-config/CMakeCache.txt"] = args.cmake_cache
    for path in args.handoff + args.audit:
        relative = ("handoff/" if path in args.handoff else "audit/") + path.name
        if relative in requested:
            raise ValueError("Duplicate support archive name: " + relative)
        requested[relative] = path
    files = {}
    for relative, original in sorted(requested.items()):
        path = original.resolve(strict=True)
        data = path.read_bytes()
        reason = exclusion(relative, data)
        if reason:
            raise ValueError("Support file excluded (" + reason + "): " + str(path))
        if relative == "build-config/CMakeCache.txt":
            for line in data.decode("utf-8").splitlines():
                if line.startswith(("#", "//")) or "=" not in line or ":" not in line.partition("=")[0]:
                    continue
                declaration, _, value = line.partition("=")
                name, _, kind = declaration.partition(":")
                if kind == "BOOL" and value.upper() in {"ON", "OFF", "TRUE", "FALSE", "YES", "NO", "0", "1"}:
                    continue
                if value.strip() and SECRET_SETTING.search(name):
                    raise ValueError("CMake cache has a nonempty credential-like variable; review before export: " + name)
        files[relative] = {"source": str(path), "sha256": digest(data), "bytes": len(data)}
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--output", type=Path)
    parser.add_argument("--tooling-dir", type=Path, help="Include the five named external packaging/test helpers.")
    parser.add_argument("--cmake-cache", type=Path, help="Include build flags after checking for credential-like settings.")
    parser.add_argument("--handoff", type=Path, action="append", default=[])
    parser.add_argument("--audit", type=Path, action="append", default=[])
    parser.add_argument("--write", action="store_true", help="Create the snapshot after source work is finished.")
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    head = git(source, "rev-parse", "HEAD").decode().strip()
    status = git(source, "status", "--porcelain=v1", "--untracked-files=all").decode()
    files, excluded, missing = collect(source)
    support = collect_support(args)
    summary = {"source": str(source), "head": head, "files": len(files), "bytes": sum(item["bytes"] for item in files.values()),
               "excluded": excluded, "deletedTrackedFiles": missing, "supportFiles": len(support), "write": args.write}
    if not args.write:
        print(json.dumps(summary, indent=2))
        return
    if args.output is None:
        parser.error("--write requires a new --output directory")
    output = args.output.resolve()
    if output.is_relative_to(source) or source.is_relative_to(output):
        raise ValueError("Snapshot directory must be separate from the source tree")
    output.mkdir(parents=True, exist_ok=False)
    # HEAD includes its complete reachable history without unrelated stashes or refs.
    subprocess.run(["git", "-C", str(source), "bundle", "create", str(output / "history.bundle"), "HEAD"], check=True)
    subprocess.run(["git", "-C", str(source), "bundle", "verify", str(output / "history.bundle")], check=True)
    with zipfile.ZipFile(output / "working-tree.zip", "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for relative, recorded in files.items():
            path = source / relative
            data = os.readlink(path).encode("utf-8") if path.is_symlink() else path.read_bytes()
            if digest(data) != recorded["sha256"]:
                raise RuntimeError("Source changed while snapshot was being created: " + relative)
            entry = zipfile.ZipInfo(relative)
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = recorded["mode"] << 16
            archive.writestr(entry, data)
    if support:
        with zipfile.ZipFile(output / "support-files.zip", "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            for relative, recorded in support.items():
                data = Path(recorded["source"]).read_bytes()
                if digest(data) != recorded["sha256"]:
                    raise RuntimeError("Support file changed while snapshot was being created: " + relative)
                archive.writestr(relative, data)
    final_files, final_excluded, final_missing = collect(source)
    if (final_files, final_excluded, final_missing) != (files, excluded, missing) or git(source, "rev-parse", "HEAD").decode().strip() != head:
        raise RuntimeError("Source changed during snapshot. This directory is incomplete; make a new snapshot after edits finish.")
    if collect_support(args) != support:
        raise RuntimeError("Support files changed during snapshot. This directory is incomplete; make a new snapshot after edits finish.")
    with zipfile.ZipFile(output / "working-tree.zip") as archive:
        if archive.testzip() is not None or any(digest(archive.read(name)) != item["sha256"] for name, item in files.items()):
            raise RuntimeError("Working-tree archive verification failed")
    if support:
        with zipfile.ZipFile(output / "support-files.zip") as archive:
            if archive.testzip() is not None or any(digest(archive.read(name)) != item["sha256"] for name, item in support.items()):
                raise RuntimeError("Support archive verification failed")
    metadata = {**summary, "createdUtc": datetime.now(timezone.utc).isoformat(), "status": status, "fileManifest": files, "supportManifest": support}
    (output / "source-manifest.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    instructions = f"""privateCloud source recovery snapshot

Base commit: {head}
history.bundle contains the base commit and its reachable Git history.
working-tree.zip contains current tracked/untracked source files, including the
installer recipe. source-manifest.json records checksums, deletions and exclusions.
support-files.zip, when present, holds external tooling, build settings and handoff
notes in separate directories; it is not an overlay of the source checkout.
The archive retains only the two fingerprinted disposable test private keys.
It excludes detected local credentials, build outputs and caches. This is not a
replacement for a secrets review before publishing the source.

Recover only into a NEW empty directory:
1. Verify every SHA256 in SHA256SUMS.json.
2. git init <new-directory>
3. git -C <new-directory> fetch <snapshot>/history.bundle HEAD
4. git -C <new-directory> checkout -b privatecloud-recovered FETCH_HEAD
5. Overlay working-tree.zip into that new checkout. Preserve actual symlink
   entries if using a platform with symlinks; Windows checkouts may use text links.
6. In the new checkout only, remove the exact deletedTrackedFiles listed in the
   manifest, after checking each resolved path remains below that checkout.
7. Review exclusions and git status. Verify source files against fileManifest.
8. Extract support-files.zip separately, if present, and verify supportManifest.
   Use its CMakeCache.txt to inspect original build flags, not as a replacement
   cache in a different checkout. Review the latest handoff section before builds.

No commit was created and the original repository/index were not changed.
"""
    (output / "RECOVERY.txt").write_text(instructions, encoding="utf-8")
    checksums = {path.name: digest(path.read_bytes()) for path in sorted(output.iterdir()) if path.is_file()}
    (output / "SHA256SUMS.json").write_text(json.dumps(checksums, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({**summary, "output": str(output), "verified": True}, indent=2))


if __name__ == "__main__":
    main()
