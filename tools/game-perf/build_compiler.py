#!/usr/bin/env python3
"""Build the opt-in memory compiler without changing the SDK's WAMR checkout."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


PIN = "6a54186abc1c10c37c28e443107107c97af8f25f"
PROFILES = {
    "fixed-memory": ["wamr-base-alignment.patch", "wamr-fixed-memory.patch"],
    "minimum-range": ["wamr-base-alignment.patch", "wamr-fixed-memory.patch",
                      "wamr-minimum-memory-range.patch"],
    "size-experiment": ["wamr-base-alignment.patch", "wamr-fixed-memory.patch",
                        "wamr-size-optimization.patch"],
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture(*args):
    return subprocess.check_output([str(a) for a in args], text=True).strip()


def source_state(source):
    return {
        "commit": capture("git", "-C", source, "rev-parse", "HEAD"),
        "diff": hashlib.sha256(subprocess.check_output(
            ["git", "-C", str(source), "diff", "--binary", "HEAD"])).hexdigest(),
        "status": capture("git", "-C", source, "status", "--porcelain",
                          "--untracked-files=normal"),
    }


def save(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def main():
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, default=here.parents[1] / "deps/pxa-system")
    parser.add_argument("--output", type=Path, required=True,
                        help="Local artifact directory; compiler path is printed to stdout")
    parser.add_argument("--profile", choices=PROFILES, default="fixed-memory")
    parser.add_argument("--jobs", type=int, default=3)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    sdk = args.sdk.resolve()
    config = json.loads((sdk / "config/wamr.json").read_text())
    if config["commit"] != PIN:
        parser.error("Memory patches require the pinned WAMR revision " + PIN)
    patches = [here / name for name in PROFILES[args.profile]]
    bootstrap = sdk / "tools/package/build_wamrc.sh"
    recipe = {
        "profile": args.profile, "wamr": config,
        "patches": {p.name: sha(p) for p in patches},
        "bootstrap": sha(bootstrap), "builder": sha(Path(__file__)),
        "sdk": str(sdk),
        "toolchain": {key: os.environ.get(key, "") for key in
                      ("CC", "CXX", "CFLAGS", "CXXFLAGS", "LDFLAGS",
                       "PXA_WAMR_CC", "PXA_WAMR_CXX", "PXA_WAMR_LLVM_DIR",
                       "PXA_WAMR_CACHE_DIR")},
    }
    key = hashlib.sha256(json.dumps(recipe, sort_keys=True).encode()).hexdigest()[:20]
    output = args.output.resolve() / (args.profile + "-" + key)
    output.mkdir(parents=True, exist_ok=True)
    with (output / "build.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        source, build = output / "wamr", output / "build"
        prepared, artifact = output / "source.json", output / "compiler.json"
        compiler = build / "wamrc"
        if prepared.exists():
            old = json.loads(prepared.read_text())
            if old["recipe"] != recipe or old["source"] != source_state(source):
                raise RuntimeError("Compiler source was changed; choose a new --output directory")
        else:
            if source.exists():
                raise RuntimeError("Unrecorded source directory exists; choose a new --output directory")
            subprocess.run(["git", "-C", str(sdk / "wamr"), "worktree", "add",
                            "--detach", str(source), PIN], check=True, stdout=sys.stderr)
            for patch in patches:
                subprocess.run(["git", "-C", str(source), "apply", "--check", str(patch)], check=True)
                subprocess.run(["git", "-C", str(source), "apply", str(patch)], check=True)
            save(prepared, {"recipe": recipe, "source": source_state(source)})
        if artifact.exists():
            record = json.loads(artifact.read_text())
            if record["recipe"] != recipe or not compiler.is_file() or sha(compiler) != record["sha256"]:
                raise RuntimeError("Cached compiler does not match its record; choose a new --output directory")
            print(compiler)
            return
        env = os.environ.copy()
        env.update(PXA_WAMR_SOURCE_DIR=str(source), PXA_WAMR_BUILD_DIR=str(build),
                   PXA_WAMR_LOCK_FILE=str(output / "bootstrap.lock"),
                   PXA_WAMR_JOBS=str(args.jobs))
        subprocess.run(["bash", str(bootstrap)], env=env, check=True, stdout=sys.stderr)
        # Also completes an interrupted build if bootstrap found its executable.
        subprocess.run(["cmake", "--build", str(build), "--parallel", str(args.jobs)],
                       check=True, stdout=sys.stderr)
        version = capture(compiler, "--version")
        if source_state(source) != json.loads(prepared.read_text())["source"]:
            raise RuntimeError("Source changed while compiling; no compiler record written")
        save(artifact, {"recipe": recipe, "sha256": sha(compiler), "version": version})
        print(compiler)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"build_compiler: {error}", file=sys.stderr)
        raise SystemExit(1)
