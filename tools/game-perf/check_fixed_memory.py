#!/usr/bin/env python3
"""Exercise an opt-in WAMR fixed-memory optimization; outputs stay local."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess


def exported_function_indices(data):
    """Read the small probe's function imports and exports (no other imports)."""
    def leb(at):
        value = shift = 0
        while True:
            byte = data[at]; at += 1
            value |= (byte & 127) << shift
            if byte < 128:
                return value, at
            shift += 7
    at, imports, exports = 8, 0, {}
    while at < len(data):
        kind = data[at]; size, at = leb(at + 1); end = at + size
        if kind == 2:
            imports, cursor = leb(at)
            for _ in range(imports):
                for _ in range(2):
                    length, cursor = leb(cursor); cursor += length
                assert data[cursor] == 0, "Probe only imports functions"
                _, cursor = leb(cursor + 1)
        if kind == 7:
            count, cursor = leb(at)
            for _ in range(count):
                length, cursor = leb(cursor)
                name = data[cursor:cursor + length].decode(); cursor += length
                export_kind = data[cursor]; index, cursor = leb(cursor + 1)
                if export_kind == 0:
                    exports[name] = index - imports
        at = end
    return exports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("wamrc", "wasi-sdk", "runtime-library", "wamr", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--baseline-wamrc", type=Path,
                        help="Compare fixed-memory RV32 metadata loads against this compiler")
    parser.add_argument("--require-bounded-access", action="store_true",
                        help="Also require proven-safe masked accesses to omit bounds loads")
    parser.add_argument("--candidate-opt-level", choices=("0", "1", "2", "3", "s", "z"),
                        default="3", help="Candidate pipeline; baseline comparisons retain level 3")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / "result.json").unlink(missing_ok=True)
    here = Path(__file__).resolve().parent

    def run(*argv):
        subprocess.run([str(a) for a in argv], check=True)

    runner = out / "runner"
    run(*shlex.split(os.environ.get("CC", "cc")), "-O2", "-Wall", "-Wextra",
        "-I" + str(args.wamr / "core/iwasm/include"),
        here / "fixed_memory_runner.c", args.runtime_library,
        "-lm", "-ldl", "-pthread", "-o", runner)
    cases = []
    for name, initial, maximum in (("fixed1", 1, 1), ("fixed2", 2, 2), ("growing", 2, 3)):
        wasm, aot = out / (name + ".wasm"), out / (name + ".aot")
        run(args.wasi_sdk / "bin/clang", "-O2", "-nostdlib", "-Wl,--no-entry",
            "-Wl,-z,stack-size=8192", "-Wl,--initial-memory=" + str(initial * 65536),
            "-Wl,--max-memory=" + str(maximum * 65536),
            here / "fixed_memory_probe.c", "-o", wasm)
        run(args.wamrc, "--target=x86_64", "--opt-level=" + args.candidate_opt_level, "--bounds-checks=1",
            "-o", aot, wasm)
        for heap in (0, 16384):
            run(runner, aot, heap, int(initial == maximum))
            cases.append({"case": name, "heap": heap, "passed": True})
    wasm, aot = out / "shrunk.wasm", out / "shrunk.aot"
    run(args.wasi_sdk / "bin/clang", "-O2", "-nostdlib", "-Wl,--no-entry",
        "-Wl,-z,stack-size=8192", "-Wl,--initial-memory=131072", "-Wl,--max-memory=131072",
        "-Wl,--export=__heap_base", "-Wl,--export=__data_end", "-Wl,--export=__stack_pointer",
        here / "shrunk_memory_probe.c", "-o", wasm)
    run(args.wamrc, "--target=x86_64", "--opt-level=" + args.candidate_opt_level, "--bounds-checks=1",
        "-o", aot, wasm)
    for heap in (0, 16384):
        run(runner, aot, heap, 2)
        cases.append({"case": "shrunk", "heap": heap, "passed": True})
    codegen = {}
    if args.require_bounded_access and not args.baseline_wamrc:
        parser.error("--require-bounded-access needs --baseline-wamrc")
    if args.baseline_wamrc:
        exports = exported_function_indices((out / "fixed2.wasm").read_bytes())
        flags = ("--target=riscv32", "--target-abi=ilp32f", "--cpu=generic-rv32",
                 "--cpu-features=+m,+a,+f,+c,+zba,+zbb,+zbs",
                 "--size-level=3", "--format=llvmir-opt")
        for label, compiler in (("baseline", args.baseline_wamrc), ("candidate", args.wamrc)):
            ir = out / (label + ".ll")
            level = args.candidate_opt_level if label == "candidate" else "3"
            run(compiler, *flags, "--opt-level=" + level, "-o", ir, out / "fixed2.wasm")
            text = ir.read_text()
            codegen[label] = {
                "base_loads": len(re.findall(r"%mem_base\w* = load ptr", text)),
                "bounds_loads": len(re.findall(r"%(?:mem_check_bound\w*|bound_check_\w*) = load i32", text)),
                "loop_bounds_loads": len(re.findall(
                    r"%(?:mem_check_bound\w*|bound_check_\w*) = load i32",
                    text[text.rindex("loop0_begin:"):].split("\n}", 1)[0])),
            }
            for name in ("bounded_read", "bounded_write"):
                body = re.search(r'^define internal[^\n]*@"aot_func_internal#' +
                                 str(exports[name]) + r'"[^\n]*\{\n(.*?)^}',
                                 text, re.M | re.S)
                assert body, name
                codegen[label][name + "_bounds_loads"] = len(re.findall(
                    r"%(?:mem_check_bound\w*|bound_check_\w*) = load i32", body[1]))
        assert 0 < codegen["candidate"]["base_loads"] < codegen["baseline"]["base_loads"], codegen
        # One bounds load can remain, but moves outside the call-containing loop.
        assert codegen["candidate"]["bounds_loads"] > 0, codegen
        assert codegen["candidate"]["loop_bounds_loads"] == 0, codegen
        assert codegen["baseline"]["loop_bounds_loads"] > 0, codegen
        if args.require_bounded_access:
            for name in ("bounded_read", "bounded_write"):
                assert codegen["candidate"][name + "_bounds_loads"] == 0, codegen
                assert codegen["baseline"][name + "_bounds_loads"] > 0, codegen
    (out / "result.json").write_text(json.dumps({
        "cases": cases, "passed": True,
        "compiler_sha256": hashlib.sha256(args.wamrc.read_bytes()).hexdigest(),
        "candidate_opt_level": args.candidate_opt_level,
        "codegen": codegen,
        "device_fps_measured": False,
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()
