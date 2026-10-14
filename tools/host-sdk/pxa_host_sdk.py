#!/usr/bin/env python3
"""Build, install and verify a fixed, offline ESP-IDF PXA Host SDK."""
from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile
import tempfile

SCHEMA = "pxa-esp-host-sdk-1"
LOCK_SCHEMA = "pxa-esp-host-sdk-lock-1"
COMPONENTS = (
    "pxa", "pxa_system_core", "pxa_system_native", "pxa_system_pxa",
    "pxa_system_standard", "pxa_system_esp", "pxa_system_renderer_lvgl",
    "pxa_system_renderer_headless", "pxa_system_reference_ui", "pxa_lvgl_input",
    "pxa_lvgl_memory", "pxadb", "page_manager",
)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def json_bytes(value: object) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode()


def relative_path(value: str) -> PurePosixPath:
    path = PurePosixPath(value)
    if not value or path.is_absolute() or ".." in path.parts or "\\" in value:
        raise ValueError(f"Unsafe relative path: {value!r}")
    return path


def git(root: Path, *args: str) -> str:
    return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()


def require_clean(root: Path) -> str:
    if git(root, "status", "--porcelain", "--untracked-files=normal"):
        raise ValueError(f"SDK sources must be committed and clean: {root}")
    return git(root, "rev-parse", "HEAD")


def export_git(root: Path, destination: Path, paths: list[str]) -> None:
    data = subprocess.check_output(["git", "-C", str(root), "archive", "HEAD", *paths])
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        archive.extractall(destination, filter="data")


def verify(root: Path, lock: dict | None = None) -> dict:
    manifest_path = root / "host-sdk.json"
    manifest_data = manifest_path.read_bytes()
    if lock is not None and digest(manifest_data) != lock["manifest_sha256"]:
        raise ValueError("SDK manifest differs from the product lock")
    manifest = json.loads(manifest_data)
    if manifest.get("schema") != SCHEMA:
        raise ValueError("Unsupported Host SDK manifest")
    if lock is not None:
        if lock.get("schema") != LOCK_SCHEMA:
            raise ValueError("Unsupported Host SDK lock")
        for key in ("version", "pxa_version", "sources"):
            if manifest[key] != lock[key]:
                raise ValueError(f"SDK {key} differs from product lock")
    expected = set(manifest["files"]) | {"host-sdk.json"}
    actual: set[str] = set()
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ValueError(f"SDK symlinks are forbidden: {path}")
        if path.is_file():
            actual.add(path.relative_to(root).as_posix())
    if actual != expected:
        raise ValueError(f"SDK inventory changed: missing={sorted(expected-actual)[:5]}, extra={sorted(actual-expected)[:5]}")
    for name, entry in manifest["files"].items():
        path = root / relative_path(name)
        if digest(path.read_bytes()) != entry["sha256"]:
            raise ValueError(f"SDK file modified: {name}; fix upstream and replace the locked SDK")
    return manifest


def install(lock_path: Path, archive_path: Path | None, destination: Path | None) -> Path:
    lock = json.loads(lock_path.read_text())
    if lock.get("schema") != LOCK_SCHEMA:
        raise ValueError("Unsupported Host SDK lock")
    archive_path = archive_path or lock_path.parent / relative_path(lock["archive"])
    destination = destination or lock_path.parent / relative_path(lock["install_dir"])
    if destination.exists():
        verify(destination, lock)
        return destination
    if digest(archive_path.read_bytes()) != lock["archive_sha256"]:
        raise ValueError("Host SDK archive SHA-256 mismatch")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".pxa-host-sdk-", dir=destination.parent) as temporary:
        staging = Path(temporary) / "sdk"
        staging.mkdir()
        with tarfile.open(archive_path, "r:gz") as archive:
            for member in archive.getmembers():
                relative_path(member.name)
                if not (member.isfile() or member.isdir()):
                    raise ValueError(f"Unsupported SDK archive member: {member.name}")
            archive.extractall(staging, filter="data")
        verify(staging, lock)
        os.rename(staging, destination)
    return destination


