#!/usr/bin/env python3
"""Check size-oriented AOT code against the current RV32 Host symbol ABI."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("wamrc", "wasi-sdk", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    result = out / "result.json"
    result.unlink(missing_ok=True)
    source = Path(__file__).with_name("size_codegen_probe.c")
    wasm = out / "probe.wasm"
    subprocess.run([str(args.wasi_sdk / "bin/clang"), "-O2", "-nostdlib",
                    "-Wl,--no-entry", str(source), "-o", str(wasm)], check=True)
    rows = []
    for level in ("3", "s", "z"):
        command = [str(args.wamrc), "--target=riscv32", "--target-abi=ilp32f",
                   "--cpu=generic-rv32", "--cpu-features=+m,+a,+f,+c,+zba,+zbb,+zbs",
                   "--opt-level=" + level, "--size-level=3"]
        obj = out / (level + ".o")
        subprocess.run(command + ["--format=object", "-o", str(obj), str(wasm)],
                       check=True)
        symbols = subprocess.check_output(
            [str(args.wasi_sdk / "bin/llvm-nm"), "--undefined-only", str(obj)],
            text=True)
        undefined = {line.split()[-1] for line in symbols.splitlines() if line.strip()}
        # This probe needs only the ordinary WAMR exception helper. Fail on
        # any new helper, not just the three shifts observed previously.
        unexpected = undefined - {"aot_set_exception_with_id"}
        if unexpected:
            raise RuntimeError(f"RV32 O{level} needs unsupported helpers: {sorted(unexpected)}")
        subprocess.run(command + ["-o", str(out / (level + ".aot")), str(wasm)],
                       check=True)
        rows.append({"level": level, "undefined": sorted(undefined)})
    result.write_text(json.dumps({
        "passed": True, "cases": rows,
        "compiler_sha256": hashlib.sha256(args.wamrc.read_bytes()).hexdigest(),
        "device_fps_measured": False,
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()
