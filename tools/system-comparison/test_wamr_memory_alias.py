#!/usr/bin/env python3
"""Check grow barriers, linear-memory aliasing and software bounds traps."""
from pathlib import Path
import argparse
import json
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wamr", type=Path, required=True)
    parser.add_argument("--wamrc", type=Path, required=True)
    parser.add_argument("--iwasm", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expect-bound-ranges", action="store_true")
    parser.add_argument("--clang", type=Path, default=Path.home()/".local/share/pxa/releases/0.11.0/toolchains/wasi-sdk/bin/clang")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = Path(__file__).resolve().parent / "wamr-memory-tests"
    commands = []

    def run(command, name, check=True):
        commands.append([str(x) for x in command])
        result = subprocess.run(command, capture_output=True, text=True)
        (args.output / (name + ".log")).write_text(result.stdout + result.stderr)
        if check and result.returncode:
            raise RuntimeError(name + ": " + (result.stdout + result.stderr)[-1500:])
        return result

    native = args.output / "native.so"
    run(["cc", "-shared", "-fPIC", "-O2", "-I" + str(args.wamr / "core/iwasm/include"),
         str(fixture / "native.c"), "-o", str(native)], "native-build")
    base = [str(args.clang), "--target=wasm32-unknown-unknown", "-O3", "-fno-builtin",
            "-fno-strict-aliasing", "-nostdlib", str(fixture / "guest.c"),
            "-Wl,--no-entry", "-Wl,--export-all", "-Wl,--initial-memory=131072",
            "-Wl,--max-memory=524288", "-Wl,-z,stack-size=16384"]
    wasm = args.output / "guest.wasm"
    run([*base, "-o", str(wasm)], "guest-build")
    aot = args.output / "guest.aot"
    flags = [str(args.wamrc), "--target=x86_64", "--bounds-checks=1", "--stack-bounds-checks=1", "--opt-level=3"]
    run([*flags, "-o", str(aot), str(wasm)], "aot-build")
    results = []
    for function, values, expected in [("static_loop", [], 0), ("static_dynamic_index", [1073741824], 0),
                                       ("grow_direct", [], 0), ("grow_indirect", [], 0),
                                       ("grow_native", [], 0), ("grow_failure", [], 0), ("offset_valid", [], 0),
                                       ("grow_offset_preserve", [4096], 0), ("grow_offset_fresh", [131008], 0),
                                       ("alias_words", [4096, 4096, 1065353216], 0),
                                       ("alias_bytes", [4096, 4096, 305419896], 305419776)]:
        result = run([str(args.iwasm), "--native-lib=" + str(native), "-f", function, str(aot),
                      *map(str, values)], function)
        found = re.search(r"0x([0-9a-fA-F]+):i32", result.stdout)
        if not found or int(found[1], 16) != expected:
            raise AssertionError(function + ": " + result.stdout + result.stderr)
        results.append({"function": function, "passed": True})
    for function in ("trap_read", "trap_write", "trap_crossing", "trap_offset_end", "trap_offset_wrap", "trap_offset_store_wrap"):
        result = run([str(args.iwasm), "--native-lib=" + str(native), "-f", function, str(aot)], function, False)
        if "out of bounds memory access" not in (result.stdout + result.stderr).lower():
            raise AssertionError(function + ": missing bounds trap")
        results.append({"function": function, "passed": True, "expected_trap": True})
    result = run([str(args.iwasm), "--native-lib=" + str(native), "-f", "static_dynamic_index", str(aot), "32768"], "static-dynamic-trap", False)
    if "out of bounds memory access" not in (result.stdout + result.stderr).lower():
        raise AssertionError("Dynamic static-array index lost its bounds check")
    results.append({"function": "static_dynamic_index_oob", "passed": True, "expected_trap": True})
    if "base_moved=1" not in (args.output / "grow_native.log").read_text():
        raise AssertionError("Growth did not move the base; disable hardware bounds checks in the test runtime")
    for variant, extra, aot_extra in [("owned", [], []), ("imported", ["-Wl,--import-memory"], []),
                                     ("shared", ["-matomics", "-mbulk-memory", "-Wl,--shared-memory"], ["--enable-multi-thread"])]:
        module = args.output / (variant + ".wasm")
        ir = args.output / (variant + ".ll")
        run([*base, *extra, "-o", str(module)], variant + "-build")
        run([*flags, *aot_extra, "--format=llvmir-unopt", "-o", str(ir), str(module)], variant + "-ir")
        tags = "!tbaa" in ir.read_text()
        if tags != (variant == "owned"):
            raise AssertionError("Unexpected alias tags for " + variant)
        if args.expect_bound_ranges:
            ranges = any("!range" in line and ("bound_check_" in line or "mem_check_bound" in line)
                         for line in ir.read_text().splitlines())
            if ranges != (variant == "owned"):
                raise AssertionError("Unexpected memory-bound ranges for " + variant)
        results.append({"module": variant, "passed": True, "alias_tags": tags})
    if args.expect_bound_ranges:
        unchecked_ir = args.output / "unchecked.ll"
        run([*flags, "--bounds-checks=0", "--format=llvmir-unopt", "-o", str(unchecked_ir), str(wasm)], "unchecked-ir")
        if any("!range" in line and ("bound_check_" in line or "mem_check_bound" in line)
               for line in unchecked_ir.read_text().splitlines()):
            raise AssertionError("Disabled bounds checks unexpectedly carry bound metadata")
        results.append({"module": "unchecked", "passed": True, "bound_ranges": False})
    zero_wasm = args.output / "zero.wasm"
    zero_aot = args.output / "zero.aot"
    run([str(args.clang), "--target=wasm32-unknown-unknown", "-O3", "-nostdlib",
         str(fixture / "zero.c"), "-Wl,--no-entry", "-Wl,--export-all", "-Wl,--initial-memory=0",
         "-Wl,--max-memory=131072", "-Wl,-z,stack-size=0", "-Wl,--global-base=0", "-o", str(zero_wasm)], "zero-build")
    run([*flags, "-o", str(zero_aot), str(zero_wasm)], "zero-aot")
    result = run([str(args.iwasm), "--heap-size=0", "-f", "zero_then_grow", str(zero_aot)], "zero-grow")
    if not re.search(r"0x0:i32", result.stdout):
        raise AssertionError("Zero-memory growth failed: " + result.stdout + result.stderr)
    result = run([str(args.iwasm), "--heap-size=0", "-f", "zero_trap", str(zero_aot)], "zero-trap", False)
    if "out of bounds memory access" not in (result.stdout + result.stderr).lower():
        raise AssertionError("Missing zero-memory bounds trap")
    results.extend([{"function": "zero_then_grow", "passed": True},
                    {"function": "zero_trap", "passed": True, "expected_trap": True}])
    (args.output / "results.json").write_text(json.dumps({"passed": True, "results": results, "commands": commands}, indent=2) + "\n")
    print("Memory alias semantic regression checks PASS", len(results))


if __name__ == "__main__":
    main()