def package(workspace: Path, system: Path, output: Path, revision: int) -> tuple[Path, Path]:
    workspace_commit = require_clean(workspace)
    system_commit = require_clean(system)
    version = (system / "VERSION").read_text().strip()
    wamr_config = json.loads((system / "config/wamr.json").read_text())
    pins = {}
    for name in ("wamr", "ui/backends/lvgl/page_manager"):
        pin = git(system, "ls-tree", "HEAD", name).split()[2]
        if require_clean(system / name) != pin:
            raise ValueError(f"Unpinned dependency: {name}")
        pins[name] = pin
    if pins["wamr"] != wamr_config["commit"]:
        raise ValueError("WAMR config and submodule pin differ")
    kit_version = f"{version}+idf.{revision}"
    output.mkdir(parents=True, exist_ok=True)
    filename = f"pxa-host-sdk-{version}-idf.{revision}.tar.gz"
    archive_path = output / filename
    lock_path = output / "pxa-host-sdk.lock.json"
    if archive_path.exists():
        raise ValueError(f"Refusing to overwrite an immutable SDK archive: {archive_path}")
    with tempfile.TemporaryDirectory(prefix="pxa-host-sdk-") as temporary:
        root = Path(temporary)
        export_git(workspace, root, [*(f"firmware/components/{name}" for name in COMPONENTS),
                                    "tools/host-sdk", "tools/audit-codec-memory.py",
                                    "tools/test-audit-codec-memory.py", "cmake/pxa_host_sdk.cmake",
                                    "docs/pxa-esp-host-sdk.zh-CN.md"])
        system_dest = root / "deps/pxa-system"
        system_dest.mkdir(parents=True)
        # Guest apps and their development signing keys never enter Host SDKs.
        core_paths = [name for name in git(system, "ls-tree", "--name-only", "HEAD").splitlines()
                      if name not in ("apps", "wamr")]
        export_git(system, system_dest, core_paths)
        for name in pins:
            nested = system_dest / name
            nested.mkdir(parents=True, exist_ok=True)
            paths = ["CMakeLists.txt", "core", "build-scripts", "LICENSE"] if name == "wamr" else []
            export_git(system / name, nested, paths)
        header = root / "firmware/components/pxa/include/pxa/host_sdk_version.h"
        header.write_text(f'#pragma once\n#define PXA_HOST_SDK_PXA_VERSION "{version}"\n#define PXA_HOST_SDK_VERSION "{kit_version}"\n'
                          f'#define PXA_HOST_SDK_SYSTEM_COMMIT "{system_commit}"\n'
                          f'#define PXA_HOST_SDK_WORKSPACE_COMMIT "{workspace_commit}"\n')
        files = {}
        for path in sorted(root.rglob("*")):
            if path.is_symlink():
                raise ValueError(f"Symlink in source payload: {path}")
            if path.is_file():
                files[path.relative_to(root).as_posix()] = {
                    "sha256": digest(path.read_bytes()), "mode": 0o755 if path.stat().st_mode & 0o111 else 0o644}
        sources = {"workspace": workspace_commit, "system": system_commit,
                   "wamr": pins["wamr"], "page_manager": pins["ui/backends/lvgl/page_manager"]}
        manifest = {"schema": SCHEMA, "version": kit_version, "pxa_version": version,
                    "engine_abi": wamr_config["engine_abi"], "sources": sources,
                    "components": list(COMPONENTS), "files": files}
        manifest_data = json_bytes(manifest)
        (root / "host-sdk.json").write_bytes(manifest_data)
        verify(root)
        with archive_path.open("xb") as stream:
            with gzip.GzipFile(fileobj=stream, mode="wb", filename="", mtime=0) as compressed:
                with tarfile.open(fileobj=compressed, mode="w") as archive:
                    for name in sorted(set(files) | {"host-sdk.json"}):
                        data = (root / name).read_bytes()
                        info = tarfile.TarInfo(name)
                        info.size = len(data)
                        info.mode = files.get(name, {"mode": 0o644})["mode"]
                        archive.addfile(info, io.BytesIO(data))
    lock = {"schema": LOCK_SCHEMA, "version": kit_version, "pxa_version": version,
            "sources": sources, "archive": filename,
            "archive_sha256": digest(archive_path.read_bytes()), "manifest_sha256": digest(manifest_data),
            "install_dir": f"deps/pxa-host-sdk-{version}-idf.{revision}"}
    lock_path.write_bytes(json_bytes(lock))
    return archive_path, lock_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("package")
    build.add_argument("--workspace", type=Path, required=True)
    build.add_argument("--system", type=Path, required=True)
    build.add_argument("--output", type=Path, required=True)
    build.add_argument("--revision", type=int, default=1)
    deploy = commands.add_parser("install")
    deploy.add_argument("--lock", type=Path, required=True)
    deploy.add_argument("--archive", type=Path)
    deploy.add_argument("--destination", type=Path)
    check = commands.add_parser("verify")
    check.add_argument("--root", type=Path, required=True)
    check.add_argument("--lock", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "package":
            print("\n".join(map(str, package(args.workspace, args.system, args.output, args.revision))))
        elif args.command == "install":
            print(install(args.lock, args.archive, args.destination))
        else:
            lock = json.loads(args.lock.read_text()) if args.lock else None
            result = verify(args.root, lock)
            print(f"Verified immutable PXA Host SDK {result['version']} / {result['sources']['system']}")
    except (ValueError, OSError, KeyError, IndexError, tarfile.TarError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"pxa-host-sdk: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
