#!/usr/bin/env python3
"""Compare independent PXA block codecs on signed package inventories.

This reads expanded packages without changing them. Write --output under
local/artifacts; sizes include per-block authentication index overhead.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "deps/pxa-system/tools/package"))
from build_pxa_container import Lz4, manifest_files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("packages", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    lz4 = Lz4()
    rows = []
    for package in args.packages:
        manifest = (package / "manifest.pxm").read_bytes()
        files = manifest_files(manifest)
        for block in (4096, 16384, 32768):
            for codec in ("lz4", "deflate"):
                total, packed, chunks, stored_blocks = 0, 64 + len(manifest) + 216, 0, 0
                begin = time.monotonic()
                for relative, size, digest in files:
                    contents = (package / relative).read_bytes()
                    assert len(contents) == size and hashlib.sha256(contents).digest() == digest
                    packed += 16
                    for at in range(0, size, block):
                        raw = contents[at:at + block]
                        if codec == "lz4":
                            encoded = lz4.compress(raw)
                        else:
                            compressor = zlib.compressobj(level=9, wbits=-15)
                            encoded = compressor.compress(raw) + compressor.flush()
                            assert zlib.decompress(encoded, wbits=-15) == raw
                        stored_blocks += len(encoded) >= len(raw)
                        packed += min(len(encoded), len(raw)) + 4 + 48
                        chunks += 1
                    total += size
                row = dict(package=str(package), codec=codec, block_bytes=block, raw_bytes=total,
                           container_bytes=packed, ratio=packed / total, chunks=chunks,
                           stored_blocks=stored_blocks, encode_seconds=time.monotonic() - begin)
                rows.append(row)
                print(json.dumps(row), flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(rows, indent=2) + "\n")


if __name__ == "__main__":
    main()
