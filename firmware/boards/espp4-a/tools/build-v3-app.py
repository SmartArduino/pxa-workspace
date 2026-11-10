#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build an explicitly revision-qualified P4 v3 performance package locally.

Keeps the SDK's generic P4 recipe unchanged: early P4 chips lack Zb. Outputs
are for v3+ devices only and are never uploaded to the Store by this tool.
"""
import argparse
import json
import os
import math
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", help="Source directory, e.g. local/pxa-apps/duke3d")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--chip-revision", type=float, required=True,
                        help="Revision already verified on the actual target device")
    args = parser.parse_args()
    if not math.isfinite(args.chip_revision) or args.chip_revision < 3:
        parser.error("the Zb recipe requires a verified ESP32-P4 revision >= 3.0")
    project = Path(__file__).resolve().parents[4]
    source = Path(args.app).resolve()
    metadata = json.loads((source / "package.json").read_text())
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    sdk = project / "deps/pxa-system"
    staged = output / "sdk-profile"
    staged.mkdir()
    for item in sdk.iterdir():
        if item.name in {".git", "local", "build", "__pycache__"}:
            continue
        target = staged / item.name
        if item.name in {"tools", "config"}:
            shutil.copytree(item, target, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        else:
            target.symlink_to(item.resolve(), target_is_directory=item.is_dir())
    path = staged / "config/aot-targets.json"
    recipes = json.loads(path.read_text())
    flags = recipes["esp32p4"]["flags"]
    # Retain WAMR's RV32 small-code-model recipe, including size-level=3.
    # Its default code model is unsupported by the bundled RV32 LLVM backend.
    flags = [flag for flag in flags if not flag.startswith("--cpu-features=")]
    flags += ["--cpu-features=+m,+a,+f,+c,+zba,+zbb,+zbs"]
    recipes["esp32p4"]["flags"] = flags
    path.write_text(json.dumps(recipes, indent=2) + "\n")
    (output / "hardware-requirements.json").write_text(json.dumps({
        "chip": "esp32p4", "minimum_revision": 3.0,
        "verified_revision": args.chip_revision, "abi": "ilp32f",
        "flags": flags, "generic_sdk_unchanged": True,
        "distribution": "local revision-qualified build; do not publish as generic ESP32-P4"
    }, indent=2) + "\n")
    env = os.environ.copy()
    env["PXSYS_ROOT"] = str(staged)
    # Keep the actual publisher/signature workflow, resource binding and app
    # identity. No replacement of installed packages occurs during this build.
    subprocess.run(["bash", str(project / "tools/app.sh"), "build", source.name,
                    "--board", "espp4-a", "--target", "esp32p4", "--aot-only",
                    "--source-root", str(source.parent), "--output", str(output / "package")],
                   env=env, check=True, cwd=project)
    print("Built", metadata["id"], "for ESP32-P4 v3+ with the ILP32F ABI", flush=True)


if __name__ == "__main__":
    main()
