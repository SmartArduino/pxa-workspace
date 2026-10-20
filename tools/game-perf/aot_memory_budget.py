#!/usr/bin/env python3
"""Compare WAMR v6 section sizes; report a lower bound, never load admission."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def inspect(path):
    data = path.read_bytes()
    if len(data) < 8 or data[:8] != b"\0aot\x06\0\0\0":
        raise ValueError(f"{path}: expected WAMR AOT v6")
    offset = 8
    sections = []
    seen = set()
    while offset < len(data):
        if len(data) - offset < 8:
            raise ValueError(f"{path}: truncated section header")
        kind, size = struct.unpack_from("<II", data, offset)
        offset += 8
        if size > len(data) - offset:
            raise ValueError(f"{path}: truncated section {kind}")
        # WAMR can have more than one custom section (100).
        if kind in seen and kind != 100:
            raise ValueError(f"{path}: duplicate section {kind}")
        seen.add(kind)
        sections.append({"kind": kind, "bytes": size})
        offset = (offset + size + 3) & ~3
        if offset > len(data):
            raise ValueError(f"{path}: missing section padding")
    if 2 not in seen:
        raise ValueError(f"{path}: missing text section")
    return {
        "path": str(path), "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(), "sections": sections,
        "text_section_bytes": next(s["bytes"] for s in sections if s["kind"] == 2),
    }


def nonnegative(value):
    result = int(value, 0)
    if result < 0:
        raise argparse.ArgumentTypeError("expected nonnegative byte count")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("aot", type=Path, nargs="+")
    parser.add_argument("--reserved-linear", type=nonnegative, required=True)
    parser.add_argument("--idle-psram", type=nonnegative, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    rows = [inspect(path) for path in args.aot]
    for row in rows:
        total = row["text_section_bytes"] + args.reserved_linear
        row.update(text_plus_linear_bytes=total,
                   remaining_before_other_allocations_bytes=args.idle_psram - total,
                   text_section_reduction_from_first_bytes=(
                       rows[0]["text_section_bytes"] - row["text_section_bytes"]))
    report = {
        "schema": "pxa-aot-memory-lower-bound-1",
        "reserved_linear_bytes": args.reserved_linear,
        "observed_idle_psram_bytes": args.idle_psram,
        "artifacts": rows,
        "scope": "Static section-size diagnostic only. Text includes its literal prefix. "
                 "Alignment, loader metadata, relocations, runtime heap, stacks and other "
                 "allocations are excluded. Positive remaining space does not establish "
                 "load admission, executable imports, correctness or device FPS.",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
