#!/usr/bin/env python3
"""Fail the firmware build if the audited codec allocation boundary changes.

Inspect the archives actually selected by the linker, not every optional codec
in the vendor archive. Hashes identify the reviewed binary implementation;
upgrading it requires a fresh import/body review and updating this allowlist.
This proves linkage, not decoder behavior or real-device playback performance.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ARCHIVES = {
    "libesp_audio_codec.a": "4c5d764b3fdcbc75ba727b2cb5db1609363f2eeb4cd62467b74b03234780f3e6",
    "libesp_audio_simple_dec.a": "84c9a0977dbcba3bfc856e5c610fa13390b82b3fa7a07c0b6665847ef5ce2ec4",
}
ARCHIVES_BY_TARGET = {
    "esp32s3": ARCHIVES,
    "esp32s31": {
        "libesp_audio_codec.a": "1fda41dc09d46f1ed8bbe2d687fb6ccad3a5e2ac6726f5d15804f4f0c8a6f0c2",
        "libesp_audio_simple_dec.a": "7cbc14248f4e5746eaeca3c8177c62f903b6388892902fe25ceef3b2469d915f",
    },
}
HOOKS = {"media_lib_module_malloc", "media_lib_module_calloc",
         "media_lib_module_realloc", "media_lib_free"}
MEMORY = re.compile(r"alloc|(?:^|_)free(?:$|_)|strdup|strndup|^_Zn[aw]|^_Zd[al]")


def symbols(text):
    for line in text.splitlines():
        fields = line.split()
        if len(fields) == 8 and fields[0].rstrip(":").isdigit():
            yield fields


def audit(elf_symbols, map_text, archive_symbols):
    defined = {s[7]: s for s in symbols(elf_symbols) if s[6] != "UND"}
    for hook in HOOKS:
        entry = defined.get(hook)
        if not entry or entry[3:5] != ["FUNC", "GLOBAL"]:
            raise ValueError(f"missing strong budget hook: {hook}")
    if "Discarded input sections" not in map_text:
        raise ValueError("unrecognized linker map: missing archive selection boundary")
    selected_text = map_text.split("Discarded input sections", 1)[0]
    if not re.search(r"^\S*libpxa\.a\(pxa_codec_memory\.cc\.obj\)", selected_text, re.M):
        raise ValueError("budget adapter object was not selected")
    report = {}
    object_imports = {}
    internal_functions = set()
    for archive, listing in archive_symbols.items():
        selected = set(re.findall(r"^\S*" + re.escape(archive) + r"\(([^)]+)\)",
                                  selected_text, re.M))
        if not selected:
            raise ValueError(f"no selected objects from {archive}")
        found = set()
        for block in re.split(r"(?m)^File: ", listing)[1:]:
            match = re.search(r"\(([^)]+)\)$", block.splitlines()[0])
            if not match or match[1] not in selected:
                continue
            member = match[1]
            found.add(member)
            internal_functions.update(s[7] for s in symbols(block)
                                      if s[3] == "FUNC" and s[4] in {"GLOBAL", "WEAK"} and s[6] != "UND"
                                      and not s[7].startswith("media_lib_")
                                      and s[7] not in {"malloc", "calloc", "realloc", "free"})
            imports = {s[7] for s in symbols(block) if s[6] == "UND"}
            object_imports[f"{archive}({member})"] = imports
        if found != selected:
            raise ValueError(f"missing symbol tables: {sorted(selected - found)}")
    seen_hooks = set()
    for obj, imports in object_imports.items():
        memory = {s for s in imports if MEMORY.search(s) or s.startswith("media_lib_")}
        # Codec-local helpers (e.g. CELT bit allocation, Vorbis floor teardown)
        # are covered by the same walk. Their imports are checked separately.
        bypasses = memory - HOOKS - internal_functions
        if bypasses:
            raise ValueError(f"unaudited allocation imports in {obj}: {sorted(bypasses)}")
        seen_hooks.update(memory & HOOKS)
        report[obj] = sorted(memory & HOOKS)
    if seen_hooks != HOOKS:
        raise ValueError(f"unexpected codec path: observed hooks {sorted(seen_hooks)}")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--map", type=Path, required=True)
    parser.add_argument("--codec-dir", type=Path, required=True)
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    try:
        expected_archives = ARCHIVES_BY_TARGET.get(args.codec_dir.name)
        if expected_archives is None:
            raise ValueError(f"unsupported codec target: {args.codec_dir.name}")
        listings = {}
        for name, expected in expected_archives.items():
            archive = args.codec_dir / name
            if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
                raise ValueError(f"{archive}: unaudited codec binary; review before updating hashes")
            listings[name] = subprocess.check_output([args.readelf, "-sW", str(archive)], text=True)
        report = audit(subprocess.check_output([args.readelf, "-sW", str(args.elf)], text=True),
                       args.map.read_text(), listings)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"codec memory audit failed: {error}\n")
    args.report.write_text(json.dumps({"archive_sha256": expected_archives, "selected_imports": report}, indent=2) + "\n")
    print(f"Codec memory audit passed: {len(report)} selected objects, four strong budget hooks")


if __name__ == "__main__":
    main()
