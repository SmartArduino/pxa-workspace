#!/usr/bin/env python3
"""Build every maintained app against the current SDK and optionally smoke it.

Run from any directory. Outputs, isolated app state and logs live under --output.
A smoke probe is the native pxsys_app_matrix_test target from the desktop build.
This checks startup/lifecycle, not all application-specific user journeys.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import signal
from pathlib import Path
import subprocess
import time


def run_logged(command: list[str], log, timeout: int, **kwargs) -> None:
    # A packaging timeout must also stop its compiler children before the next
    # package can reuse an incremental build directory.
    with subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                          start_new_session=True, **kwargs) as process:
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            raise
        if code:
            raise subprocess.CalledProcessError(code, command)


def source_fingerprint(paths: list[Path], settings: dict) -> str:
    digest = hashlib.sha256(json.dumps(settings, sort_keys=True).encode())
    for root in paths:
        for path in ([root] if root.is_file() else sorted(root.rglob("*"))):
            if not path.is_file() or any(part in {".git", ".dev-signing", "__pycache__", "docs", "tests"}
                                         for part in path.relative_to(root.parent if root.is_file() else root).parts):
                continue
            if path.name.startswith("test_"):
                continue
            digest.update(path.relative_to(root.parent if root.is_file() else root).as_posix().encode() + b"\0")
            digest.update(path.read_bytes())
    return digest.hexdigest()


def main() -> int:
    workspace = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--target", default="all")
    parser.add_argument("--sdk-examples", action="store_true")
    parser.add_argument("--apps", nargs="+", help="directory names to select")
    parser.add_argument("--exclude", nargs="+", default=[])
    parser.add_argument("--probe", type=Path)
    parser.add_argument("--run-only", action="store_true")
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()
    if args.run_only and not args.probe:
        parser.error("--run-only requires --probe")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report_path = output / "report.json"
    previous = json.loads(report_path.read_text()) if args.resume and report_path.exists() else {}
    report = {"target": args.target, "scope": "build and startup/lifecycle smoke", "apps": {}}
    roots = {"apps": workspace / "local/pxa-apps"}
    if args.sdk_examples:
        roots["sdk"] = workspace / "deps/pxa-system/sdk/guest-cpp/examples"
    key = Path(os.environ.get("PXA_SIGNING_KEY", roots["apps"] / ".dev-signing/publisher-private.pem"))
    public = output / "publisher.der"
    subprocess.run(["openssl", "pkey", "-in", str(key), "-pubout", "-outform", "DER", "-out", str(public)], check=True)
    selected = 0
    sdk = workspace / "deps/pxa-system/sdk"
    sdk_hash = source_fingerprint([sdk / "guest-cpp/include", sdk / "guest-cpp/src",
                                  sdk / "guest-c/include", sdk / "cmake",
                                  workspace / "local/pxa-apps/common",
                                  workspace / "deps/pxa-system/tools/package",
                                  workspace / "deps/pxa-system/tools/i18n",
                                  workspace / "tools/app.sh"], {})
    for group, source in roots.items():
        for manifest in sorted(source.glob("*/package.json")):
            name = manifest.parent.name
            if name in args.exclude or (args.apps and name not in args.apps):
                continue
            selected += 1
            identity = f"{group}/{name}"
            directory = output / group / name
            directory.mkdir(parents=True, exist_ok=True)
            metadata = json.loads(manifest.read_text())
            fingerprint = source_fingerprint([manifest.parent], {
                "sdk": sdk_hash, "target": args.target,
                "defines": os.environ.get("PXA_APP_DEFINES", ""),
                "publisher": hashlib.sha256(public.read_bytes()).hexdigest()})
            row = {"id": metadata["id"], "version": metadata["version"],
                   "source_sha256": fingerprint, "passed": False}
            report["apps"][identity] = row
            started = time.monotonic()
            try:
                if any(metadata.get(k) != [2, 0] for k in ("min_sdk", "target_sdk", "compile_sdk")):
                    raise ValueError("manifest must declare the current Core 2 SDK")
                package = directory / ("pxa-" + name)
                old = previous.get("apps", {}).get(identity, {})
                build_done = (args.resume and old.get("build_passed") and
                              old.get("source_sha256") == fingerprint and
                              (package / "manifest.pxm").exists())
                if args.run_only and old.get("source_sha256") != fingerprint:
                    raise ValueError("package source/SDK changed; rebuild before testing")
                if not args.run_only and not build_done:
                    command = ["bash", str(workspace / "tools/app.sh"), "build", name,
                               "--source-root", str(source), "--target", args.target, "--output", str(directory)]
                    with (directory / "build.log").open("w") as log:
                        run_logged(command, log, 1800, cwd=workspace,
                                   env=os.environ | {"PXA_SIGNING_KEY": str(key)})
                if not (package / "manifest.pxm").is_file():
                    raise FileNotFoundError(package / "manifest.pxm")
                row["build_passed"] = True
                container = directory / ("pxa-" + name + ".pxa")
                if container.exists():
                    row["package_bytes"] = container.stat().st_size
                    row["package_sha256"] = hashlib.sha256(container.read_bytes()).hexdigest()
                if args.probe:
                    row["profiles"] = []
                    for label, width, height, dpi, shape in (("touch", 296, 240, 160, 1),
                                                           ("dense", 480, 480, 305, 1),
                                                           ("round", 176, 176, 160, 2)):
                        prefix = directory / label
                        state = directory / "state" / label
                        state.mkdir(parents=True, exist_ok=True)
                        command = [str(args.probe.resolve()), str(package), str(public),
                                   str(state), str(width), str(height),
                                   str(dpi), str(shape), str(prefix)]
                        with prefix.with_suffix(".log").open("w") as log:
                            run_logged(command, log, 60, cwd=workspace)
                        row["profiles"].append(json.loads(prefix.with_suffix(".json").read_text()))
                row["passed"] = True
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                row["error"] = str(error)
            row["seconds"] = round(time.monotonic() - started, 3)
            report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
            print(identity, "PASS" if row["passed"] else "FAIL", row.get("error", ""), flush=True)
    if not selected:
        parser.error("no apps selected")
    return 0 if all(row["passed"] for row in report["apps"].values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
